/*
 * wifi_manager.cpp - trwale Wi-Fi bramki (bez konsoli):
 *   - dane sieci w NVS ("wifi_cfg": ssid/pass), auto-connect po starcie,
 *   - brak danych / nieudane proby -> wlasny AP konfiguracyjny "Matter-GW-XXXX"
 *     (panel pod https://192.168.4.1),
 *   - skan sieci / zmiana sieci / zapominanie z poziomu panelu.
 */

#include "wifi_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ping/ping_sock.h>
#include <freertos/semphr.h>
#include "panel_security.h"
#include "input_validation.h"

#include <cJSON.h>
#include <esp_attr.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>

static const char *TAG = "wifi_manager";

#define WIFI_NVS_NS "wifi_cfg"
#define MAX_STA_RETRIES 8

static esp_netif_t *s_sta_netif = nullptr;
static esp_netif_t *s_ap_netif = nullptr;
static bool s_wifi_inited = false;
static std::atomic<bool> s_ap_started{false};
static std::atomic<bool> s_recovery_used{false};
static std::atomic<bool> s_pending{false};
static std::atomic<int> s_last_error{ESP_OK};
static esp_timer_handle_t s_ap_timer = nullptr;
static StaticSemaphore_t s_wifi_mutex_storage;
static SemaphoreHandle_t s_wifi_mutex = nullptr;
static StaticSemaphore_t s_mode_mutex_storage;
static SemaphoreHandle_t s_mode_mutex = nullptr;
struct ModeLock {ModeLock(){xSemaphoreTake(s_mode_mutex,portMAX_DELAY);}~ModeLock(){xSemaphoreGive(s_mode_mutex);}};
struct WifiLock { WifiLock(){xSemaphoreTake(s_wifi_mutex,portMAX_DELAY);} ~WifiLock(){xSemaphoreGive(s_wifi_mutex);} };
static std::atomic<int> s_fail_count{0};
static char s_ap_ssid[33] = "";
static char s_sta_ssid[33] = "";
static char s_sta_pass[65] = "";
static std::atomic<bool> s_ap_timeout_pending{false};
static std::atomic<bool> s_got_ip_pending{false};
static std::atomic<uint32_t> s_disconnects{0}, s_ip_losses{0}, s_last_disconnect_reason{0};
static std::atomic<uint32_t> s_reconnects{0}, s_recoveries{0};
static std::atomic<uint32_t> s_link_generation{0};

constexpr int64_t kSecond = 1000000;
constexpr int64_t kNoIpGrace = 25 * kSecond;
constexpr int64_t kRecoveryCooldown = 120 * kSecond;
constexpr uint32_t kProbeIntervalMs = 15000;
constexpr uint32_t kProbeMisses = 4;

struct GatewayProbe {
    uintptr_t generation = 0;
    uint32_t misses = 0;
    bool active = false;
    bool seen_reply = false;
};

static portMUX_TYPE s_probe_lock = portMUX_INITIALIZER_UNLOCKED;
static GatewayProbe s_probe;

static GatewayProbe probe_status()
{
    portENTER_CRITICAL(&s_probe_lock);
    const auto status = s_probe;
    portEXIT_CRITICAL(&s_probe_lock);
    return status;
}

static void probe_result(void *context, bool success)
{
    const auto generation = reinterpret_cast<uintptr_t>(context);
    portENTER_CRITICAL(&s_probe_lock);
    if (s_probe.generation == generation && s_probe.active) {
        if (success) {
            s_probe.seen_reply = true;
            s_probe.misses = 0;
        } else if (s_probe.misses < UINT32_MAX) {
            ++s_probe.misses;
        }
    }
    portEXIT_CRITICAL(&s_probe_lock);
}

static void stop_probe(esp_ping_handle_t &handle)
{
    if (!handle) return;
    portENTER_CRITICAL(&s_probe_lock);
    s_probe = GatewayProbe{s_probe.generation + 1};
    portEXIT_CRITICAL(&s_probe_lock);
    esp_ping_stop(handle);
    esp_ping_delete_session(handle);
    handle = nullptr;
}

static esp_ping_handle_t start_probe(uint32_t gateway)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = kProbeIntervalMs;
    cfg.timeout_ms = 2000;
    cfg.interface = esp_netif_get_netif_impl_index(s_sta_netif);
    if (cfg.interface == 0 || cfg.interface == UINT32_MAX) return nullptr;
    ip_addr_set_ip4_u32(&cfg.target_addr, gateway);
    esp_ping_callbacks_t callbacks{};
    callbacks.on_ping_success = [](esp_ping_handle_t, void *ctx){ probe_result(ctx, true); };
    callbacks.on_ping_timeout = [](esp_ping_handle_t, void *ctx){ probe_result(ctx, false); };
    portENTER_CRITICAL(&s_probe_lock);
    const auto generation = s_probe.generation + 1;
    s_probe = GatewayProbe{generation, 0, true, false};
    portEXIT_CRITICAL(&s_probe_lock);
    callbacks.cb_args = reinterpret_cast<void *>(generation);
    esp_ping_handle_t handle = nullptr;
    if (esp_ping_new_session(&cfg, &callbacks, &handle) != ESP_OK) {
        portENTER_CRITICAL(&s_probe_lock);
        s_probe.active = false;
        portEXIT_CRITICAL(&s_probe_lock);
        return nullptr;
    }
    if (esp_ping_start(handle) != ESP_OK) stop_probe(handle);
    return handle;
}


static void start_ap(bool keep_sta);
static esp_err_t start_sta_locked(bool keep_ap);

/* --------------------------------------------------------------------- NVS */


struct WifiCredentials { uint32_t version;
char ssid[33];
char pass[65]; };
static bool load_creds(char *ssid,size_t ssid_size,char *pass,size_t pass_size) {
    nvs_handle_t h;
    esp_err_t err=nvs_open(WIFI_NVS_NS,NVS_READONLY,&h);
    if(err!=ESP_OK){if(err!=ESP_ERR_NVS_NOT_FOUND)s_last_error=err;
    return false;}
    WifiCredentials record{};
    size_t n=sizeof(record);
    err=nvs_get_blob(h,"creds_v1",&record,&n);
    if(err==ESP_OK) {
        bool valid=n==sizeof(record)&&record.version==1&&memchr(record.ssid,0,sizeof(record.ssid))&&memchr(record.pass,0,sizeof(record.pass))&&m2s::valid_wifi(record.ssid,record.pass);
        nvs_close(h);
        if(!valid){s_last_error=ESP_ERR_INVALID_STATE;
        return false;}
        snprintf(ssid,ssid_size,"%s",record.ssid);
        snprintf(pass,pass_size,"%s",record.pass);
        return true;
    }
    if(err!=ESP_ERR_NVS_NOT_FOUND){nvs_close(h);
    s_last_error=err;
    return false;}
    size_t len=ssid_size;
    err=nvs_get_str(h,"ssid",ssid,&len);
    len=pass_size;
    esp_err_t pe=nvs_get_str(h,"pass",pass,&len);
    if(pe==ESP_ERR_NVS_NOT_FOUND)pass[0]=0;
    nvs_close(h);
    if(err==ESP_ERR_NVS_NOT_FOUND)return false;
    if(err!=ESP_OK||(pe!=ESP_OK&&pe!=ESP_ERR_NVS_NOT_FOUND)||!m2s::valid_wifi(ssid,pass)){s_last_error=ESP_ERR_INVALID_STATE;
    return false;}
    return true;
}
static esp_err_t save_creds(const char *ssid,const char *pass) {
    WifiCredentials record{};
    record.version=1;
    snprintf(record.ssid,sizeof(record.ssid),"%s",ssid);
    snprintf(record.pass,sizeof(record.pass),"%s",pass);
    nvs_handle_t h;
    esp_err_t err=nvs_open(WIFI_NVS_NS,NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_set_blob(h,"creds_v1",&record,sizeof(record));
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    return err;
}
esp_err_t wifi_manager_forget(void) {
    WifiLock lock;
    s_pending=false;
    nvs_handle_t h;
    esp_err_t err=nvs_open(WIFI_NVS_NS,NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_erase_all(h);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    if(err==ESP_OK){s_sta_ssid[0]=0;
    s_sta_pass[0]=0;}
    s_last_error=err;
    return err;
}
static void ap_timeout_cb(void *) { s_ap_timeout_pending = true; }

static void finish_ap_timeout() {
    ModeLock operation;
    if(!s_ap_started)return;
    bool has_ssid;
    {WifiLock lock;
    has_ssid=s_sta_ssid[0]!=0;}
    esp_err_t err=esp_wifi_set_mode(has_ssid?WIFI_MODE_STA:WIFI_MODE_NULL);
    if(err==ESP_OK)s_ap_started=false;
    else s_last_error=err;
    ESP_LOGI(TAG,"Okno konfiguracji AP zakonczone; ponowne otwarcie wymaga restartu");
}
/* ------------------------------------------------------------ start/stop */

static esp_err_t start_sta_locked(bool keep_ap)
{
    keep_ap=keep_ap && s_ap_started.load();
    wifi_config_t sta_cfg = {};
    { WifiLock lock;
      memcpy(sta_cfg.sta.ssid,s_sta_ssid,strlen(s_sta_ssid));
      memcpy(sta_cfg.sta.password,s_sta_pass,strlen(s_sta_pass));
    }
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    s_got_ip_pending=false;
    ++s_link_generation;
    esp_wifi_stop();
    s_pending=true;
    esp_err_t err = esp_wifi_set_mode(keep_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        s_fail_count = 0;
        err = esp_wifi_connect();
    }
    ESP_LOGI(TAG, "Rozpoczeto probe polaczenia Wi-Fi");
    return err;
}

static void start_ap(bool keep_sta)
{
    ModeLock operation;
    if(s_recovery_used)return;
    wifi_config_t ap_cfg = {};
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", s_ap_ssid);
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    // Laboratory v2: initial setup must be possible without reading a console.
    // The existing 15-minute timer and shutdown after STA GOT_IP still apply.
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_stop();
    esp_err_t err = esp_wifi_set_mode(keep_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Nie udalo sie wlaczyc AP: %d", err);
        return;
    }
    s_ap_started = true;
    s_recovery_used = true;
    if(s_ap_timer){esp_timer_stop(s_ap_timer);
    esp_timer_start_once(s_ap_timer,15LL*60*1000000);}
    ESP_LOGW(TAG, "Tryb konfiguracji: polacz sie z siecia \"%s\" i otworz https://192.168.4.1", s_ap_ssid);
}


esp_err_t wifi_manager_connect(const char *ssid,const char *pass) {
    if(!m2s::valid_wifi(ssid,pass?pass:""))return ESP_ERR_INVALID_ARG;
    ModeLock operation;
    {WifiLock lock;
    snprintf(s_sta_ssid,sizeof(s_sta_ssid),"%s",ssid);
    snprintf(s_sta_pass,sizeof(s_sta_pass),"%s",pass?pass:"");
    s_pending=false;}
    esp_err_t err=start_sta_locked(s_ap_started.load());
    s_last_error=err;
    return err;
}
esp_err_t wifi_manager_last_error(void){return (esp_err_t)s_last_error.load();}
/* ------------------------------------------------------------------ eventy */

static void wifi_event_handler(void *, esp_event_base_t, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_CONNECTED) {
        s_fail_count = 0;
        ++s_link_generation;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ++s_disconnects;
        ++s_link_generation;
        const int failure = ++s_fail_count;
        const auto *event = static_cast<const wifi_event_sta_disconnected_t *>(data);
        const unsigned reason = event ? event->reason : 0;
        s_last_disconnect_reason = reason;
        ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u, attempt=%d/%d",reason,failure,MAX_STA_RETRIES);
    }
}

static void ip_event_handler(void *, esp_event_base_t, int32_t id, void *)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        s_got_ip_pending = true;
    } else if (id == IP_EVENT_STA_LOST_IP) {
        ++s_ip_losses;
        ESP_LOGW(TAG, "Wi-Fi station lost IPv4");
    }
}

static void finish_got_ip_locked(const wifi_ap_record_t &ap)
{
    s_fail_count = 0;
    {
        WifiLock lock;
        if(s_pending && strcmp(reinterpret_cast<const char *>(ap.ssid),s_sta_ssid)==0){
            s_last_error=save_creds(s_sta_ssid,s_sta_pass);
            s_pending=false;
            if(s_last_error!=ESP_OK)ESP_LOGE(TAG,"Wi-Fi połączone, zapis NVS nie powiódł się");
        }
    }
    s_recovery_used=true;
    if(s_ap_started){
        const esp_err_t err=esp_wifi_set_mode(WIFI_MODE_STA);
        if(err==ESP_OK){s_ap_started=false;if(s_ap_timer)esp_timer_stop(s_ap_timer);}
        else s_last_error=err;
    }
    ESP_LOGI(TAG, "Wi-Fi polaczone (got IP)");
}

static void wifi_health_task(void *)
{
    esp_ping_handle_t probe = nullptr;
    uint32_t probe_ip = 0, probe_gateway = 0, probe_link = 0;
    int64_t next_probe = 0, next_connect = esp_timer_get_time()+5*kSecond;
    int64_t no_ip_since = 0, next_recovery = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (s_ap_timeout_pending.exchange(false)) finish_ap_timeout();
        const auto now = esp_timer_get_time();
        bool start_setup = false;
        {
            ModeLock operation;
            bool configured;
            { WifiLock lock; configured = s_sta_ssid[0] != 0; }
            wifi_ap_record_t ap{};
            const bool associated = configured && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            const auto link = s_link_generation.load();
            esp_netif_ip_info_t ip{};
            const bool has_ip = associated && esp_netif_is_netif_up(s_sta_netif) &&
                esp_netif_get_ip_info(s_sta_netif,&ip)==ESP_OK && ip.ip.addr!=0;
            if (has_ip && s_got_ip_pending.exchange(false)) finish_got_ip_locked(ap);

            if (probe && (!has_ip || s_ap_started || ip.ip.addr!=probe_ip ||
                          ip.gw.addr!=probe_gateway || link!=probe_link)) {
                stop_probe(probe);
                // Let the SDK's old ping task release its socket before replacing it.
                next_probe=now+2*kProbeIntervalMs*1000;
            }
            if (!probe && has_ip && !s_ap_started && ip.gw.addr && now>=next_probe) {
                probe=start_probe(ip.gw.addr);
                probe_ip=ip.ip.addr;
                probe_gateway=ip.gw.addr;
                probe_link=link;
                next_probe=now+2*kProbeIntervalMs*1000;
            }

            if (associated && !has_ip && !s_ap_started) {
                if (!no_ip_since) no_ip_since=now;
            } else no_ip_since=0;
            const auto health=probe_status();
            const bool missing_ip=no_ip_since && now-no_ip_since>=kNoIpGrace;
            const bool missed_gateway=probe && health.seen_reply && health.misses>=kProbeMisses;
            if (associated && !s_ap_started && now>=next_recovery && (missing_ip || missed_gateway)) {
                next_recovery=now+kRecoveryCooldown;
                stop_probe(probe);
                next_probe=now+2*kProbeIntervalMs*1000;
                ++s_recoveries;
                const auto err=esp_wifi_disconnect();
                next_connect=now+2*kSecond;
                ESP_LOGW(TAG,"Wi-Fi recovery: %s, disconnect=%s",missing_ip?"IPv4 missing":"gateway unanswered",esp_err_to_name(err));
            } else if (configured && !associated && now>=next_connect) {
                const int failures=s_fail_count.load();
                start_setup=failures>=MAX_STA_RETRIES && !s_ap_started && !s_recovery_used;
                next_connect=now+(failures>=MAX_STA_RETRIES?10:3)*kSecond;
                if (!start_setup) {
                    ++s_reconnects;
                    const auto err=esp_wifi_connect();
                    if (err!=ESP_OK) ESP_LOGW(TAG,"Wi-Fi reconnect: %s",esp_err_to_name(err));
                }
            }
        }
        if (start_setup) start_ap(true);
    }
}

void wifi_manager_health(wifi_health_t *out)
{
    if (!out) return;
    const auto health=probe_status();
    *out={s_disconnects.load(),s_ip_losses.load(),s_last_disconnect_reason.load(),
          s_reconnects.load(),s_recoveries.load(),health.misses,health.active,health.seen_reply};
}

/* ------------------------------------------------------------------- start */

esp_err_t wifi_manager_start(void)
{
    if (s_wifi_inited) return ESP_OK;
    s_wifi_mutex=xSemaphoreCreateMutexStatic(&s_wifi_mutex_storage);
    s_mode_mutex=xSemaphoreCreateMutexStatic(&s_mode_mutex_storage);
    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    if (!s_sta_netif || !s_ap_netif) return ESP_ERR_NO_MEM;

    // Matter initializes the driver first. Reuse it; do not initialize twice.
    wifi_mode_t current_mode=WIFI_MODE_NULL;
    esp_err_t err=esp_wifi_get_mode(&current_mode);
    if(err==ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
        err=esp_wifi_init(&cfg);
    }
    if(err!=ESP_OK){ESP_LOGE(TAG,"Wi-Fi driver unavailable: %d",err);return err;}



    // Enable the Wi-Fi PHY before generating device secrets (hardware RNG entropy).
    err=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(err==ESP_OK)err=esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t empty{};
    if(err==ESP_OK)err=esp_wifi_set_config(WIFI_IF_STA,&empty);
    if(err==ESP_OK)err=esp_wifi_start();
    if(err==ESP_OK)err=panel_security_init();
    if(err!=ESP_OK)return err;
    esp_timer_create_args_t ap_args{};
    ap_args.callback=ap_timeout_cb;
    ap_args.name="secure_ap_window";
    err=esp_timer_create(&ap_args,&s_ap_timer);
    if(err!=ESP_OK)return err;
    err=esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr);
    if(err!=ESP_OK)return err;
    err=esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, nullptr, nullptr);
    if(err!=ESP_OK)return err;
    err=esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &ip_event_handler, nullptr, nullptr);
    if(err!=ESP_OK)return err;

    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Matter-GW-%02X%02X", mac[4], mac[5]);
    } else {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Matter-GW");
    }

    if (load_creds(s_sta_ssid, sizeof(s_sta_ssid), s_sta_pass, sizeof(s_sta_pass))) {
        ModeLock operation;
        const auto start=start_sta_locked(false);
        if(start!=ESP_OK)return start;
    } else {
        ESP_LOGI(TAG, "Brak zapisanej sieci Wi-Fi - startuje w trybie konfiguracji");
        start_ap(false);
        if(!s_ap_started)return ESP_FAIL;
    }
    if(xTaskCreate(wifi_health_task,"wifi_health",4096,nullptr,3,nullptr)!=pdPASS)return ESP_ERR_NO_MEM;
    s_wifi_inited = true;
    return ESP_OK;
}

bool wifi_manager_in_ap_mode(void)
{
    return s_ap_started;
}

const char *wifi_manager_ap_ssid(void)
{
    return s_ap_ssid;
}

/* -------------------------------------------------------------------- skan */

esp_err_t wifi_manager_scan(char *json_out, size_t out_size)
{
    if (!json_out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(json_out, out_size, "{\"networks\":[]}");
    if (!s_wifi_inited) {
        return ESP_ERR_INVALID_STATE;
    }

    static EXT_RAM_BSS_ATTR char s_cached_scan[2048];
    static int64_t s_cached_scan_at_us = 0;

    if (xSemaphoreTake(s_mode_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        // Another scan or Wi-Fi transition is in progress; return cached result safely under WifiLock.
        WifiLock cache_lock;
        const size_t cached_len = strlen(s_cached_scan);
        if (cached_len > 0 && cached_len < out_size) {
            memcpy(json_out, s_cached_scan, cached_len + 1);
            return ESP_OK;
        }
        return ESP_ERR_TIMEOUT;
    }
    struct ReleaseMode { ~ReleaseMode() { xSemaphoreGive(s_mode_mutex); } } release_mode;

    const int64_t now_us = esp_timer_get_time();
    {
        WifiLock cache_lock;
        const size_t cached_len = strlen(s_cached_scan);
        if (cached_len > 0 && s_cached_scan_at_us > 0 && (now_us - s_cached_scan_at_us) < 15LL * kSecond) {
            if (cached_len < out_size) {
                memcpy(json_out, s_cached_scan, cached_len + 1);
                return ESP_OK;
            }
        }
    }

    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Skan Wi-Fi nie powiodl sie: %d", err);
        return err;
    }

    uint16_t count = 0;
    err=esp_wifi_scan_get_ap_num(&count);
    if(err!=ESP_OK){esp_wifi_clear_ap_list();return err;}
    if (count > 20) {
        count = 20;
    }
    if (count == 0) {
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }
    wifi_ap_record_t *recs = (wifi_ap_record_t *)calloc(count, sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err=esp_wifi_scan_get_ap_records(&count,recs);
    if(err!=ESP_OK){esp_wifi_clear_ap_list();free(recs);
    return err;}

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    if (root && arr) {
        cJSON_AddItemToObject(root, "networks", arr);
        for (uint16_t i = 0; i < count; i++) {
            cJSON *o = cJSON_CreateObject();
            if (!o) {
                break;
            }
            cJSON_AddStringToObject(o, "ssid", (const char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
            cJSON_AddStringToObject(o, "auth", recs[i].authmode == WIFI_AUTH_OPEN ? "open" : "wpa");
            cJSON_AddItemToArray(arr, o);
        }
        char *text = cJSON_PrintUnformatted(root);
        if (text) {
            const size_t len = strlen(text);
            if(len>=out_size){free(text);
            cJSON_Delete(root);
            free(recs);
            return ESP_ERR_INVALID_SIZE;}
            memcpy(json_out,text,len+1);
            if (len < sizeof(s_cached_scan)) {
                WifiLock cache_lock;
                memcpy(s_cached_scan, text, len + 1);
                s_cached_scan_at_us = esp_timer_get_time();
            }
            free(text);
        }
    }
    if (root) {
        cJSON_Delete(root);
    }
    free(recs);
    return ESP_OK;
}
