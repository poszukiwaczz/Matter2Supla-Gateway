#include "gateway_services.h"
#include "gateway_formats.h"
#include "version.h"
#include "quick_add.h"
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <cstring>
#include <memory>
namespace gateway {namespace {
constexpr const char *kTag = "gateway_ota";
extern const uint8_t key_start[] asm("_binary_update_public_key_pem_start");
extern const uint8_t key_end[] asm("_binary_update_public_key_pem_end");
bool ready_layout(){return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,"otadata")&&esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"ota_0")&&esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"ota_1");}
bool receive(httpd_req_t *r,void *out,size_t n){auto *p=(char*)out;unsigned timeout=0;while(n){int got=httpd_req_recv(r,p,n);if(got==HTTPD_SOCK_ERR_TIMEOUT&&timeout++<5)continue;if(got<=0){ESP_LOGW(kTag,"HTTP receive failed: %d, %u bytes pending in chunk",got,(unsigned)n);return false;}
p+=got;n-=got;}return true;}
bool signature(const UpdateHeader &h){mbedtls_pk_context pk;mbedtls_pk_init(&pk);uint8_t hash[32];size_t written=0;bool hashed=psa_crypto_init()==PSA_SUCCESS&&psa_hash_compute(PSA_ALG_SHA_256,(const uint8_t*)&h,offsetof(UpdateHeader,signature_bytes),hash,32,&written)==PSA_SUCCESS&&written==32;bool ok=hashed&&mbedtls_pk_parse_public_key(&pk,key_start,key_end-key_start)==0&&mbedtls_pk_verify(&pk,MBEDTLS_MD_SHA256,hash,32,h.signature,h.signature_bytes)==0;mbedtls_pk_free(&pk);return ok;}
}
esp_err_t update(httpd_req_t *r){ESP_LOGI(kTag,"Upload request: %u bytes",(unsigned)r->content_len);if(!ready_layout()){return reply(r,409,"Wymagana jednorazowa migracja partycji0.3, OTA nie zapisuje tablicy partycji.");}
if(mutation_busy()){return reply(r,409,"Bramka jest zajeta.");}
struct Guard{~Guard(){mutation_done();}} guard;if(quick_add_pairing_busy()){return reply(r,409,"Poczekaj na zakonczenie parowania.");}

 UpdateHeader h{};if(r->content_len<sizeof(h)||r->content_len>kAppLimit+sizeof(h)||!receive(r,&h,sizeof(h))||!update_header(h,FW_BUILD,r->content_len)||!signature(h)){ESP_LOGW(kTag,"Rejected OTA header/signature (content_len=%u)",(unsigned)r->content_len);return reply(r,400,"Niepoprawny podpis, wersja, model lub rozmiar pakietu. Niczego nie zapisano.");}

 constexpr size_t kChunkBytes = 4096;
 auto buffer = std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[kChunkBytes]);
 if(!buffer){return reply(r,503,"Brak pamieci tymczasowej na bufor OTA.");}

 ESP_LOGI(kTag,"Signed header accepted: build %u, %u image bytes",(unsigned)h.build,(unsigned)h.image_bytes);
 const auto *target=esp_ota_get_next_update_partition(nullptr);esp_ota_handle_t ota;if(!target||esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&ota)!=ESP_OK){return reply(r,500,"Nie mozna otworzyc nieaktywnego obrazu.");}

 psa_hash_operation_t sha=PSA_HASH_OPERATION_INIT;bool hash_ready=psa_hash_setup(&sha,PSA_ALG_SHA_256)==PSA_SUCCESS;size_t remaining=h.image_bytes;bool ok=hash_ready;while(remaining&&ok){size_t n=remaining>kChunkBytes?kChunkBytes:remaining;if(!receive(r,buffer.get(),n)||esp_ota_write(ota,buffer.get(),n)!=ESP_OK){ok=false;break;}if(psa_hash_update(&sha,buffer.get(),n)!=PSA_SUCCESS){ok=false;break;}remaining-=n;}uint8_t digest[32];size_t digest_length=0;if(psa_hash_finish(&sha,digest,sizeof(digest),&digest_length)!=PSA_SUCCESS||digest_length!=32)ok=false;psa_hash_abort(&sha);
 if(!ok||memcmp(digest,h.sha,32)){ESP_LOGW(kTag,"Transfer or hash failed, %u image bytes remaining",(unsigned)remaining);esp_ota_abort(ota);return reply(r,400,"Przerwano transfer lub suma obrazu jest bledna. Poprzednia aplikacja pozostaje aktywna.");}
 if(const auto error=esp_ota_end(ota);error!=ESP_OK){ESP_LOGW(kTag,"Image validation failed: %s",esp_err_to_name(error));return reply(r,400,"Obraz ESP32-S3 nie przeszedl weryfikacji.");}
if(const auto error=esp_ota_set_boot_partition(target);error!=ESP_OK){ESP_LOGW(kTag,"Boot selection failed: %s",esp_err_to_name(error));return reply(r,500,"Nie zatwierdzono nowego obrazu.");}
ESP_LOGI(kTag,"Image validated and selected for restart: %s",target->label);
auto e=reply(r,200,"Podpis i obraz poprawne. Restart do nowej wersji; aktywna kontrola startu i rollback.");restart_later();return e;}
esp_err_t rollback(httpd_req_t *r){if(mutation_busy()){return reply(r,409,"Bramka zajeta.");}
struct Guard{~Guard(){mutation_done();}} guard;auto *j=body_json(r,256);const auto *v=j?cJSON_GetObjectItemCaseSensitive(j,"confirm"):nullptr;bool confirmed=cJSON_IsString(v)&&!strcmp(v->valuestring,"ROLLBACK");cJSON_Delete(j);if(!confirmed||quick_add_pairing_busy()){return reply(r,400,"Wymagane jawne potwierdzenie ROLLBACK.");}
const auto *running=esp_ota_get_running_partition();const auto *target=esp_ota_get_next_update_partition(running);esp_ota_img_states_t state;
 if(!target||esp_ota_get_state_partition(target,&state)!=ESP_OK||state!=ESP_OTA_IMG_VALID){target=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,"factory");}
if(!target||target==running){return reply(r,409,"Brak poprzedniego potwierdzonego obrazu.");}
if(esp_ota_set_boot_partition(target)!=ESP_OK){return reply(r,500,"Nie wybrano poprzedniej aplikacji.");}
auto e=reply(r,200,"Wybrano poprzedni obraz lub obraz odzyskiwania. NVS i historia bez zmian.");restart_later();return e;}
void ota_health(bool s3_healthy,bool c6_ready){
 static bool completed=false;
 if(completed)return;
 esp_ota_img_states_t state;
 const auto *p=esp_ota_get_running_partition();
 if(esp_ota_get_state_partition(p,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY){completed=true;return;}
 int64_t up=esp_timer_get_time();
 // Validate at 90s when both S3 RAM and C6 RCP are healthy,
 // or at 180s if S3 itself is healthy (so a detached/hung external C6 doesn't falsely rollback good S3 code).
 // If even S3 itself fails health checks past 240s, automatically rollback to the previous valid slot.
 if((up>90000000&&s3_healthy&&c6_ready) || (up>180000000&&s3_healthy)){
  if(esp_ota_mark_app_valid_cancel_rollback()==ESP_OK){
   ESP_LOGI(kTag,"OTA image confirmed VALID at %llds (s3=%d c6=%d)",(long long)(up/1000000),(int)s3_healthy,(int)c6_ready);
   completed=true;
  }
 } else if(up>240000000){
  ESP_LOGE(kTag,"OTA image failed S3 health check at %llds; triggering rollback",(long long)(up/1000000));
  completed=true;
  esp_ota_mark_app_invalid_rollback_and_reboot();
 }
}
}
