extern "C" {
#include "webrtc.h"
#include "device/buffer.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "device/device.h"
#include "output/output.h"
#include "util/http/http.h"
#include "util/opts/log.h"
#include "util/opts/fourcc.h"
#include "util/opts/control.h"
#include "util/opts/opts.h"
#include "device/buffer.h"
};

#include "util/opts/helpers.hh"
#include "util/http/json.hh"

#define DEFAULT_PING_INTERVAL_US        (1 * 1000 * 1000)
#define DEFAULT_PONG_INTERVAL_US        (30 * 1000 * 1000)
#define DEFAULT_TIMEOUT_S               (60 * 60)

#ifdef USE_LIBDATACHANNEL

#include <inttypes.h>
#include <string>
#include <memory>
#include <optional>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <set>
#include <vector>
#include <rtc/peerconnection.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/h264rtppacketizer.hpp>
#include <rtc/h264packetizationhandler.hpp>
#include <rtc/rtcpnackresponder.hpp>
#include <mosquitto.h>
#include <unordered_map>

#include "third_party/magic_enum/include/magic_enum.hpp"

using namespace std::chrono_literals;

class Client;
struct ClientTrackData;

static webrtc_options_t *webrtc_options;
static std::set<std::shared_ptr<Client> > webrtc_clients;
static std::mutex webrtc_clients_lock;
static struct mosquitto *mqtt_connection;
static std::string mqtt_sdp_base_topic;
static std::string mqtt_ice_base_topic;
static std::unordered_map<std::string, std::shared_ptr<Client>> mqtt_client_map;
static std::shared_ptr<Client> signaling_client;

static const auto webrtc_client_lock_timeout = 3 * 1000ms;
static const auto webrtc_client_max_json_body = 10 * 1024;
static const auto webrtc_client_video_payload_type = 102; // H264
static rtc::Configuration webrtc_configuration = {
  // .iceServers = { rtc::IceServer("stun:stun.l.google.com:19302") },
  .disableAutoNegotiation = true
};
static void signaling_connect();
static std::shared_ptr<ClientTrackData> webrtc_add_video(const std::shared_ptr<rtc::PeerConnection> pc, const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid);
static void webrtc_parse_ice_servers(rtc::Configuration &config, const nlohmann::json &message);
static std::shared_ptr<Client> webrtc_peer_connection(rtc::Configuration config, const nlohmann::json &message);

struct ClientTrackData
{
  std::shared_ptr<rtc::Track> track;
	std::shared_ptr<rtc::RtcpSrReporter> sender;

  void startStreaming()
  {
    double currentTime_s = get_monotonic_time_us(NULL, NULL)/(1000.0*1000.0);
    sender->rtpConfig->setStartTime(currentTime_s, rtc::RtpPacketizationConfig::EpochStart::T1970);
    sender->startRecording();
  }

  void sendTime()
  {
    double currentTime_s = get_monotonic_time_us(NULL, NULL)/(1000.0*1000.0);

    auto rtpConfig = sender->rtpConfig;
    uint32_t elapsedTimestamp = rtpConfig->secondsToTimestamp(currentTime_s);

    sender->rtpConfig->timestamp = sender->rtpConfig->startTimestamp + elapsedTimestamp;
    auto reportElapsedTimestamp = sender->rtpConfig->timestamp - sender->previousReportedTimestamp;
    if (sender->rtpConfig->timestampToSeconds(reportElapsedTimestamp) > 1) {
      sender->setNeedsToReport();
    }
  }

  bool wantsFrame() const
  {
    if (!track)
      return false;

    return track->isOpen();
  }
};

class Client
{
public:
  Client(std::shared_ptr<rtc::PeerConnection> pc_)
    : pc(pc_)
  {
    id.resize(20);
    for (auto & c : id) {
      c = 'a' + (rand() % 26);
    }
    id = "rtc-" + id;
    name = strdup(id.c_str());
    last_ping_us = last_pong_us = get_monotonic_time_us(NULL, NULL);
  }

  ~Client()
  {
    free(name);
  }

  void close()
  {
    if (pc)
      pc->close();
  }

  bool wantsKeepAlive()
  {
    return dc_keepAlive != nullptr;
  }

  bool keepAlive()
  {
    uint64_t now_us = get_monotonic_time_us(NULL, NULL);

    if (deadline_us > 0 && now_us > deadline_us) {
      LOG_INFO(this, "The stream reached the deadline.");
      return false;
    }
  
    if (!dc_keepAlive)
      return true;

    if (dc_keepAlive->isOpen() && now_us - last_ping_us >= DEFAULT_PING_INTERVAL_US) {
      LOG_DEBUG(this, "Checking if client still alive.");
      dc_keepAlive->send("ping");
      last_ping_us = now_us;
    }

    if (now_us - last_pong_us >= DEFAULT_PONG_INTERVAL_US) {
      LOG_INFO(this, "No heartbeat from client.");
      return false;
    }

    return true;
  }

  bool wantsFrame() const
  {
    if (!pc || !video)
      return false;
    if (pc->state() != rtc::PeerConnection::State::Connected)
      return false;
    return video->wantsFrame();
  }

  std::string client_id;

  void pushFrame(buffer_t *buf)
  {
    if (!video || !video->track) {
      return;
    }

    if (!had_key_frame) {
      had_key_frame = buf->flags.is_keyframe;
    }

    if (!had_key_frame) {
      if (!requested_key_frame) {
        device_video_force_key(buf->buf_list->dev);
        requested_key_frame = true;
      }
      return;
    }

    rtc::binary data((std::byte*)buf->start, (std::byte*)buf->start + buf->used);
    video->sendTime();
    video->track->send(data);
  }

  void describePeerConnection(nlohmann::json &message)
  {
    nlohmann::json ice_servers = nlohmann::json::array();

    for (const auto &ice_server : pc->config()->iceServers) {
      nlohmann::json json;

      std::string url;

      if (ice_server.type == rtc::IceServer::Type::Turn) {
        json["username"] = ice_server.username;
        json["credential"] = ice_server.password;
        url = ice_server.relayType == rtc::IceServer::RelayType::TurnTls ? "turns:" : "turn:";
      } else {
        url = "stun:";
      }

      url += ice_server.hostname + ":" + std::to_string(ice_server.port);
      json["urls"] = url;

      ice_servers.push_back(json);
    }

    message["iceServers"] = ice_servers;
  }

public:
  char *name = NULL;
  std::string id;
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::DataChannel> dc_keepAlive;
  std::shared_ptr<ClientTrackData> video;
  std::mutex lock;
  std::condition_variable wait_for_complete;
  std::vector<rtc::Candidate> pending_remote_candidates;
  bool has_set_sdp_answer = false;
  bool had_key_frame = false;
  bool requested_key_frame = false;
  uint64_t last_ping_us = 0;
  uint64_t last_pong_us = 0;
  uint64_t deadline_us = 0;
};

std::shared_ptr<Client> webrtc_find_client(std::string id)
{
  std::unique_lock lk(webrtc_clients_lock);
  for (auto client : webrtc_clients) {
    if (client && client->id == id) {
      return client;
    }
  }

  return std::shared_ptr<Client>();
}

void webrtc_remove_client(const std::shared_ptr<Client> &client, const char *reason)
{
  std::unique_lock lk(webrtc_clients_lock);
  webrtc_clients.erase(client);
  if (!client->client_id.empty())
    mqtt_client_map.erase(client->client_id);
  LOG_INFO(client.get(), "Client removed: %s.", reason);
}

static std::string mqtt_get_topic(const std::string &topic, const std::string &client_id = "")
{
  std::string result;
  if (webrtc_options->mqtt_uid[0]) {
    result = std::string(webrtc_options->mqtt_uid) + "/";
  }
  result += topic;
  if (!client_id.empty())
    result += "/" + client_id;
  return result;
}

static std::string mqtt_get_client_id(const std::string &topic, const std::string &base)
{
  size_t start = base.size() + 1;
  size_t end = topic.find('/', start);
  if (end == std::string::npos)
    return std::string();
  return topic.substr(start, end - start);
}

static void signaling_send(const nlohmann::json &message)
{
  if (!mqtt_connection || !signaling_client || signaling_client->client_id.empty())
    return;

  std::string payload = message.dump();
  std::string topic;
  auto type = message.value("type", std::string());
  if (type == "answer")
    topic = mqtt_get_topic("sdp", signaling_client->client_id);
  else if (type == "candidate")
    topic = mqtt_get_topic("ice", signaling_client->client_id);
  else
    return;

  mosquitto_publish(mqtt_connection, NULL, topic.c_str(), payload.size(), payload.c_str(), 1, false);
}

static void signaling_handle_offer(const std::string &client_id, const nlohmann::json &message)
{
  auto offer = rtc::Description(message["sdp"].get<std::string>(), message["type"].get<std::string>());
  auto config = webrtc_configuration;
  webrtc_parse_ice_servers(config, message);
  auto client = webrtc_peer_connection(config, message);
  client->client_id = client_id;
  mqtt_client_map[client_id] = client;

  try {
    client->video = webrtc_add_video(client->pc, webrtc_client_video_payload_type, rand(), "video", client->id);
    {
      std::unique_lock lock(client->lock);
      client->pc->setRemoteDescription(offer);
      client->has_set_sdp_answer = true;
      client->pc->setLocalDescription();
      client->wait_for_complete.wait_for(lock, webrtc_client_lock_timeout);
    }

    if (client->pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
      auto description = client->pc->localDescription();
      nlohmann::json answer;
      answer["type"] = description->typeString();
      answer["sdp"] = std::string(description.value());
      signaling_client = client;
      signaling_send(answer);
      LOG_VERBOSE(client.get(), "Local SDP Answer: %s", std::string(answer["sdp"]).c_str());
    }
  } catch(const std::exception &e) {
    webrtc_remove_client(client, e.what());
  }
}

static void mqtt_on_connect(struct mosquitto *mosq, void *, int result)
{
  if (result != 0) {
    LOG_INFO(NULL, "MQTT connect failed: %s", mosquitto_strerror(result));
    return;
  }
  mosquitto_subscribe(mosq, nullptr, (mqtt_sdp_base_topic + "/+/offer").c_str(), 0);
  mosquitto_subscribe(mosq, nullptr, (mqtt_ice_base_topic + "/+/offer").c_str(), 0);
  LOG_INFO(NULL, "Connected to MQTT broker");
}

static void mqtt_on_message(struct mosquitto *mosq, void *, const struct mosquitto_message *msg)
{
  if (!msg->payload)
    return;

  std::string topic(msg->topic);
  std::string payload(static_cast<const char*>(msg->payload), msg->payloadlen);

  try {
    if (topic.rfind(mqtt_sdp_base_topic, 0) == 0) {
      std::string cid = mqtt_get_client_id(topic, mqtt_sdp_base_topic);
      auto j = nlohmann::json::parse(payload);
      signaling_handle_offer(cid, j);
    } else if (topic.rfind(mqtt_ice_base_topic, 0) == 0) {
      std::string cid = mqtt_get_client_id(topic, mqtt_ice_base_topic);
      auto it = mqtt_client_map.find(cid);
      if (it != mqtt_client_map.end()) {
        auto client = it->second;
        auto j = nlohmann::json::parse(payload);
        auto cand = rtc::Candidate(j["candidate"].get<std::string>(), j["sdpMid"].get<std::string>());
        std::unique_lock lock(client->lock);
        if (client->has_set_sdp_answer)
          client->pc->addRemoteCandidate(cand);
        else
          client->pending_remote_candidates.push_back(cand);
      }
    }
  } catch(const std::exception &e) {
    LOG_INFO(NULL, "Failed to parse MQTT message: %s", e.what());
  }
}

static void signaling_connect()
{
  if (!webrtc_options || !webrtc_options->mqtt_host[0])
    return;

  mosquitto_lib_init();
  mqtt_connection = mosquitto_new(NULL, true, NULL);
  if (!mqtt_connection) {
    LOG_INFO(NULL, "Failed to create mosquitto instance");
    return;
  }

  mosquitto_int_option(mqtt_connection, MOSQ_OPT_PROTOCOL_VERSION, MQTT_PROTOCOL_V5);
  if (webrtc_options->mqtt_port == 8883) {
      mosquitto_int_option(mqtt_connection, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
  }

  if (webrtc_options->mqtt_username[0])
    mosquitto_username_pw_set(mqtt_connection, webrtc_options->mqtt_username, webrtc_options->mqtt_password);

  mosquitto_connect_callback_set(mqtt_connection, mqtt_on_connect);
  mosquitto_message_callback_set(mqtt_connection, mqtt_on_message);

  mqtt_sdp_base_topic = mqtt_get_topic("sdp");
  mqtt_ice_base_topic = mqtt_get_topic("ice");

  int rc = mosquitto_connect_async(mqtt_connection, webrtc_options->mqtt_host, webrtc_options->mqtt_port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    LOG_INFO(NULL, "MQTT connect failed: %s", mosquitto_strerror(rc));
    return;
  }

  LOG_INFO(NULL, "MQTT connected successfully");

  mosquitto_loop_start(mqtt_connection);
}

static std::shared_ptr<ClientTrackData> webrtc_add_video(const std::shared_ptr<rtc::PeerConnection> pc, const uint8_t payloadType, const uint32_t ssrc, const std::string cname, const std::string msid)
{
  auto video = rtc::Description::Video(cname, rtc::Description::Direction::SendOnly);
  video.addH264Codec(payloadType);
  video.setBitrate(1000);
  std::string stream_id = msid.empty() ? "stream-" + std::to_string(ssrc) : msid;
  video.addSSRC(ssrc, cname, stream_id, cname);
  auto track = pc->addTrack(video, {stream_id});
  auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::H264RtpPacketizer::defaultClockRate);
  auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::H264RtpPacketizer::Separator::LongStartSequence, rtpConfig);
  auto h264Handler = std::make_shared<rtc::H264PacketizationHandler>(packetizer);
  auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
  h264Handler->addToChain(srReporter);
  auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
  h264Handler->addToChain(nackResponder);
  track->setMediaHandler(h264Handler);
  return std::shared_ptr<ClientTrackData>(new ClientTrackData{track, srReporter});
}

static void webrtc_parse_ice_servers(rtc::Configuration &config, const nlohmann::json &message)
{
  auto ice_servers = message.find("iceServers");
  if (ice_servers == message.end() || !ice_servers->is_array())
    return;

  if (webrtc_options->disable_client_ice) {
    LOG_VERBOSE(NULL, "ICE server from SDP request ignored due to `disable_client_ice`: %s",
      ice_servers->dump().c_str());
    return;
  }

  for (const auto& ice_server : *ice_servers) {
    try {
      auto urls = ice_server["urls"];

      // convert non array to array
      if (!urls.is_array()) {
        urls = nlohmann::json::array();
        urls.push_back(ice_server["urls"]);
      }

      for (const auto& url : urls) {
        auto iceServer = rtc::IceServer(url.get<std::string>());
        if (iceServer.type == rtc::IceServer::Type::Turn) {
          if (ice_server.contains("username"))
            iceServer.username = ice_server["username"].get<std::string>();
          if (ice_server.contains("credential"))
            iceServer.password = ice_server["credential"].get<std::string>();
        }

        config.iceServers.push_back(iceServer);
        LOG_VERBOSE(NULL, "Added ICE server from SDP request json: %s", url.dump().c_str());
      }

    } catch (nlohmann::detail::exception &e) {
      LOG_VERBOSE(NULL, "Failed to parse ICE server: %s: %s",
        ice_server.dump().c_str(), e.what());
    }
  }
}

static std::shared_ptr<Client> webrtc_peer_connection(rtc::Configuration config, const nlohmann::json &message)
{
  webrtc_parse_ice_servers(config, message);

  auto pc = std::make_shared<rtc::PeerConnection>(config);
  auto client = std::make_shared<Client>(pc);
  auto wclient = std::weak_ptr(client);

  if (message.value("keepAlive", false)) {
    LOG_INFO(client.get(), "Client supports Keep-Alives.");

    client->dc_keepAlive = pc->createDataChannel("keepalive");

    client->dc_keepAlive->onOpen([wclient]() {
      if(auto client = wclient.lock()) {
        LOG_DEBUG(client.get(), "data channel onOpen");
      }
    });

    client->dc_keepAlive->onMessage([wclient](auto message) {
      auto client = wclient.lock();
      if(client && std::holds_alternative<rtc::string>(message)) {
        LOG_DEBUG(client.get(), "data channel onMessage: %s", std::get<std::string>(message).c_str());
        client->last_pong_us = get_monotonic_time_us(NULL, NULL);
      }
    });
  } else {
    LOG_INFO(client.get(), "Client does not support Keep-Alives. This might result in stale streams.");
  }

  int64_t timeout_s = message.value("timeout_s", DEFAULT_TIMEOUT_S);
  if (timeout_s > 0) {
    LOG_INFO(client.get(), "The stream will auto-close in %" PRId64 "s.", timeout_s);
    client->deadline_us = get_monotonic_time_us(NULL, NULL) + timeout_s * 1000 * 1000;
  }

  pc->onTrack([wclient](std::shared_ptr<rtc::Track> track) {
    if(auto client = wclient.lock()) {
      LOG_DEBUG(client.get(), "onTrack: %s", track->mid().c_str());
    }
  });

  pc->onLocalDescription([wclient](rtc::Description description) {
    if(auto client = wclient.lock()) {
      LOG_DEBUG(client.get(), "onLocalDescription: %s", description.typeString().c_str());
    }
  });

  pc->onLocalCandidate([wclient](rtc::Candidate candidate) {
    if(auto client = wclient.lock()) {
      nlohmann::json msg;
      msg["type"] = "candidate";
      msg["id"] = client->id;
      msg["candidate"] = candidate.candidate();
      msg["sdpMid"] = candidate.mid();
      signaling_client = client;
      signaling_send(msg);
    }
  });

  pc->onSignalingStateChange([wclient](rtc::PeerConnection::SignalingState state) {
    if(auto client = wclient.lock()) {
      LOG_DEBUG(client.get(), "onSignalingStateChange: %d", (int)state);
    }
  });

  pc->onStateChange([wclient](rtc::PeerConnection::State state) {
    if(auto client = wclient.lock()) {
      LOG_DEBUG(client.get(), "onStateChange: %d", (int)state);

      if(state == rtc::PeerConnection::State::Connected) {
        // Start streaming once the client is connected, to ensure a keyframe is sent to start the stream.
        std::unique_lock lock(client->lock);
        client->video->startStreaming();
      } else if (state == rtc::PeerConnection::State::Disconnected ||
        state == rtc::PeerConnection::State::Failed ||
        state == rtc::PeerConnection::State::Closed)
      {
        webrtc_remove_client(client, "stream closed");
      }
    }
  });

  pc->onGatheringStateChange([wclient](rtc::PeerConnection::GatheringState state) {
    if(auto client = wclient.lock()) {
      LOG_DEBUG(client.get(), "onGatheringStateChange: %d", (int)state);

      if (state == rtc::PeerConnection::GatheringState::Complete) {
        client->wait_for_complete.notify_all();
      }
    }
  });

  std::unique_lock lk(webrtc_clients_lock);
  webrtc_clients.insert(client);
  return client;
}

static bool webrtc_h264_needs_buffer(buffer_lock_t *buf_lock)
{
  std::unique_lock lk(webrtc_clients_lock);
  for (auto client : webrtc_clients) {
    if (client->wantsFrame())
      return true;
  }

  return false;
}

static void webrtc_h264_capture(buffer_lock_t *buf_lock, buffer_t *buf)
{
  std::set<std::shared_ptr<Client> > lost_clients;

  std::unique_lock lk(webrtc_clients_lock);
  for (auto client : webrtc_clients) {
    if (client->wantsFrame())
      client->pushFrame(buf);
    if (!client->keepAlive())
      lost_clients.insert(client);
  }
  lk.unlock();

  for (auto lost_client : lost_clients) {
    lost_client->close();
  }
}

static void http_webrtc_request(http_worker_t *worker, FILE *stream, const nlohmann::json &message)
{
  auto client = webrtc_peer_connection(webrtc_configuration, message);
  LOG_INFO(client.get(), "Stream requested.");

  client->video = webrtc_add_video(client->pc, webrtc_client_video_payload_type, rand(), "video", client->id);

  try {
    {
      std::unique_lock lock(client->lock);
      client->pc->setLocalDescription();
      client->wait_for_complete.wait_for(lock, webrtc_client_lock_timeout);
    }

    if (client->pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
      auto description = client->pc->localDescription();
      nlohmann::json message;
      message["id"] = client->id;
      message["type"] = description->typeString();
      message["sdp"] = std::string(description.value());
      message["keepAlive"] = client->wantsKeepAlive();
      client->describePeerConnection(message);
      http_write_response(stream, "200 OK", "application/json", message.dump().c_str(), 0);
      LOG_VERBOSE(client.get(), "Local SDP Offer: %s", std::string(message["sdp"]).c_str());
    } else {
      http_500(stream, "Not complete");
    }
  } catch(const std::exception &e) {
    http_500(stream, e.what());
    webrtc_remove_client(client, e.what());
  }
}

static void http_webrtc_answer(http_worker_t *worker, FILE *stream, const nlohmann::json &message)
{
  if (!message.contains("id") || !message.contains("sdp")) {
    http_400(stream, "no sdp or id");
    return;
  }

  if (auto client = webrtc_find_client(message["id"])) {
    LOG_INFO(client.get(), "Answer received.");
    LOG_VERBOSE(client.get(), "Remote SDP Answer: %s", std::string(message["sdp"]).c_str());

    try {
      auto answer = rtc::Description(std::string(message["sdp"]), std::string(message["type"]));
      client->pc->setRemoteDescription(answer);
      client->has_set_sdp_answer = true;

      // If there are any pending candidates that make it in before the answer request, add them now.
      for(auto const &candidate : client->pending_remote_candidates) {
        client->pc->addRemoteCandidate(candidate);
      }
      client->pending_remote_candidates.clear();
      http_write_response(stream, "200 OK", "application/json", "{}", 0);
    } catch(const std::exception &e) {
      http_500(stream, e.what());
      webrtc_remove_client(client, e.what());
    }
  } else {
    http_404(stream, "No client found");
  }
}

static void http_webrtc_offer(http_worker_t *worker, FILE *stream, const nlohmann::json &message)
{
  if (!message.contains("sdp")) {
    http_400(stream, "no sdp");
    return;
  }

  auto offer = rtc::Description(std::string(message["sdp"]), std::string(message["type"]));
  auto client = webrtc_peer_connection(webrtc_configuration, message);

  LOG_INFO(client.get(), "Offer received.");
  LOG_VERBOSE(client.get(), "Remote SDP Offer: %s", std::string(message["sdp"]).c_str());

  try {
    client->video = webrtc_add_video(client->pc, webrtc_client_video_payload_type, rand(), "video", client->id);

    {
      std::unique_lock lock(client->lock);
      client->pc->setRemoteDescription(offer);
      client->has_set_sdp_answer = true;
      client->pc->setLocalDescription();
      client->wait_for_complete.wait_for(lock, webrtc_client_lock_timeout);
    }

    if (client->pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete) {
      auto description = client->pc->localDescription();
      nlohmann::json message;
      message["type"] = description->typeString();
      message["sdp"] = std::string(description.value());
      message["keepAlive"] = client->wantsKeepAlive();
      client->describePeerConnection(message);
      http_write_response(stream, "200 OK", "application/json", message.dump().c_str(), 0);

      LOG_VERBOSE(client.get(), "Local SDP Answer: %s", std::string(message["sdp"]).c_str());
    } else {
      http_500(stream, "Not complete");
    }
  } catch(const std::exception &e) {
    http_500(stream, e.what());
    webrtc_remove_client(client, e.what());
  }
}

static void http_webrtc_remote_candidate(http_worker_t *worker, FILE *stream, const nlohmann::json &message)
{
  if (!message.contains("candidates") || !message.contains("id") || !message["candidates"].is_array()) {
    http_400(stream, "candidates is not array or no id");
    return;
  }

  auto client = webrtc_find_client(message["id"]);
  if (!client) {
    http_404(stream, "No client found");
    return;
  }

  for (auto const & entry : message["candidates"]) {
    try {
      auto remoteCandidate = rtc::Candidate(
        entry["candidate"].get<std::string>(),
        entry["sdpMid"].get<std::string>());
  
      std::unique_lock lock(client->lock);
      // The ICE candidate http requests can race the sdp answer http request and win. But it's invalid to set the ICE
      // candidates before the SDP answer is set.
      if (client->has_set_sdp_answer) {
        client->pc->addRemoteCandidate(remoteCandidate);
      } else {
        client->pending_remote_candidates.push_back(remoteCandidate);
      }
    } catch (nlohmann::detail::exception &e) {
      http_400(stream, e.what());
      return;
    }
  }

  http_write_response(stream, "200 OK", "application/json", "{}", 0);
}

extern "C" void http_webrtc_offer(http_worker_t *worker, FILE *stream)
{
  auto message = http_parse_json_body(worker, stream, webrtc_client_max_json_body);

  if (!message.contains("type")) {
    http_400(stream, "missing 'type'");
    return;
  }

  std::string type = message["type"];

  LOG_DEBUG(worker, "Recevied: '%s'", type.c_str());

  if (type == "request") {
    http_webrtc_request(worker, stream, message);
  } else if (type == "answer") {
    http_webrtc_answer(worker, stream, message);
  } else if (type == "offer") {
    http_webrtc_offer(worker, stream, message);
  } else if (type == "remote_candidate") {
    http_webrtc_remote_candidate(worker, stream, message);
  } else {
    http_400(stream, (std::string("Not expected: " + type)).c_str());
  }
}

extern "C" int webrtc_server(webrtc_options_t *options)
{
  webrtc_options = options;

  for (const auto &ice_server : str_split(options->ice_servers, OPTION_VALUE_LIST_SEP_CHAR)) {
    webrtc_configuration.iceServers.push_back(rtc::IceServer(ice_server));
  }

  buffer_lock_register_check_streaming(&video_lock, webrtc_h264_needs_buffer);
  buffer_lock_register_notify_buffer(&video_lock, webrtc_h264_capture);

  signaling_connect();

  options->running = true;
  return 0;
}

#else // USE_LIBDATACHANNEL

extern "C" void http_webrtc_offer(http_worker_t *worker, FILE *stream)
{
  http_404(stream, NULL);
}

extern "C" int webrtc_server(webrtc_options_t *options)
{
  return 0;
}

#endif // USE_LIBDATACHANNEL
