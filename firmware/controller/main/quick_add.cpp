/*
 * quick_add.cpp - mini-panel WWW bramki Matter (S3 + OTBR + C6 RCP):
 * szybkie dodawanie urzadzen po kodzie QR ("MT:...") lub kodzie recznym.
 *
 * Jak to dziala:
 *  - kod QR / kod numeryczny jest walidowany parserem CHIP
 *    (chip::SetupPayload::FromStringRepresentation - obsluguje oba formaty),
 *  - dataset Thread bierzemy LOKALNIE z OTBR (otDatasetGetActiveTlvs),
 *  - commissioner probuje dodac urzadzenie przez Thread, Wi-Fi lub siec IP,
 *  - stan parowania (PASE / sukces / blad) aktualizuja callbacki pairing_command.
 *
 * Panel lokalny HTTPS; wszystkie trasy API wymagaja tokenu dostepu.
 */

#include "quick_add.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <new>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <esp_https_server.h>
#include "panel_security.h"
#include "input_validation.h"
#include "version.h"

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_controller_pairing_command.h>
#include <esp_matter_controller_client.h>
#include "matter_discovery.h"
#include "channel_proposal.h"
#include "gateway_services.h"
#include "matter_ota.h"
#include <esp_tls.h>
#include <mbedtls/ssl.h>
#include "mapping_nvs.h"
#include "device_telemetry.h"
#include <platform/CHIPDeviceLayer.h>

#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/dataset.h>
#include <openthread/link.h>
#include <openthread/thread.h>

#include <setup_payload/SetupPayload.h>

#include <cJSON.h>
#include <esp_system.h>

#include "wifi_manager.h"
#include "supla_bridge.h"
#include "supla_config.h"

// Osadzone pliki panelu (EMBED_TXTFILES w main/CMakeLists.txt).
// Uwaga: IDF generuje nazwy symboli z samej nazwy pliku (bez katalogow: "www/").
extern const uint8_t index_gzip_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_gzip_end[] asm("_binary_index_html_gz_end");
extern const uint8_t app_gzip_start[] asm("_binary_app_js_gz_start");
extern const uint8_t app_gzip_end[] asm("_binary_app_js_gz_end");
extern const uint8_t qr_gzip_start[] asm("_binary_jsQR_js_gz_start");
extern const uint8_t qr_gzip_end[] asm("_binary_jsQR_js_gz_end");
extern const uint8_t www_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t www_index_html_end[] asm("_binary_index_html_end");
extern const uint8_t theme_js_start[] asm("_binary_theme_js_start");
extern const uint8_t theme_js_end[] asm("_binary_theme_js_end");
extern const uint8_t gateway_js_start[] asm("_binary_gateway_js_start");
extern const uint8_t gateway_js_end[] asm("_binary_gateway_js_end");
extern const uint8_t www_app_js_start[] asm("_binary_app_js_start");
extern const uint8_t www_app_js_end[] asm("_binary_app_js_end");
extern const uint8_t www_jsqr_js_start[] asm("_binary_jsQR_js_start");
extern const uint8_t www_jsqr_js_end[] asm("_binary_jsQR_js_end");



static const char *TAG = "quick_add";

namespace {

// ---------------------------------------------------------------------------
// Stan parowania (wspolny miedzy zadaniami HTTP i watkiem Matter)
// ---------------------------------------------------------------------------

enum qa_state_t {
    QA_IDLE = 0,
    QA_STARTED,
    QA_COMMISSIONING,
    QA_REMOVING,
    QA_SUCCESS,
    QA_FAILED,
};

struct qa_status_t {
    qa_state_t state;
    uint64_t node_id;
    uint32_t started_ms;
    char message[192];
};

SemaphoreHandle_t s_lock = nullptr;
qa_status_t s_status = {QA_IDLE, 0, 0, "bezczynne"};
httpd_handle_t s_server = nullptr;
struct RequestOutcome {bool close=false;};

const char *state_name(qa_state_t state)
{
    switch (state) {
    case QA_STARTED: return "started";
    case QA_COMMISSIONING: return "commissioning";
    case QA_REMOVING: return "removing";
    case QA_SUCCESS: return "success";
    case QA_FAILED: return "failed";
    default: return "idle";
    }
}

void set_status(qa_state_t state, uint64_t node_id, const char *fmt, ...)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.state = state;
    s_status.node_id = node_id;
    if (state == QA_STARTED) {
        s_status.started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else if (state == QA_IDLE) {
        s_status.started_ms = 0;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status.message, sizeof(s_status.message), fmt, ap);
    va_end(ap);
    xSemaphoreGive(s_lock);
}

uint64_t current_node_id()
{
    uint64_t node = 0;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        node = s_status.node_id;
        xSemaphoreGive(s_lock);
    }
    return node;
}

// ---------------------------------------------------------------------------
// NVS - kolejny numer node id dla nowych urzadzen
// ---------------------------------------------------------------------------


#define MAX_DEVICES 64
#define LEGACY_MAX_DEVICES 32
struct device_entry_t {uint64_t node;
uint16_t vid;
uint16_t pid;
char name[40];};
struct DeviceRegistry {uint32_t version;
uint32_t count;
device_entry_t entries[MAX_DEVICES];};
struct DeviceRegistryLegacy32 {uint32_t version;
uint32_t count;
device_entry_t entries[LEGACY_MAX_DEVICES];};
EXT_RAM_BSS_ATTR device_entry_t s_devices[MAX_DEVICES]{};
size_t s_devices_count=0;
DRAM_ATTR StaticSemaphore_t s_devices_mutex_storage,s_devices_writer_storage;
SemaphoreHandle_t s_devices_mutex=nullptr,s_devices_writer=nullptr;
// Set when the stored registry could not be read at boot: the in-memory copy is
// then empty and must never overwrite the (possibly recoverable) NVS blob.
bool s_registry_readonly=false;
struct RegistryLock {SemaphoreHandle_t lock;
RegistryLock(SemaphoreHandle_t m):lock(m){xSemaphoreTake(lock,portMAX_DELAY);}~RegistryLock(){xSemaphoreGive(lock);}};
void devices_snapshot(DeviceRegistry *out) {
    RegistryLock lock(s_devices_mutex);
    memset(out,0,sizeof(*out));
    out->version=1;
    out->count=s_devices_count;
    memcpy(out->entries,s_devices,s_devices_count*sizeof(device_entry_t));
}
size_t devices_count(){RegistryLock lock(s_devices_mutex);
return s_devices_count;}
bool devices_contains(uint64_t node){RegistryLock lock(s_devices_mutex);
for(size_t i=0;i<s_devices_count;i++)if(s_devices[i].node==node)return true;
return false;}
esp_err_t devices_persist(const DeviceRegistry &snapshot) {
    if(s_registry_readonly)return ESP_ERR_INVALID_STATE;
    nvs_handle_t h;
    esp_err_t err=nvs_open("panel",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    if(snapshot.count<=LEGACY_MAX_DEVICES) {
        // Keep exact 32-entry binary layout in registry_v1 for seamless rollback to 0.3.2/0.3.3.
        auto legacy=std::unique_ptr<DeviceRegistryLegacy32>(new(std::nothrow)DeviceRegistryLegacy32{});
        if(!legacy){nvs_close(h);return ESP_ERR_NO_MEM;}
        legacy->version=1;
        legacy->count=snapshot.count;
        memcpy(legacy->entries,snapshot.entries,legacy->count*sizeof(device_entry_t));
        err=nvs_set_blob(h,"registry_v1",legacy.get(),sizeof(*legacy));
        if(err==ESP_OK) {
            esp_err_t del=nvs_erase_key(h,"registry_v2");
            if(del!=ESP_OK&&del!=ESP_ERR_NVS_NOT_FOUND)err=del;
        }
    } else {
        // Store compact blob (only active entries) in registry_v2 and drop registry_v1 to save 24 KiB NVS space.
        const size_t compact_bytes=offsetof(DeviceRegistry,entries)+snapshot.count*sizeof(device_entry_t);
        err=nvs_set_blob(h,"registry_v2",&snapshot,compact_bytes);
        if(err==ESP_OK) {
            esp_err_t del=nvs_erase_key(h,"registry_v1");
            if(del!=ESP_OK&&del!=ESP_ERR_NVS_NOT_FOUND)err=del;
        }
    }
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK){RegistryLock lock(s_devices_mutex);
    s_devices_count=snapshot.count;
    memcpy(s_devices,snapshot.entries,sizeof(s_devices));}
    return err;
}
bool valid_node_number(double node){return std::isfinite(node)&&node>=1&&node<=UINT32_MAX&&std::floor(node)==node;}
esp_err_t devices_load() {
    RegistryLock writer(s_devices_writer);
    auto snapshot=std::unique_ptr<DeviceRegistry>(new(std::nothrow)DeviceRegistry{});
    if(!snapshot)return ESP_ERR_NO_MEM;
    snapshot->version=1;
    nvs_handle_t h;
    esp_err_t err=nvs_open("panel",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(err!=ESP_OK)return err;
    size_t n=sizeof(*snapshot);
    err=nvs_get_blob(h,"registry_v2",snapshot.get(),&n);
    const size_t header_bytes=offsetof(DeviceRegistry,entries);
    bool loaded_blob=false;
    if(err==ESP_OK && n>=header_bytes && n<=sizeof(*snapshot)) {
        const size_t payload=n-header_bytes;
        if((n==sizeof(*snapshot) && snapshot->count<=MAX_DEVICES) ||
           (payload%sizeof(device_entry_t)==0 && snapshot->count==payload/sizeof(device_entry_t) && snapshot->count<=MAX_DEVICES)) {
            loaded_blob=true;
        }
    }
    if(!loaded_blob) {
        memset(snapshot.get(),0,sizeof(*snapshot));
        n=sizeof(*snapshot);
        err=nvs_get_blob(h,"registry_v1",snapshot.get(),&n);
        if(err==ESP_OK && (n==sizeof(DeviceRegistryLegacy32) || n==sizeof(*snapshot))) {
            const size_t max_for_blob=(n==sizeof(DeviceRegistryLegacy32))?LEGACY_MAX_DEVICES:MAX_DEVICES;
            if(snapshot->count<=max_for_blob)loaded_blob=true;
            else err=ESP_ERR_INVALID_STATE;
        } else if(err==ESP_OK) {
            err=ESP_ERR_INVALID_STATE;
        }
    }
    if(loaded_blob) {
        nvs_close(h);
        if(snapshot->version!=1||snapshot->count>MAX_DEVICES)return ESP_ERR_INVALID_STATE;
        for(size_t i=0;i<snapshot->count;i++) {
            auto &d=snapshot->entries[i];
            if(!d.node||d.node>UINT32_MAX||!memchr(d.name,0,sizeof(d.name))||!m2s::valid_name(d.name))return ESP_ERR_INVALID_STATE;
            for(size_t j=0;j<i;j++)if(snapshot->entries[j].node==d.node)return ESP_ERR_INVALID_STATE;
        }
        RegistryLock lock(s_devices_mutex);
        s_devices_count=snapshot->count;
        memcpy(s_devices,snapshot->entries,sizeof(s_devices));
        return ESP_OK;
    }
    if(err!=ESP_ERR_NVS_NOT_FOUND){nvs_close(h);
    return err;}
    n=0;
    err=nvs_get_str(h,"devices",nullptr,&n);
    if(err==ESP_ERR_NVS_NOT_FOUND){nvs_close(h);
    return ESP_OK;}
    if(err!=ESP_OK||n<2||n>16384){nvs_close(h);
    return ESP_ERR_INVALID_SIZE;}
    auto text=std::unique_ptr<char[]>(new(std::nothrow)char[n]);
    if(!text){nvs_close(h);
    return ESP_ERR_NO_MEM;}
    err=nvs_get_str(h,"devices",text.get(),&n);
    nvs_close(h);
    if(err!=ESP_OK)return err;
    cJSON *json=cJSON_Parse(text.get());
    cJSON *arr=json?cJSON_GetObjectItem(json,"devices"):nullptr;
    if(!cJSON_IsArray(arr)||cJSON_GetArraySize(arr)>MAX_DEVICES){cJSON_Delete(json);
    return ESP_ERR_INVALID_STATE;}
    snapshot->count=0;
    cJSON *item=nullptr;
    cJSON_ArrayForEach(item,arr) {
        auto *node=cJSON_GetObjectItem(item,"node");
        if(!cJSON_IsNumber(node)||!valid_node_number(node->valuedouble)){cJSON_Delete(json);
        return ESP_ERR_INVALID_STATE;}
        auto &d=snapshot->entries[snapshot->count];
        d={};
        d.node=(uint64_t)node->valuedouble;
        for(size_t j=0;j<snapshot->count;j++)if(snapshot->entries[j].node==d.node){cJSON_Delete(json);
        return ESP_ERR_INVALID_STATE;}
        auto *name=cJSON_GetObjectItem(item,"name");
        if(cJSON_IsString(name)&&m2s::valid_name(name->valuestring))snprintf(d.name,sizeof(d.name),"%s",name->valuestring);
        else snprintf(d.name,sizeof(d.name),"Urzadzenie %u",(unsigned)d.node);
        auto *vid=cJSON_GetObjectItem(item,"vid"),*pid=cJSON_GetObjectItem(item,"pid");
        if(cJSON_IsNumber(vid)&&vid->valuedouble>=0&&vid->valuedouble<=65535)d.vid=(uint16_t)vid->valuedouble;
        if(cJSON_IsNumber(pid)&&pid->valuedouble>=0&&pid->valuedouble<=65535)d.pid=(uint16_t)pid->valuedouble;
        ++snapshot->count;
    }
    cJSON_Delete(json);
    return devices_persist(*snapshot);
}
esp_err_t devices_change(uint64_t node,const char *name,bool remove) {
    RegistryLock writer(s_devices_writer);
    auto next=std::unique_ptr<DeviceRegistry>(new(std::nothrow)DeviceRegistry{});
    if(!next)return ESP_ERR_NO_MEM;
    devices_snapshot(next.get());
    size_t index=next->count;
    for(size_t i=0;i<next->count;i++)if(next->entries[i].node==node){index=i;
    break;}
    if(remove){if(index==next->count)return ESP_ERR_NOT_FOUND;
    for(size_t i=index;i+1<next->count;i++)next->entries[i]=next->entries[i+1];
    --next->count;
    next->entries[next->count]={};}
    else if(name){if(index==next->count)return ESP_ERR_NOT_FOUND;
    snprintf(next->entries[index].name,sizeof(next->entries[index].name),"%s",name);}
    else {if(index<next->count)return ESP_OK;
    if(next->count>=MAX_DEVICES)return ESP_ERR_NO_MEM;
    auto &d=next->entries[next->count++];
    d={};
    d.node=node;
    snprintf(d.name,sizeof(d.name),"Urzadzenie %u",(unsigned)node);}
    return devices_persist(*next);
}
esp_err_t reserve_next_id(uint32_t *out) {
    if(s_registry_readonly)return ESP_ERR_INVALID_STATE; // no pairing while the registry is unreadable
    RegistryLock writer(s_devices_writer);
    uint64_t high=0;
    esp_err_t err=supla_bridge_max_reserved_node(&high);
    if(err!=ESP_OK)return err;
    {RegistryLock lock(s_devices_mutex);
    if(s_devices_count>=MAX_DEVICES)return ESP_ERR_NO_MEM;
    for(size_t i=0;i<s_devices_count;i++)high=std::max(high,s_devices[i].node);}
    nvs_handle_t h;
    err=nvs_open("quick_add",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    uint32_t stored=1;
    err=nvs_get_u32(h,"next_id",&stored);
    if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
    if(high>=UINT32_MAX){nvs_close(h);return ESP_ERR_INVALID_STATE;}
    uint64_t candidate=std::max((uint64_t)stored,high+1);
    if(err==ESP_OK&&(candidate==0||candidate>=UINT32_MAX))err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK)err=nvs_set_u32(h,"next_id",(uint32_t)candidate+1);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK)*out=(uint32_t)candidate;
    return err;
}

// ---------------------------------------------------------------------------
// Bufor logu (zakladka "Log" w panelu)
// ---------------------------------------------------------------------------

#define LOG_BUF_SIZE 8192

char s_log_buf[LOG_BUF_SIZE];
size_t s_log_written = 0;
size_t s_log_pos = 0;
std::atomic<vprintf_like_t> s_log_orig{nullptr};
portMUX_TYPE s_log_lock=portMUX_INITIALIZER_UNLOCKED;

int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    char tmp[256];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (n > 0) {
        size_t len = (n < (int)sizeof(tmp)) ? (size_t)n : sizeof(tmp) - 1;
        portENTER_CRITICAL(&s_log_lock);
        const size_t first = std::min(len, (size_t)LOG_BUF_SIZE - s_log_pos);
        memcpy(s_log_buf + s_log_pos, tmp, first);
        if (len > first) {
            memcpy(s_log_buf, tmp + first, len - first);
        }
        s_log_pos = (s_log_pos + len) % LOG_BUF_SIZE;
        s_log_written = std::min((size_t)LOG_BUF_SIZE, s_log_written + len);
        portEXIT_CRITICAL(&s_log_lock);
    }
    auto original=s_log_orig.load();
    int ret = original ? original(fmt, ap2) : n;
    va_end(ap2);
    return ret;
}

void log_capture_start()
{
    if (!s_log_orig.load()) {
        s_log_orig.store(esp_log_set_vprintf(&log_hook));
    }
}

void log_tail(char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    portENTER_CRITICAL(&s_log_lock);
    size_t len = (s_log_written > LOG_BUF_SIZE) ? LOG_BUF_SIZE : s_log_written;
    size_t start = (s_log_written >= LOG_BUF_SIZE) ? s_log_pos : 0;
    if (len > out_size - 1) {
        start = (start + (len - (out_size - 1))) % LOG_BUF_SIZE;
        len = out_size - 1;
    }
    const size_t first = std::min(len, (size_t)LOG_BUF_SIZE - start);
    memcpy(out, s_log_buf + start, first);
    if (len > first) {
        memcpy(out + first, s_log_buf, len - first);
    }
    out[len] = 0;
    portEXIT_CRITICAL(&s_log_lock);
}

// ---------------------------------------------------------------------------
// Callbacki parowania
// ---------------------------------------------------------------------------

void on_pase_complete(CHIP_ERROR err)
{
    if (err == CHIP_NO_ERROR) {
        set_status(QA_COMMISSIONING, current_node_id(), "PASE OK - konfiguracja urzadzenia...");
        ESP_LOGI(TAG, "PASE complete");
    } else {
        set_status(QA_FAILED, current_node_id(), "Blad PASE: %s", chip::ErrorStr(err));
        ESP_LOGE(TAG, "PASE failed: %s", chip::ErrorStr(err));
    }
}

void on_commissioning_success(chip::ScopedNodeId peer_id)
{
    uint64_t node = peer_id.GetNodeId();
    uint32_t used = (uint32_t)node;
    if(devices_change(node,nullptr,false)!=ESP_OK){set_status(QA_FAILED,node,"Sparowano, ale zapis rejestru nie powiodl sie. Nie ponawiaj dodawania; sprawdz NVS.");
    return;}
    set_status(QA_SUCCESS, node, "Dodano urzadzenie (node %u).", (unsigned)used);
    ESP_LOGI(TAG, "Commissioning success, node=%u", (unsigned)used);
    const CHIP_ERROR scheduled = chip::DeviceLayer::PlatformMgr().ScheduleWork([](intptr_t id) {
        const auto node_id = static_cast<uint32_t>(id);
        if (quick_add_device_exists(node_id)) matter_discovery::start(node_id);
    }, static_cast<intptr_t>(used));
    if (scheduled != CHIP_NO_ERROR) ESP_LOGW(TAG, "Descriptor scan was not scheduled");
}

void on_commissioning_failure(chip::ScopedNodeId peer_id, CHIP_ERROR error,
                              chip::Controller::CommissioningStage stage,
                              std::optional<chip::Credentials::AttestationVerificationResult> info)
{
    (void)info;
    set_status(QA_FAILED, peer_id.GetNodeId(), "Nie udalo sie: %s (etap %d)",
               chip::ErrorStr(error), (int)stage);
    ESP_LOGE(TAG, "Commissioning failed: %s stage=%d", chip::ErrorStr(error), (int)stage);
}

void on_unpair_complete(chip::NodeId removed_node, CHIP_ERROR error)
{
    if (error == CHIP_NO_ERROR) {
        matter_ota::forget(removed_node);
        if(devices_change((uint64_t)removed_node,nullptr,true)!=ESP_OK){set_status(QA_FAILED,removed_node,"Usunieto z Matter, ale zapis rejestru nie powiodl sie.");
        return;}
        set_status(QA_IDLE, 0, "Urzadzenie usuniete (node %u).", (unsigned)removed_node);
        ESP_LOGI(TAG, "Unpair complete, node=%u", (unsigned)removed_node);
    } else {
        set_status(QA_FAILED,removed_node,"Nie udalo sie usunac urzadzenia.");
        ESP_LOGE(TAG, "Unpair failed: %s", chip::ErrorStr(error));
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi / Thread - informacje do statusu
// ---------------------------------------------------------------------------

struct wifi_info_t {
    bool connected;
    bool ap_mode;
    char ip[16];
    char ssid[33];
    int rssi;
};

wifi_info_t get_wifi_info()
{
    wifi_info_t info = {};
    strcpy(info.ip, "-");
    esp_netif_ip_info_t ip_info;
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        info.connected = true;
        snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            info.rssi = ap.rssi;
            snprintf(info.ssid, sizeof(info.ssid), "%s", (const char *)ap.ssid);
        }
    } else if (wifi_manager_in_ap_mode()) {
        info.ap_mode = true;
        info.connected = true;
        esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip_info.ip));
        }
        snprintf(info.ssid, sizeof(info.ssid), "%s", wifi_manager_ap_ssid());
    }
    return info;
}

struct thread_info_t {
    const char *role;
    uint8_t channel;
    bool has_dataset;
};

thread_info_t get_thread_info()
{
    thread_info_t info = {"disabled", 0, false};
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(30))) {
        info.role = "busy";
        return info;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance) {
        switch (otThreadGetDeviceRole(instance)) {
        case OT_DEVICE_ROLE_DETACHED: info.role = "detached";
        break;
        case OT_DEVICE_ROLE_CHILD: info.role = "child";
        break;
        case OT_DEVICE_ROLE_ROUTER: info.role = "router";
        break;
        case OT_DEVICE_ROLE_LEADER: info.role = "leader";
        break;
        default: info.role = "disabled";
        break;
        }
        info.channel = otLinkGetChannel(instance);
        otOperationalDatasetTlvs tlvs;
        if (otDatasetGetActiveTlvs(instance, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0) {
            info.has_dataset = true;
        }
    }
    esp_openthread_lock_release();
    return info;
}

bool fetch_dataset(uint8_t *buffer, size_t buffer_size, size_t *out_len)
{
    bool ok = false;
    otOperationalDatasetTlvs tlvs;
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        return false;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance && otDatasetGetActiveTlvs(instance, &tlvs) == OT_ERROR_NONE && tlvs.mLength > 0 &&
        tlvs.mLength <= buffer_size) {
        memcpy(buffer, tlvs.mTlvs, tlvs.mLength);
        *out_len = tlvs.mLength;
        ok = true;
    }
    esp_openthread_lock_release();
    return ok;
}

// ---------------------------------------------------------------------------
// Pomocnicze: JSON, dekodowanie formularza, normalizacja kodu
// ---------------------------------------------------------------------------

void url_decode_inplace(char *text)
{
    char *write = text;
    for (char *read = text; *read; read++) {
        if (*read == '+') {
            *write++ = ' ';
        } else if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            auto hex_value = [](char c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                return tolower((unsigned char)c) - 'a' + 10;
            };
            *write++ = (char)((hex_value(read[1]) << 4) | hex_value(read[2]));
            read += 2;
        } else {
            *write++ = *read;
        }
    }
    *write = 0;
}

// Przyjmuje "MT:..." (QR) albo kod reczny (z opcjonalnymi spacjami/kreskami).
bool normalize_code(const char *input, char *output, size_t output_size)
{
    std::string text(input ? input : "");
    while (!text.empty() && isspace((unsigned char)text.front())) {
        text.erase(text.begin());
    }
    while (!text.empty() && isspace((unsigned char)text.back())) {
        text.pop_back();
    }
    if (text.empty()) {
        return false;
    }

    size_t pos = text.find("MT:");
    if (pos == std::string::npos) {
        pos = text.find("mt:");
    }
    if (pos != std::string::npos) {
        size_t end = pos;
        while (end < text.size() && !isspace((unsigned char)text[end])) {
            end++;
        }
        std::string qr = "MT:" + text.substr(pos + 3, end - pos - 3);
        if (qr.size() < 8 || qr.size() >= output_size) {
            return false;
        }
        memcpy(output, qr.c_str(), qr.size() + 1);
        return true;
    }

    std::string digits;
    for (char c : text) {
        if (isdigit((unsigned char)c)) {
            digits.push_back(c);
        } else if (c == ' ' || c == '-') {
            continue;
        } else {
            return false;
        }
    }
    const bool valid_length = digits.size() == chip::kManualSetupShortCodeCharLength + 1 ||
                              digits.size() == chip::kManualSetupLongCodeCharLength + 1;
    if (!valid_length || digits.size() >= output_size) {
        return false;
    }
    memcpy(output, digits.c_str(), digits.size() + 1);
    return true;
}

esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

bool pairing_busy()
{
    bool busy = false;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        busy = (s_status.state == QA_STARTED || s_status.state == QA_COMMISSIONING || s_status.state == QA_REMOVING);
        xSemaphoreGive(s_lock);
    }
    return busy;
}

// ---------------------------------------------------------------------------
// GET /api/status - JSON: Wi-Fi / Thread / stan parowania
// ---------------------------------------------------------------------------

esp_err_t api_status_get(httpd_req_t *req)
{
    wifi_info_t wifi = get_wifi_info();
    thread_info_t thread = get_thread_info();

    qa_status_t status;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        status = s_status;
        xSemaphoreGive(s_lock);
    } else {
        status = {QA_IDLE, 0, 0, "bezczynne"};
    }

    uint32_t elapsed = 0;
    if (status.started_ms != 0 && (status.state == QA_STARTED || status.state == QA_COMMISSIONING)) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        elapsed = (now - status.started_ms) / 1000;
    }


    cJSON *root=cJSON_CreateObject();
    if(!root)return send_json(req,"500 Internal Server Error","{\"ok\":false}");
    auto *w=cJSON_AddObjectToObject(root,"wifi"),*t=cJSON_AddObjectToObject(root,"thread"),*p=cJSON_AddObjectToObject(root,"pairing"),*sys=cJSON_AddObjectToObject(root,"system"),*su=cJSON_AddObjectToObject(root,"supla");
    if(!w||!t||!p||!sys||!su){cJSON_Delete(root);
    return send_json(req,"500 Internal Server Error","{\"ok\":false}");}
    cJSON_AddBoolToObject(w,"connected",wifi.connected);
    cJSON_AddStringToObject(w,"mode",wifi.ap_mode?"ap":"sta");
    cJSON_AddStringToObject(w,"ip",wifi.ip);
    cJSON_AddStringToObject(w,"ssid",wifi.ssid);
    cJSON_AddNumberToObject(w,"rssi",wifi.rssi);
    cJSON_AddStringToObject(w,"config_error",wifi_manager_last_error()==ESP_OK?"":esp_err_to_name(wifi_manager_last_error()));
    cJSON_AddStringToObject(t,"role",thread.role);
    cJSON_AddNumberToObject(t,"channel",thread.channel);
    cJSON_AddBoolToObject(t,"dataset",thread.has_dataset);
    gateway::append_name(sys);
    cJSON_AddStringToObject(p,"state",state_name(status.state));
    cJSON_AddNumberToObject(p,"node",status.node_id);
    cJSON_AddStringToObject(p,"message",status.message);
    cJSON_AddNumberToObject(p,"elapsed",elapsed);
    cJSON_AddStringToObject(sys,"version",FW_VERSION);
    cJSON_AddNumberToObject(sys,"uptime",esp_timer_get_time()/1000000);
    cJSON_AddNumberToObject(sys,"heap",esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys,"devices",devices_count());
    cJSON_AddStringToObject(su,"state",supla_bridge_state_name());
    cJSON_AddNumberToObject(su,"code",supla_bridge_status_code());
    char *json=cJSON_PrintUnformatted(root);
    esp_err_t result=send_json(req,json?"200 OK":"500 Internal Server Error",json?json:"{\"ok\":false}");
    free(json);
    cJSON_Delete(root);
    return result;
}

int read_body_json(httpd_req_t *req,char *body,size_t size);

// ---------------------------------------------------------------------------
// POST /api/add - szybkie dodawanie: QR "MT:..." albo kod reczny
// ---------------------------------------------------------------------------

enum class PairNetwork { Thread, Wifi, OnNetwork };

esp_err_t pair_device(httpd_req_t *req, const char *value, PairNetwork network,
                      const char *ssid = nullptr, const char *password = nullptr)
{
    char code[256];
    if (!normalize_code(value, code, sizeof(code))) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"Nierozpoznany kod - wklej MT:... lub kod reczny (11 albo 21 cyfr).\"}");
    }

    std::vector<chip::SetupPayload> payloads;
    CHIP_ERROR parse_err = chip::SetupPayload::FromStringRepresentation(std::string(code), payloads);
    if (parse_err != CHIP_NO_ERROR) {
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"Kod nie przeszedl walidacji (suma kontrolna?).\"}");
    }

    if (pairing_busy()) {
        return send_json(req, "409 Conflict",
                         "{\"ok\":false,\"message\":\"Trwa juz parowanie - poczekaj na wynik.\"}");
    }

    uint8_t dataset[254];
    size_t dataset_len = 0;
    if (network == PairNetwork::Thread && !fetch_dataset(dataset, sizeof(dataset), &dataset_len)) {
        return send_json(req, "409 Conflict",
                         "{\"ok\":false,\"message\":\"Brak aktywnej sieci Thread (OTBR jeszcze nie wystartowal?).\"}");
    }

    if (network != PairNetwork::Thread && !get_wifi_info().connected) {
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Najpierw polacz bramke z siecia Wi-Fi.\"}");
    }

    uint32_t node_id=0;
    if(reserve_next_id(&node_id)!=ESP_OK)return send_json(req,"409 Conflict","{\"ok\":false,\"message\":\"Brak miejsca lub blad zapisu identyfikatora NVS.\"}");
    set_status(QA_STARTED, node_id, "Start parowania (node %u)...", node_id);
    ESP_LOGI(TAG, "Quick add: node=%u", node_id);

    esp_err_t err = ESP_ERR_INVALID_STATE;
    {
        using namespace esp_matter::controller;
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        auto *commissioner = matter_controller_client::get_instance().get_commissioner();
        if (commissioner && !commissioner->GetPairingDelegate()) {
            chip::Controller::CommissioningParameters params;
            auto discovery = chip::Controller::DiscoveryType::kAll;
            if (network == PairNetwork::Thread) {
                params.SetThreadOperationalDataset(chip::ByteSpan(dataset, dataset_len));
            } else if (network == PairNetwork::Wifi) {
                params.SetWiFiCredentials(chip::Controller::WiFiCredentials(
                    chip::ByteSpan(reinterpret_cast<const uint8_t *>(ssid), strlen(ssid)),
                    chip::ByteSpan(reinterpret_cast<const uint8_t *>(password), strlen(password))));
            } else {
                discovery = chip::Controller::DiscoveryType::kDiscoveryNetworkOnly;
            }
            commissioner->RegisterPairingDelegate(&pairing_command::get_instance());
            const CHIP_ERROR result = commissioner->PairDevice(node_id, code, params, discovery);
            if (result == CHIP_NO_ERROR) {
                err = ESP_OK;
            } else {
                commissioner->RegisterPairingDelegate(nullptr);
                err = ESP_FAIL;
                ESP_LOGW(TAG, "PairDevice rejected request: %s", chip::ErrorStr(result));
            }
        }
    }
    if (err != ESP_OK) {
        set_status(QA_FAILED, node_id, "Nie mozna rozpoczac parowania (err %d).", (int)err);
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"ok\":false,\"message\":\"Nie mozna rozpoczac parowania (err %d).\"}", (int)err);
        return send_json(req, "500 Internal Server Error", json);
    }

    char json[200];
    snprintf(json, sizeof(json),
             "{\"ok\":true,\"node\":%u,\"message\":\"Parowanie rozpoczete (node %u) - obserwuj status.\"}",
             node_id, node_id);
    return send_json(req, "200 OK", json);
}

esp_err_t api_add_post(httpd_req_t *req)
{
    char body[1024];
    if(read_body_json(req,body,sizeof(body))<0)return send_json(req,"400 Bad Request","{\"ok\":false,\"message\":\"Niepelne lub za dlugie dane.\"}");

    char *value = body;
    if (strncmp(body, "qr=", 3) == 0) {
        value = body + 3;
    } else if (strncmp(body, "code=", 5) == 0) {
        value = body + 5;
    }
    url_decode_inplace(value);

    return pair_device(req, value, PairNetwork::Thread);
}

esp_err_t api_matter_pair_post(httpd_req_t *req)
{
    char body[768];
    if (read_body_json(req, body, sizeof(body)) < 0)
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Niepelne dane.\"}");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithOpts(body, nullptr, true), cJSON_Delete);
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "code"));
    const char *mode = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "network"));
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "ssid"));
    const char *password = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(json.get(), "password"));
    if (!code || !mode)
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Podaj kod i tryb parowania.\"}");
    if (strcmp(mode, "thread") == 0) return pair_device(req, code, PairNetwork::Thread);
    if (strcmp(mode, "lan") == 0) return pair_device(req, code, PairNetwork::OnNetwork);
    if (strcmp(mode, "wifi") == 0 && m2s::valid_wifi(ssid, password))
        return pair_device(req, code, PairNetwork::Wifi, ssid, password);
    return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Niepoprawny tryb lub dane Wi-Fi.\"}");
}

// ---------------------------------------------------------------------------
// Pliki statyczne panelu (osadzone w firmware przez EMBED_TXTFILES)
// ---------------------------------------------------------------------------

bool accepts_gzip(httpd_req_t *req)
{
    char value[96];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value)) != ESP_OK) return false;
    for (char *item = value; item;) {
        char *next = strchr(item, ',');
        if (next) *next++ = 0;
        item += strspn(item, " \t");
        if (strncasecmp(item, "gzip", 4) == 0 && strspn(item + 4, " \t;") + 4 == strlen(item)) return true;
        if (strncasecmp(item, "gzip", 4) == 0 && (item[4] == ';' || item[4] == ' ' || item[4] == '\t')) {
            auto *param = strchr(item + 4, ';');
            if (!param) { item = next; continue; }
            param += 1 + strspn(param + 1, " \t");
            if (strncasecmp(param, "q=", 2) == 0 && strtod(param + 2, nullptr) > 0) return true;
        }
        item = next;
    }
    return false;
}

esp_err_t send_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *mime,
                        const uint8_t *gzip_start = nullptr, const uint8_t *gzip_end = nullptr)
{
    size_t len = (end > start) ? (size_t)(end - start) : 0;
    if (len > 0 && start[len - 1] == 0) {
        len--;
        // EMBED_TXTFILES doklada bajt NUL
    }
    const bool compressed = gzip_start && accepts_gzip(req);
    if (gzip_start) httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    if (compressed) {
        start = gzip_start;
        len = static_cast<size_t>(gzip_end - gzip_start);
    }
    char etag[64];
    snprintf(etag, sizeof(etag), "\"m2s-%s-%zu-%c\"", FW_VERSION, len, compressed ? 'g' : 'i');
    char candidate[64];
    const bool unchanged = httpd_req_get_hdr_value_str(req, "If-None-Match", candidate, sizeof(candidate)) == ESP_OK &&
                           strcmp(candidate, etag) == 0;
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    if (unchanged) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, nullptr, 0);
    }
    if (compressed) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_type(req, mime);
    return httpd_resp_send(req, reinterpret_cast<const char *>(start), len);
}

esp_err_t root_get(httpd_req_t *req)
{
    return send_embedded(req, www_index_html_start, www_index_html_end, "text/html; charset=utf-8", index_gzip_start, index_gzip_end);
}

esp_err_t app_js_get(httpd_req_t *req)
{
    return send_embedded(req, www_app_js_start, www_app_js_end, "application/javascript; charset=utf-8", app_gzip_start, app_gzip_end);
}

esp_err_t jsqr_js_get(httpd_req_t *req)
{
    return send_embedded(req, www_jsqr_js_start, www_jsqr_js_end, "application/javascript; charset=utf-8", qr_gzip_start, qr_gzip_end);
}

// ---------------------------------------------------------------------------
// Pomocnicze: czytanie ciala zadania (JSON) i restart
// ---------------------------------------------------------------------------

int read_body_json(httpd_req_t *req, char *body, size_t size)
{
    auto reject=[&](){static_cast<RequestOutcome*>(req->user_ctx)->close=true;
    return -1;};
    if(req->content_len==0 || req->content_len>=size)return reject();
    const size_t content_len=req->content_len;
    size_t received=0;
    const int64_t deadline=esp_timer_get_time()+5LL*1000000;
    while(received<content_len){
        if(esp_timer_get_time()>=deadline)return reject();
        int count=httpd_req_recv(req,body+received,content_len-received);
        if(count<=0)return reject();
        received+=(size_t)count;
    }
    body[received]=0;
    return (int)received;
}

double json_number(const cJSON *root, const char *key)
{
    cJSON *item = root ? cJSON_GetObjectItem(root, key) : nullptr;
    return cJSON_IsNumber(item) && valid_node_number(item->valuedouble) ? item->valuedouble : 0;
}

void restart_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

void schedule_restart(uint32_t delay_ms)
{
    static esp_timer_handle_t timer = nullptr;
    if (!timer) {
        esp_timer_create_args_t args = {};
        args.callback = &restart_timer_cb;
        args.name = "panel_restart";
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            esp_restart();
            return;
        }
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}

// ---------------------------------------------------------------------------
// /api/devices - lista sparowanych urzadzen
// ---------------------------------------------------------------------------

esp_err_t send_devices(httpd_req_t *req, bool include_telemetry)
{
    auto snapshot = std::unique_ptr<DeviceRegistry>(new(std::nothrow) DeviceRegistry{});
    if (!snapshot) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"message\":\"Brak pamieci.\"}");
    }
    devices_snapshot(snapshot.get());
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_sendstr_chunk(req, "{\"devices\":[");
    bool first = true;
    for (size_t i = 0; ret == ESP_OK && i < snapshot->count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddNumberToObject(o, "node", (double)snapshot->entries[i].node);
        cJSON_AddStringToObject(o, "name", snapshot->entries[i].name);
        cJSON_AddNumberToObject(o, "vid", snapshot->entries[i].vid);
        cJSON_AddNumberToObject(o, "pid", snapshot->entries[i].pid);
        if (include_telemetry) device_telemetry::append_json(snapshot->entries[i].node, o);
        char *item = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (!item) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        if (!first) {
            ret = httpd_resp_send_chunk(req, ",", 1);
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_sendstr_chunk(req, item);
            first = false;
        }
        cJSON_free(item);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(req, "]}");
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, nullptr, 0);
    }
    return ret;
}

esp_err_t api_devices_get(httpd_req_t *req)
{
    return send_devices(req, true);
}

esp_err_t api_device_list_get(httpd_req_t *req)
{
    return send_devices(req, false);
}

esp_err_t api_device_rename_post(httpd_req_t *req)
{
    char body[256];
    if (read_body_json(req, body, sizeof(body)) < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Brak danych.\"}");
    }
    cJSON *root = cJSON_Parse(body);
    uint64_t node = (uint64_t)json_number(root, "node");
    cJSON *name = root ? cJSON_GetObjectItem(root, "name") : nullptr;
    esp_err_t rc=ESP_ERR_INVALID_ARG;
    if(node && cJSON_IsString(name) && m2s::valid_name(name->valuestring))rc=devices_change(node,name->valuestring,false);
    if (root) {
        cJSON_Delete(root);
    }
    return rc==ESP_OK ? send_json(req,"200 OK","{\"ok\":true}") : send_json(req,rc==ESP_ERR_INVALID_ARG?"400 Bad Request":rc==ESP_ERR_NOT_FOUND?"404 Not Found":"500 Internal Server Error","{\"ok\":false,\"message\":\"Nie zapisano nazwy: sprawdz 1-39 bajtow UTF-8, znaki kontrolne, numer node i NVS.\"}");
}

esp_err_t api_discovery_get(httpd_req_t *req)
{
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> snapshot(matter_discovery::snapshot_json(), cJSON_Delete);
    if (!snapshot || !quick_add_device_exists(static_cast<uint64_t>(json_number(snapshot.get(), "node"))))
        return send_json(req, "200 OK", "{\"state\":\"idle\",\"endpoints\":[]}");
    char *body = cJSON_PrintUnformatted(snapshot.get());
    const esp_err_t result = send_json(req, body ? "200 OK" : "500 Internal Server Error",
                                      body ? body : "{\"ok\":false}");
    cJSON_free(body);
    return result;
}

esp_err_t api_discovery_post(httpd_req_t *req)
{
    char body[128];
    if (read_body_json(req, body, sizeof(body)) < 0)
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> json(cJSON_ParseWithOpts(body, nullptr, true), cJSON_Delete);
    const double value = json_number(json.get(), "node");
    if (!std::isfinite(value) || value < 1 || value > UINT32_MAX || value != floor(value))
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    const auto node = static_cast<uint32_t>(value);
    if (!quick_add_device_exists(node))
        return send_json(req, "404 Not Found", "{\"ok\":false}");
    esp_err_t err;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        err = matter_discovery::start(node);
    }
    if (err == ESP_ERR_INVALID_STATE)
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Trwa odczyt mozliwosci. Sprobuj za chwile.\"}");
    if (err != ESP_OK)
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"message\":\"Nie uruchomiono odczytu.\"}");
    return send_json(req, "200 OK", "{\"ok\":true}");
}

esp_err_t mapping_error(httpd_req_t *req, m2s::mapping::Error error)
{
    using m2s::mapping::Error;
    switch (error) {
      case Error::NotFound:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Brak nowych funkcji potwierdzonych odczytem.\"}");
      case Error::Discovery:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Najpierw wykonaj swiezy, kompletny odczyt klastrow.\"}");
      case Error::Identity:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Sprawdz konfiguracje i tozsamosc SUPLA.\"}");
      case Error::Ticket:
      case Error::Conflict:
      case Error::Expired:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Propozycja nieaktualna. Wykonaj nowy odczyt i przygotuj propozycje.\"}");
      default:
        ESP_LOGW(TAG, "Channel proposal: %s", m2s::mapping::error_name(error));
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"message\":\"Operacja mapowania nie powiodla sie. Sprawdz dziennik.\"}");
    }
}

esp_err_t api_ota_get(httpd_req_t *req)
{
    char query[64],text[24];
    if(httpd_req_get_url_query_str(req,query,sizeof(query))!=ESP_OK ||
       httpd_query_key_value(query,"node",text,sizeof(text))!=ESP_OK)
        return send_json(req,"400 Bad Request","{\"ok\":false}");
    for(const char *p=text;*p;++p)if(*p<'0' || *p>'9')return send_json(req,"400 Bad Request","{\"ok\":false}");
    char *end=nullptr;unsigned long long node=strtoull(text,&end,10);
    if(!*text || *end || !node || node>UINT32_MAX || !quick_add_device_exists(node))
        return send_json(req,"404 Not Found","{\"ok\":false}");
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> result(matter_ota::snapshot(node),cJSON_Delete);
    char *body=result?cJSON_PrintUnformatted(result.get()):nullptr;
    const auto error=send_json(req,body?"200 OK":"500 Internal Server Error",body?body:"{\"ok\":false}");
    cJSON_free(body);return error;
}
esp_err_t api_ota_post(httpd_req_t *req)
{
    char body[256];if(read_body_json(req,body,sizeof(body))<0)return send_json(req,"400 Bad Request","{\"ok\":false}");
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> j(cJSON_ParseWithOpts(body,nullptr,true),cJSON_Delete);
    const double node=json_number(j.get(),"node");
    const char *action=cJSON_GetStringValue(cJSON_GetObjectItem(j.get(),"action"));
    if(!valid_node_number(node) || !action)return send_json(req,"400 Bad Request","{\"ok\":false}");
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    esp_err_t error=ESP_ERR_INVALID_ARG;
    if(strcmp(action,"check")==0)error=matter_ota::check(node);
    else if(strcmp(action,"install")==0) {
        const double version=json_number(j.get(),"version");
        if(std::isfinite(version) && version>0 && version<=UINT32_MAX && floor(version)==version)
            error=matter_ota::install(node,version);
    }
    if(error==ESP_ERR_NOT_FOUND)return send_json(req,"404 Not Found","{\"ok\":false}");
    if(error!=ESP_OK)return send_json(req,"409 Conflict","{\"ok\":false,\"message\":\"Wykonaj swiezy odczyt klastrow i sprawdz aktualizacje. Urzadzenie musi obslugiwac Matter OTA; poczekaj na odpowiedz przed kolejna proba.\"}");
    return send_json(req,"200 OK","{\"ok\":true,\"message\":\"Zlecenie wyslane. Poczekaj na potwierdzenie urzadzenia.\"}");
}

esp_err_t api_channels_prepare_post(httpd_req_t *req)
{
    char body[256];
    if (read_body_json(req, body, sizeof(body)) < 0)
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> request(cJSON_ParseWithOpts(body, nullptr, true), cJSON_Delete);
    const double value = json_number(request.get(), "node");
    if (!std::isfinite(value) || value < 1 || value > UINT32_MAX || value != floor(value))
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    cJSON *result = nullptr;
    m2s::proposal::Thermohygrometer merge;
    const auto *pair=cJSON_GetObjectItemCaseSensitive(request.get(),"thermohygrometer");
    if(pair) {
        const double temperature=json_number(pair,"temperature_endpoint"), humidity=json_number(pair,"humidity_endpoint");
        if(!cJSON_IsObject(pair) || !std::isfinite(temperature) || !std::isfinite(humidity) || temperature<1 || humidity<1 ||
           temperature>=65535 || humidity>=65535 || floor(temperature)!=temperature || floor(humidity)!=humidity)
            return send_json(req,"400 Bad Request","{\"ok\":false,\"message\":\"Niepoprawne endpointy pomiarow.\"}");
        merge.temperature_endpoint=temperature;merge.humidity_endpoint=humidity;
        if(cJSON_HasObjectItem(pair,"channel")) {
            const auto *channel_item=cJSON_GetObjectItemCaseSensitive(pair,"channel");
            double channel=cJSON_GetNumberValue(channel_item);
            if(!cJSON_IsNumber(channel_item) || !std::isfinite(channel) || channel<0 || channel>=m2s::mapping::kMaxChannels || floor(channel)!=channel)
                return send_json(req,"400 Bad Request","{\"ok\":false}");
            merge.channel=channel;
        }
    }
    m2s::proposal::ElectricityMeter meter;
    const auto *electrical=cJSON_GetObjectItemCaseSensitive(request.get(),"electricity_meter");
    if(electrical) {
        const double relay=json_number(electrical,"relay_endpoint"), endpoint=json_number(electrical,"meter_endpoint");
        if(pair || !cJSON_IsObject(electrical) || !std::isfinite(relay) || !std::isfinite(endpoint) ||
           relay<1 || endpoint<1 || relay>=65535 || endpoint>=65535 || floor(relay)!=relay || floor(endpoint)!=endpoint)
            return send_json(req,"400 Bad Request","{\"ok\":false,\"message\":\"Niepoprawne endpointy sterowania i licznika.\"}");
        meter.relay_endpoint=relay;meter.meter_endpoint=endpoint;
    }
    const auto error = m2s::proposal::prepare(static_cast<uint32_t>(value), &result,pair?&merge:nullptr,electrical?&meter:nullptr);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> owned(result, cJSON_Delete);
    if (error != m2s::mapping::Error::Ok) return mapping_error(req, error);
    char *payload = cJSON_PrintUnformatted(owned.get());
    const auto response = send_json(req, payload ? "200 OK" : "500 Internal Server Error",
                                    payload ? payload : "{\"ok\":false}");
    cJSON_free(payload);
    return response;
}

esp_err_t api_channels_commit_post(httpd_req_t *req)
{
    char body[128];
    if (read_body_json(req, body, sizeof(body)) < 0)
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> request(cJSON_ParseWithOpts(body, nullptr, true), cJSON_Delete);
    const char *token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request.get(), "token"));
    if (!token || strlen(token) != 32)
        return send_json(req, "400 Bad Request", "{\"ok\":false}");
    const auto error = m2s::proposal::commit(token);
    if (error == m2s::mapping::Error::Faulted) {
        const auto result = send_json(req, "500 Internal Server Error",
            "{\"ok\":false,\"message\":\"Wynik zapisu niepewny. Bramka uruchomi sie ponownie i sprawdzi mape.\"}");
        schedule_restart(1200);
        return result;
    }
    if (error != m2s::mapping::Error::Ok) return mapping_error(req, error);
    const auto result = send_json(req, "200 OK", "{\"ok\":true,\"message\":\"Zapisano mape. Bramka uruchomi sie ponownie.\"}");
    schedule_restart(1200);
    return result;
}

esp_err_t api_device_remove_post(httpd_req_t *req)
{
    char body[256];
    if (read_body_json(req, body, sizeof(body)) < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Brak danych.\"}");
    }
    cJSON *root = cJSON_Parse(body);
    uint64_t node = (uint64_t)json_number(root, "node");
    if (root) {
        cJSON_Delete(root);
    }
    if (node == 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Brak numeru node.\"}");
    }
    if(pairing_busy())return send_json(req,"409 Conflict","{\"ok\":false,\"message\":\"Trwa operacja Matter.\"}");
    if(!devices_contains(node))return send_json(req,"404 Not Found","{\"ok\":false}");
    set_status(QA_REMOVING,node,"Usuwanie urzadzenia...");
    esp_err_t err;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        err = esp_matter::controller::unpair_device((chip::NodeId)node);
    }
    if (err != ESP_OK) {
        set_status(QA_FAILED,node,"Nie udalo sie usunac urzadzenia.");
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"message\":\"Nie udalo sie usunac urzadzenia.\"}");
    }
    return send_json(req, "200 OK", "{\"ok\":true,\"message\":\"Usuwam urzadzenie...\"}");
}

// ---------------------------------------------------------------------------
// /api/wifi/* - konfiguracja Wi-Fi z panelu
// ---------------------------------------------------------------------------

esp_err_t api_wifi_scan_get(httpd_req_t *req)
{
    char *json = (char *)malloc(3072);
    if (!json) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"message\":\"Brak pamieci.\"}");
    }
    esp_err_t err = wifi_manager_scan(json, 3072);
    esp_err_t ret;
    if (err == ESP_OK) {
        ret = send_json(req, "200 OK", json);
    } else {
        ret = send_json(req, "500 Internal Server Error",
                        "{\"ok\":false,\"message\":\"Skan nie powiodl sie - sprobuj ponownie.\"}");
    }
    free(json);
    return ret;
}

esp_err_t api_wifi_connect_post(httpd_req_t *req)
{
    char body[256];
    if (read_body_json(req, body, sizeof(body)) < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Brak danych.\"}");
    }
    cJSON *root = cJSON_Parse(body);
    cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : nullptr;
    cJSON *pass = root ? cJSON_GetObjectItem(root, "pass") : nullptr;
    bool ok = false;
    if (cJSON_IsString(ssid) && ssid->valuestring[0]) {
        ok = (wifi_manager_connect(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "") == ESP_OK);
    }
    if (root) {
        cJSON_Delete(root);
    }
    if (!ok) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Podaj nazwe sieci.\"}");
    }
    return send_json(req, "200 OK",
                     "{\"ok\":true,\"message\":\"Lacze z siecia... panel bedzie tez pod nowym adresem IP.\"}");
}

esp_err_t api_wifi_forget_post(httpd_req_t *req)
{
    if(wifi_manager_forget()!=ESP_OK)return send_json(req,"500 Internal Server Error","{\"ok\":false,\"message\":\"Nie usunieto konfiguracji z NVS.\"}");
    schedule_restart(800);
    return send_json(req, "200 OK",
                     "{\"ok\":true,\"message\":\"Usunieto dane Wi-Fi. Restart w trybie konfiguracji (AP)...\"}");
}

// ---------------------------------------------------------------------------
// /api/system/* i /api/logs
// ---------------------------------------------------------------------------

esp_err_t api_system_reboot_post(httpd_req_t *req)
{
    schedule_restart(800);
    return send_json(req, "200 OK", "{\"ok\":true,\"message\":\"Restartuje bramke...\"}");
}

esp_err_t api_logs_get(httpd_req_t *req)
{
    static char out[4096];
    log_tail(out, sizeof(out));
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

// ---------------------------------------------------------------------------
// /api/supla - polaczenie z chmura Supla (zakladka "Supla" w panelu)
// ---------------------------------------------------------------------------

bool hex_matches_bytes(const char *hex, const uint8_t *bytes)
{
    uint8_t parsed[SUPLA_CFG_ID_SIZE];
    return supla_config_hex_to_bytes(hex, parsed) && memcmp(parsed, bytes, SUPLA_CFG_ID_SIZE) == 0;
}

esp_err_t api_supla_get(httpd_req_t *req)
{
    supla_cfg_t cfg_value{};
    if(supla_config_copy(&cfg_value)!=ESP_OK)return send_json(req,"500 Internal Server Error","{\"ok\":false,\"message\":\"Blad konfiguracji NVS.\"}");
    const supla_cfg_t *cfg=&cfg_value;

    char guid_hex[33];
    supla_config_bytes_to_hex(cfg->guid, guid_hex);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"message\":\"Brak pamieci.\"}");
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "configured", cfg->configured);
    cJSON_AddStringToObject(root, "server", cfg->server);
    cJSON_AddStringToObject(root, "email", cfg->email);
    cJSON_AddStringToObject(root, "guid", guid_hex);
    cJSON_AddBoolToObject(root, "authkey_set", true);
    cJSON_AddStringToObject(root, "state", supla_bridge_state_name());
    cJSON_AddNumberToObject(root, "code", supla_bridge_status_code());
    char status_message[160];
    supla_bridge_status_message_copy(status_message,sizeof(status_message));
    cJSON_AddStringToObject(root, "message", status_message);

    char *text = cJSON_PrintUnformatted(root);
    esp_err_t ret = send_json(req, "200 OK", text ? text : "{}");
    if (text) {
        free(text);
    }
    cJSON_Delete(root);
    return ret;
}

bool can_change_supla_guid()
{
    bool present = false;
    return m2s::mapping::v2_presence(present) == m2s::mapping::Error::Ok && !present;
}

esp_err_t api_supla_config_post(httpd_req_t *req)
{
    char body[512];
    if (read_body_json(req, body, sizeof(body)) < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"message\":\"Brak danych.\"}");
    }

    cJSON *root = cJSON_Parse(body);
    cJSON *j_server = root ? cJSON_GetObjectItem(root, "server") : nullptr;
    cJSON *j_email = root ? cJSON_GetObjectItem(root, "email") : nullptr;
    cJSON *j_guid = root ? cJSON_GetObjectItem(root, "guid") : nullptr;
    cJSON *j_key = root ? cJSON_GetObjectItem(root, "authkey") : nullptr;

    const char *server = cJSON_IsString(j_server) ? j_server->valuestring : "";
    const char *email = cJSON_IsString(j_email) ? j_email->valuestring : "";
    const char *guid_hex = cJSON_IsString(j_guid) ? j_guid->valuestring : "";
    const char *key_hex = cJSON_IsString(j_key) ? j_key->valuestring : "";

    if (server[0] == 0 || strlen(server) >= 64) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"Podaj adres serwera (np. svr1.supla.org).\"}");
    }
    if (email[0] == 0 || strlen(email) >= 64) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"Podaj e-mail konta Supla.\"}");
    }
    if (guid_hex[0] && !supla_config_hex_is_valid(guid_hex)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"GUID musi miec 32 znaki hex (0-9, a-f).\"}");
    }
    if (key_hex[0] && !supla_config_hex_is_valid(key_hex)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json(req, "400 Bad Request",
                         "{\"ok\":false,\"message\":\"AuthKey musi miec 32 znaki hex (0-9, a-f).\"}");
    }

    supla_cfg_t cfg_value{};
    if(supla_config_copy(&cfg_value)!=ESP_OK){cJSON_Delete(root);
    return send_json(req,"500 Internal Server Error","{\"ok\":false,\"message\":\"Blad konfiguracji NVS.\"}");}
    const supla_cfg_t *cfg=&cfg_value;
    bool was_configured = cfg->configured;
    bool changed = strcmp(server, cfg->server) != 0 || strcmp(email, cfg->email) != 0;
    if (!changed && guid_hex[0]) {
        changed = !hex_matches_bytes(guid_hex, cfg->guid);
    }
    if (!changed && key_hex[0]) {
        changed = !hex_matches_bytes(key_hex, cfg->authkey);
    }

    if (guid_hex[0] && !hex_matches_bytes(guid_hex, cfg->guid) && !can_change_supla_guid()) {
        cJSON_Delete(root);
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Aktywna mapa kanalow jest przypisana do obecnego GUID.\"}");
    }
    if (supla_config_save(server, email, guid_hex, key_hex) != ESP_OK) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"message\":\"Nie udalo sie zapisac konfiguracji.\"}");
    }

    supla_cfg_t after_value{};
    if(supla_config_copy(&after_value)!=ESP_OK){cJSON_Delete(root);
    return send_json(req,"500 Internal Server Error","{\"ok\":false}");}
    const supla_cfg_t *after=&after_value;
    bool restart = changed || (after->configured != was_configured);
    if (restart) {
        ESP_LOGW(TAG, "Konfiguracja Supla zmieniona - restart bramki");
        schedule_restart(1200);
    }

    char guid_out[33];
    char key_out[33];
    supla_config_bytes_to_hex(after->guid, guid_out);
    supla_config_bytes_to_hex(after->authkey, key_out);

    cJSON *resp = cJSON_CreateObject();
    if (resp) {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddBoolToObject(resp, "restart", restart);
        cJSON_AddStringToObject(resp, "guid", guid_out);
        cJSON_AddBoolToObject(resp, "authkey_set", true);
        cJSON_AddStringToObject(resp, "message",
                                restart ? "Zapisano. Bramka zrestartuje sie i polaczy z chmura Supla."
                                        : "Bez zmian - konfiguracja jest aktualna.");
    }
    char *text = resp ? cJSON_PrintUnformatted(resp) : nullptr;
    esp_err_t ret = send_json(req, "200 OK", text ? text : "{\"ok\":true}");
    if (text) {
        free(text);
    }
    if (resp) {
        cJSON_Delete(resp);
    }
    if (root) {
        cJSON_Delete(root);
    }
    return ret;
}

esp_err_t api_supla_new_identity_post(httpd_req_t *req)
{
    if (!can_change_supla_guid())
        return send_json(req, "409 Conflict", "{\"ok\":false,\"message\":\"Aktywna mapa kanalow jest przypisana do obecnego GUID.\"}");
    char guid_hex[33];
    char key_hex[33];
    if (supla_config_new_identity(guid_hex, key_hex) != ESP_OK) {
        return send_json(req, "500 Internal Server Error",
                         "{\"ok\":false,\"message\":\"Nie udalo sie zapisac nowej tozsamosci.\"}");
    }

    supla_cfg_t config_snapshot{};
    if(supla_config_copy(&config_snapshot)!=ESP_OK)return send_json(req,"500 Internal Server Error","{\"ok\":false}");
    bool restart = config_snapshot.configured;
    if (restart) {
        ESP_LOGW(TAG, "Nowa tozsamosc Supla - restart bramki");
        schedule_restart(1200);
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false}");
    }
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddBoolToObject(resp, "restart", restart);
    cJSON_AddStringToObject(resp, "guid", guid_hex);
    cJSON_AddStringToObject(resp, "authkey", key_hex);
    cJSON_AddStringToObject(resp, "message",
                            restart ? "Wygenerowano nowa tozsamosc. Bramka zrestartuje sie..."
                                    : "Wygenerowano nowa tozsamosc (zapisz razem z serwerem i e-mailem).");
    char *text = cJSON_PrintUnformatted(resp);
    esp_err_t ret = send_json(req, "200 OK", text ? text : "{}");
    if (text) {
        free(text);
    }
    cJSON_Delete(resp);
    return ret;
}

// ---------------------------------------------------------------------------
// Start panelu (wolane z app_main po starcie Matter/OTBR)
// ---------------------------------------------------------------------------


esp_err_t api_supla_reveal_key_post(httpd_req_t *req) {
    supla_cfg_t cfg{};
    if(supla_config_copy(&cfg)!=ESP_OK)return send_json(req,"500 Internal Server Error","{\"ok\":false}");
    char key[33],response[96];
    supla_config_bytes_to_hex(cfg.authkey,key);
    snprintf(response,sizeof(response),"{\"ok\":true,\"authkey\":\"%s\"}",key);
    return send_json(req,"200 OK",response);
}
esp_err_t api_panel_password_post(httpd_req_t *req) {
    char body[256]{};
    if(read_body_json(req,body,sizeof(body))<0)
        return send_json(req,"400 Bad Request","{\"ok\":false}");
    cJSON *json=cJSON_Parse(body);
    cJSON *password=json?cJSON_GetObjectItemCaseSensitive(json,"password"):nullptr;
    esp_err_t err=cJSON_IsString(password)
        ?panel_security_change_password(password->valuestring):ESP_ERR_INVALID_ARG;
    memset(body,0,sizeof(body));
    if(cJSON_IsString(password) && password->valuestring)
        memset(password->valuestring,0,strlen(password->valuestring));
    cJSON_Delete(json);
    if(err==ESP_ERR_INVALID_ARG)
        return send_json(req,"400 Bad Request","{\"ok\":false,\"message\":\"Haslo: 8-64 znaki ASCII, bez spacji.\"}");
    if(err!=ESP_OK)
        return send_json(req,"500 Internal Server Error","{\"ok\":false,\"message\":\"Nie zapisano hasla. Poprzedni dostep nadal obowiazuje.\"}");
    return send_json(req,"200 OK","{\"ok\":true}");
}

void https_error_detail(esp_https_server_user_cb_arg_t *event)
{
    if(!event || event->user_cb_state!=HTTPD_SSL_USER_CB_SESS_ERROR || !event->tls)return;
    auto *ssl=static_cast<mbedtls_ssl_context *>(esp_tls_get_ssl_context(event->tls));
    if(!ssl)return;
    const int alert=mbedtls_ssl_get_fatal_alert(ssl);
    if(alert<0)return;
    const char *reason=alert==48?"unknown_ca":alert==46?"certificate_unknown":alert==42?"bad_certificate":
                       alert==40?"handshake_failure":"other";
    ESP_LOGW(TAG,"HTTPS client TLS alert %d (%s); certificate checks remain enabled",alert,reason);
}

void start_http_server()
{
    if (s_server) {
        return;
    }

    esp_matter::controller::pairing_command_callbacks_t callbacks = {};
    callbacks.pase_callback = on_pase_complete;
    callbacks.commissioning_success_callback = on_commissioning_success;
    callbacks.commissioning_failure_callback = on_commissioning_failure;
    callbacks.unpair_complete_callback = on_unpair_complete;
    esp_matter::controller::pairing_command::get_instance().set_callbacks(callbacks);

    httpd_ssl_config_t tls=HTTPD_SSL_CONFIG_DEFAULT();
    tls.user_cb=https_error_detail;
    tls.httpd.max_req_hdr_len=2048;
    tls.httpd.stack_size=12288;
    tls.httpd.max_uri_handlers=48;
    tls.httpd.lru_purge_enable=true;
    tls.httpd.recv_wait_timeout=2;
    tls.httpd.send_wait_timeout=5;
    tls.servercert=(const uint8_t*)panel_security_certificate();
    tls.servercert_len=strlen(panel_security_certificate())+1;
    tls.prvtkey_pem=(const uint8_t*)panel_security_private_key();
    tls.prvtkey_len=strlen(panel_security_private_key())+1;
    tls.tls_handshake_timeout_ms=5000;
    if (httpd_ssl_start(&s_server, &tls) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start nie powiodl sie");
        s_server = nullptr;
        return;
    }

    struct routes_t {
        const char *uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t *);
    };
    static const routes_t routes[] = {
        {"/api/panel/certificate", HTTP_POST, gateway::certificate_save},
        {"/api/gateway/diagnostics", HTTP_GET, gateway::diagnostics},
        {"/api/gateway/settings", HTTP_GET, gateway::settings_get},
        {"/api/gateway/name", HTTP_POST, gateway::name_save},
        {"/api/gateway/thread-channel", HTTP_POST, gateway::thread_channel_save},
        {"/api/gateway/cloud-map", HTTP_GET, gateway::cloud_map},
        {"/api/gateway/backup", HTTP_POST, gateway::backup},
        {"/api/gateway/restore", HTTP_POST, gateway::restore},
        {"/api/gateway/update", HTTP_POST, gateway::update},
        {"/api/gateway/update-rcp", HTTP_POST, gateway::update_rcp},
        {"/api/gateway/rollback", HTTP_POST, gateway::rollback},
        {"/api/mqtt", HTTP_GET, gateway::mqtt_get},
        {"/api/mqtt", HTTP_POST, gateway::mqtt_save},
        {"/", HTTP_GET, root_get},
        {"/index.html", HTTP_GET, root_get},
        {"/app.js", HTTP_GET, app_js_get},
        {"/theme.js", HTTP_GET, [](httpd_req_t *r){return send_embedded(r,theme_js_start,theme_js_end,"application/javascript; charset=utf-8");}},
        {"/gateway.js", HTTP_GET, [](httpd_req_t *r){return send_embedded(r,gateway_js_start,gateway_js_end,"application/javascript; charset=utf-8");}},
        {"/jsQR.js", HTTP_GET, jsqr_js_get},
        {"/api/status", HTTP_GET, api_status_get},
        {"/api/panel/password", HTTP_POST, api_panel_password_post},
        {"/api/add", HTTP_POST, api_add_post},
        {"/api/matter/pair", HTTP_POST, api_matter_pair_post},
        {"/api/devices", HTTP_GET, api_devices_get},
        {"/api/devices/list", HTTP_GET, api_device_list_get},
        {"/api/matter/discovery", HTTP_GET, api_discovery_get},
        {"/api/matter/discovery", HTTP_POST, api_discovery_post},
        {"/api/matter/ota", HTTP_GET, api_ota_get},
        {"/api/matter/ota", HTTP_POST, api_ota_post},
        {"/api/matter/channels/prepare", HTTP_POST, api_channels_prepare_post},
        {"/api/matter/channels/commit", HTTP_POST, api_channels_commit_post},
        {"/api/device/control", HTTP_POST, gateway::relay_control},
        {"/api/device/rename", HTTP_POST, api_device_rename_post},
        {"/api/device/remove", HTTP_POST, api_device_remove_post},
        {"/api/wifi/scan", HTTP_GET, api_wifi_scan_get},
        {"/api/wifi/connect", HTTP_POST, api_wifi_connect_post},
        {"/api/wifi/forget", HTTP_POST, api_wifi_forget_post},
        {"/api/system/reboot", HTTP_POST, api_system_reboot_post},
        {"/api/logs", HTTP_GET, api_logs_get},
        {"/api/supla", HTTP_GET, api_supla_get},
        {"/api/supla/config", HTTP_POST, api_supla_config_post},
        {"/api/supla/new_identity", HTTP_POST, api_supla_new_identity_post},
        {"/api/supla/reveal_key", HTTP_POST, api_supla_reveal_key_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t uri = {};
        uri.uri = routes[i].uri;
        uri.method = routes[i].method;
        uri.user_ctx=(void*)&routes[i];
        uri.handler=[](httpd_req_t *req)->esp_err_t {
            const auto *route=(const routes_t*)req->user_ctx;
            if(route->method==HTTP_GET && req->content_len){send_json(req,"400 Bad Request","{\"ok\":false}");
            return ESP_FAIL;}
            if(strncmp(route->uri,"/api/",5)==0 && !panel_security_authorize(req,route->method==HTTP_POST,strcmp(route->uri,"/api/add")==0))return ESP_FAIL;
            if(req->content_len>=1024 && strcmp(route->uri,"/api/gateway/update") && strcmp(route->uri,"/api/gateway/update-rcp") && strcmp(route->uri,"/api/gateway/restore") && strcmp(route->uri,"/api/mqtt") && strcmp(route->uri,"/api/panel/certificate")){send_json(req,"413 Content Too Large","{\"ok\":false}");
            return ESP_FAIL;}
            httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
            httpd_resp_set_hdr(req,"X-Frame-Options","DENY");
            httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; script-src 'self'; style-src 'unsafe-inline'; img-src 'self' data: blob:; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
            RequestOutcome outcome;
            void *saved=req->user_ctx;
            req->user_ctx=&outcome;
            const bool empty_post=route->method==HTTP_POST &&
                (strcmp(route->uri,"/api/wifi/forget")==0 || strcmp(route->uri,"/api/system/reboot")==0 ||
                 strcmp(route->uri,"/api/supla/new_identity")==0 || strcmp(route->uri,"/api/supla/reveal_key")==0);
            if(empty_post){
                char body[128];
                if(read_body_json(req,body,sizeof(body))<0){req->user_ctx=saved;
                send_json(req,"400 Bad Request","{\"ok\":false}");
                return ESP_FAIL;}
                cJSON *json=cJSON_Parse(body);
                bool valid=cJSON_IsObject(json);
                cJSON_Delete(json);
                if(!valid){req->user_ctx=saved;
                send_json(req,"400 Bad Request","{\"ok\":false}");
                return ESP_FAIL;}
            }
            esp_err_t result;
            if(strcmp(route->uri,"/api/status")==0 && panel_security_setup_required(req))
                result=send_json(req,"200 OK","{\"setup_required\":true}");
            else result=route->handler(req);
            req->user_ctx=saved;
            return outcome.close?ESP_FAIL:result;
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uri));
    }

    ESP_LOGI(TAG, "Panel WWW uruchomiony: https://<ip-bramki>/ (dodawanie: QR/zdjecie/kod)");
}

} // namespace


extern "C" bool quick_add_pairing_busy(void){return pairing_busy();}

extern "C" void quick_add_init_locks(void) {
    // Called from app_main before any task (gateway services, MQTT) can touch the registry.
    if(!s_devices_mutex)s_devices_mutex=xSemaphoreCreateMutexStatic(&s_devices_mutex_storage);
    if(!s_devices_writer)s_devices_writer=xSemaphoreCreateMutexStatic(&s_devices_writer_storage);
}
extern "C" unsigned int quick_add_devices_count(void) {
    RegistryLock lock(s_devices_mutex);
    return (unsigned int)s_devices_count;
}
extern "C" unsigned int quick_add_devices_copy(uint64_t *nodes,unsigned int max) {
    if(!nodes||!max)return 0;
    RegistryLock lock(s_devices_mutex);
    unsigned int count=0;
    for(size_t i=0;i<s_devices_count&&count<max;i++)nodes[count++]=s_devices[i].node;
    return count;
}
extern "C" bool quick_add_device_name_copy(uint64_t node,char *out,size_t size) {
    if(!out||!size)return false;
    RegistryLock lock(s_devices_mutex);
    out[0]=0;
    for(size_t i=0;i<s_devices_count;i++)if(s_devices[i].node==node){snprintf(out,size,"%s",s_devices[i].name);
    return true;}
    return false;
}
extern "C" bool quick_add_device_exists(uint64_t node){return devices_contains(node);}
extern "C" bool quick_add_device_metadata(uint64_t node,const char *product,const char *label,
                                           uint16_t vid,uint16_t pid,bool has_vid,bool has_pid) {
    RegistryLock writer(s_devices_writer);
    auto next=std::unique_ptr<DeviceRegistry>(new(std::nothrow)DeviceRegistry{});
    if(!next)return false;
    devices_snapshot(next.get());
    for(size_t i=0;i<next->count;i++) {
        auto &entry=next->entries[i];
        if(entry.node!=node)continue;
        const auto before=entry;
        char generated[40];
        snprintf(generated,sizeof(generated),"Urzadzenie %u",(unsigned)node);
        const char *reported=m2s::valid_name(label)?label:(m2s::valid_name(product)?product:nullptr);
        if(reported && strcmp(entry.name,generated)==0)snprintf(entry.name,sizeof(entry.name),"%s",reported);
        if(has_vid)entry.vid=vid;
        if(has_pid)entry.pid=pid;
        if(memcmp(&entry,&before,sizeof(entry))==0)return true;
        return devices_persist(*next)==ESP_OK;
    }
    return false; // A delayed read must never resurrect a removed device.
}

extern "C" void quick_add_start(void) {
    s_lock=xSemaphoreCreateMutex();
    quick_add_init_locks();
    ESP_ERROR_CHECK(s_lock?ESP_OK:ESP_ERR_NO_MEM);
    // Corrupt stored data must not turn into an abort() boot loop: keep the panel
    // and Wi-Fi up (limited mode) so a backup can be restored or another image installed.
    esp_err_t err=devices_load();
    if(err!=ESP_OK){s_registry_readonly=true;gateway::note_boot_fault("device_registry",err);}
    const esp_err_t supla_err=supla_config_load();
    if(supla_err!=ESP_OK)gateway::note_boot_fault("supla_config",supla_err);
    log_capture_start();
    start_http_server();
    if(supla_err==ESP_OK)supla_bridge_start();
}
