#pragma once
#include <esp_http_server.h>
#include <cJSON.h>
#include <cstdint>
namespace gateway {
void start();
esp_err_t apply_pending_restore();
const char *restore_state();
esp_err_t diagnostics(httpd_req_t*);
esp_err_t settings_get(httpd_req_t*);
esp_err_t name_save(httpd_req_t*);
esp_err_t thread_channel_save(httpd_req_t*);
void append_name(cJSON *system);
constexpr size_t gateway_name_capacity = 49;
void read_gateway_name(char (&name)[gateway_name_capacity]);
esp_err_t certificate_save(httpd_req_t*);
esp_err_t cloud_map(httpd_req_t*);
esp_err_t backup(httpd_req_t*);
esp_err_t restore(httpd_req_t*);
esp_err_t update(httpd_req_t*);
esp_err_t update_rcp(httpd_req_t*);
void apply_pending_rcp_update();
void rcp_ota_status(char (&out)[96]);
bool rcp_ota_verification_pending();
void rcp_ota_observe(const char *version);
esp_err_t rollback(httpd_req_t*);
esp_err_t mqtt_get(httpd_req_t*);
esp_err_t mqtt_save(httpd_req_t*);
esp_err_t relay_control(httpd_req_t*);
esp_err_t relay_enqueue(uint64_t,uint16_t,bool,bool from_supla=false);
void mqtt_tick();
void wake_gateway_worker();
void mqtt_init();
// Health gate for a freshly installed (PENDING_VERIFY) image; see gateway_update.cpp.
void ota_health(bool core_ok,bool service_ok,bool c6_ready);
// Non-fatal startup faults (corrupt NVS data, failed restore...). The gateway keeps
// running with the panel and Wi-Fi so the user can restore a backup or install
// another version instead of looping in abort(). A faulted boot never confirms OTA.
void note_boot_fault(const char *stage,esp_err_t err);
bool boot_faulted();
esp_err_t reply(httpd_req_t*,int,const char*);
cJSON* body_json(httpd_req_t*,size_t limit);
bool mutation_busy();
void mutation_done();
void restart_later();
}
