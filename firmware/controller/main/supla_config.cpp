/*
 * supla_config.cpp - konfiguracja polaczenia z chmura Supla w NVS.
 */

#include "supla_config.h"

#include <cctype>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_random.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char *TAG = "supla_cfg";
#define SUPLA_NVS_NS "supla"

static supla_cfg_t s_cfg = {};
static bool s_loaded = false;

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void supla_config_bytes_to_hex(const uint8_t *in, char *out)
{
    static const char digits[] = "0123456789abcdef";
    if (!out) {
        return;
    }
    if (!in) {
        out[0] = 0;
        return;
    }
    for (int i = 0; i < SUPLA_CFG_ID_SIZE; i++) {
        out[i * 2] = digits[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[SUPLA_CFG_ID_SIZE * 2] = 0;
}

bool supla_config_hex_is_valid(const char *hex)
{
    if (!hex || strlen(hex) != SUPLA_CFG_ID_SIZE * 2) {
        return false;
    }
    for (size_t i = 0; i < SUPLA_CFG_ID_SIZE * 2; i++) {
        if (hex_nibble(hex[i]) < 0) {
            return false;
        }
    }
    return true;
}

bool supla_config_hex_to_bytes(const char *hex, uint8_t *out)
{
    if (!supla_config_hex_is_valid(hex) || !out) {
        return false;
    }
    for (int i = 0; i < SUPLA_CFG_ID_SIZE; i++) {
        out[i] = (uint8_t)((hex_nibble(hex[i * 2]) << 4) | hex_nibble(hex[i * 2 + 1]));
    }
    return true;
}


namespace {
StaticSemaphore_t cfg_mutex_storage;
SemaphoreHandle_t cfg_mutex=nullptr;
struct StoredConfig { uint32_t version;
supla_cfg_t cfg; };
struct Guard { Guard(){ xSemaphoreTake(cfg_mutex,portMAX_DELAY); } ~Guard(){xSemaphoreGive(cfg_mutex);} };
void ensure_mutex() { if (!cfg_mutex) cfg_mutex=xSemaphoreCreateMutexStatic(&cfg_mutex_storage); }
bool valid_text(const char *text,size_t maximum) {
    if (!text || strlen(text)>=maximum) return false;
    for(const unsigned char *p=(const unsigned char*)text;*p;p++) if (*p<0x20 || *p==0x7f) return false;
    return true;
}
esp_err_t persist(const supla_cfg_t &candidate) {
    nvs_handle_t handle;
    esp_err_t err=nvs_open(SUPLA_NVS_NS,NVS_READWRITE,&handle);
    if (err!=ESP_OK) return err;
    StoredConfig stored{};
    stored.version=1;
    stored.cfg=candidate;
    err=nvs_set_blob(handle,"cfg_v1",&stored,sizeof(stored));
    if (err==ESP_OK) err=nvs_commit(handle);
    nvs_close(handle);
    if (err==ESP_OK) {s_cfg=candidate;
    s_loaded=true;}
    return err;
}
esp_err_t get_legacy_string(nvs_handle_t h,const char *key,char *out,size_t capacity) {
    size_t n=capacity;
    esp_err_t err=nvs_get_str(h,key,out,&n);
    if (err==ESP_ERR_NVS_NOT_FOUND) {out[0]=0;
    return ESP_OK;}
    return err;
}
}
esp_err_t supla_config_load(void) {
    ensure_mutex();
    Guard lock;
    if (s_loaded) return ESP_OK;
    nvs_handle_t handle;
    esp_err_t err=nvs_open(SUPLA_NVS_NS,NVS_READWRITE,&handle);
    if (err!=ESP_OK) return err;
    StoredConfig stored{};
    size_t n=sizeof(stored);
    err=nvs_get_blob(handle,"cfg_v1",&stored,&n);
    if (err==ESP_OK) {
        nvs_close(handle);
        if(n!=sizeof(stored)||stored.version!=1||!memchr(stored.cfg.server,0,sizeof(stored.cfg.server))||!memchr(stored.cfg.email,0,sizeof(stored.cfg.email))) return ESP_ERR_INVALID_STATE;
        s_cfg=stored.cfg;
        s_cfg.configured=s_cfg.server[0]&&s_cfg.email[0];
        s_loaded=true;
        return ESP_OK;
    }
    if(err!=ESP_ERR_NVS_NOT_FOUND){nvs_close(handle);
    return err;}
    supla_cfg_t candidate{};
    err=get_legacy_string(handle,"srv",candidate.server,sizeof(candidate.server));
    if(err==ESP_OK)err=get_legacy_string(handle,"email",candidate.email,sizeof(candidate.email));
    for (int i=0;i<2&&err==ESP_OK;i++) {
        auto *out=i?candidate.authkey:candidate.guid;
        n=SUPLA_CFG_ID_SIZE;
        esp_err_t read=nvs_get_blob(handle,i?"key":"guid",out,&n);
        if(read==ESP_ERR_NVS_NOT_FOUND) esp_fill_random(out,SUPLA_CFG_ID_SIZE);
        else if(read!=ESP_OK||n!=SUPLA_CFG_ID_SIZE) err=read==ESP_OK?ESP_ERR_INVALID_STATE:read;
    }
    nvs_close(handle);
    if(err!=ESP_OK)return err;
    candidate.configured=candidate.server[0]&&candidate.email[0];
    return persist(candidate);
}
esp_err_t supla_config_copy(supla_cfg_t *out) {
    if(!out)return ESP_ERR_INVALID_ARG;
    esp_err_t err=supla_config_load();
    if(err!=ESP_OK)return err;
    Guard lock;
    *out=s_cfg;
    return ESP_OK;
}
esp_err_t supla_config_save(const char *server,const char *email,const char *guid_hex,const char *authkey_hex) {
    esp_err_t err=supla_config_load();
    if(err!=ESP_OK)return err;
    if(!valid_text(server,sizeof(s_cfg.server))||!server[0]||!valid_text(email,sizeof(s_cfg.email))||!email[0]||!strchr(email,'@'))return ESP_ERR_INVALID_ARG;
    for(const unsigned char *p=(const unsigned char*)server;*p;p++) if(!isalnum(*p)&&*p!='.'&&*p!='-')return ESP_ERR_INVALID_ARG;
    Guard lock;
    supla_cfg_t candidate=s_cfg;
    snprintf(candidate.server,sizeof(candidate.server),"%s",server);
    snprintf(candidate.email,sizeof(candidate.email),"%s",email);
    if(guid_hex&&guid_hex[0]&&!supla_config_hex_to_bytes(guid_hex,candidate.guid))return ESP_ERR_INVALID_ARG;
    if(authkey_hex&&authkey_hex[0]&&!supla_config_hex_to_bytes(authkey_hex,candidate.authkey))return ESP_ERR_INVALID_ARG;
    candidate.configured=true;
    return persist(candidate);
}
esp_err_t supla_config_new_identity(char *guid_hex_out,char *authkey_hex_out) {
    esp_err_t err=supla_config_load();
    if(err!=ESP_OK)return err;
    Guard lock;
    supla_cfg_t candidate=s_cfg;
    esp_fill_random(candidate.guid,sizeof(candidate.guid));
    esp_fill_random(candidate.authkey,sizeof(candidate.authkey));
    err=persist(candidate);
    if(err!=ESP_OK)return err;
    if(guid_hex_out)supla_config_bytes_to_hex(candidate.guid,guid_hex_out);
    if(authkey_hex_out)supla_config_bytes_to_hex(candidate.authkey,authkey_hex_out);
    ESP_LOGW(TAG,"Zmieniono tozsamosc SUPLA");
    return ESP_OK;
}
