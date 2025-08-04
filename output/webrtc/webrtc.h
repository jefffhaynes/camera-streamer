#pragma once

#include <stdio.h>
#include <stdint.h>

#define WEBRTC_OPTIONS_LENGTH 4096

typedef struct http_worker_s http_worker_t;

typedef struct webrtc_options_s {
  bool running;
  bool disabled;
  char ice_servers[WEBRTC_OPTIONS_LENGTH];
  bool disable_client_ice;
  char mqtt_host[WEBRTC_OPTIONS_LENGTH];
  unsigned int mqtt_port;
  char mqtt_username[WEBRTC_OPTIONS_LENGTH];
  char mqtt_password[WEBRTC_OPTIONS_LENGTH];
  char mqtt_uid[WEBRTC_OPTIONS_LENGTH];
} webrtc_options_t;

// WebRTC
void http_webrtc_offer(http_worker_t *worker, FILE *stream);
int webrtc_server(webrtc_options_t *options);
