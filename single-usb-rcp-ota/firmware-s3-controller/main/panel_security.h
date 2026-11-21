#pragma once
#include <esp_err.h>
#include <esp_http_server.h>
// Called once with Wi-Fi PHY running, before starting the HTTP log capture.
esp_err_t panel_security_init();
const char *panel_security_ap_password();
const char *panel_security_certificate();
const char *panel_security_private_key();
bool panel_security_authorize(httpd_req_t *req, bool mutation, bool plain_body);

bool panel_security_setup_required(httpd_req_t *req);
esp_err_t panel_security_change_password(const char *password);

// Accept only a SAN server certificate matching the existing device private key.
bool panel_security_certificate_matches(const char *certificate,const char *key);
