#include "gateway_services.h"
#include "gateway_formats.h"
#include "version.h"
#include "quick_add.h"
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <cstring>
#include <memory>
namespace gateway {namespace {
extern const uint8_t key_start[] asm("_binary_update_public_key_pem_start");
extern const uint8_t key_end[] asm("_binary_update_public_key_pem_end");
bool ready_layout(){return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_OTA,"otadata")&&esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"ota_0")&&esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"ota_1");}
bool receive(httpd_req_t *r,void *out,size_t n){auto *p=(char*)out;unsigned timeout=0;while(n){int got=httpd_req_recv(r,p,n);if(got==HTTPD_SOCK_ERR_TIMEOUT&&timeout++<5)continue;if(got<=0){return false;}
p+=got;n-=got;}return true;}
bool signature(const UpdateHeader &h){mbedtls_pk_context pk;mbedtls_pk_init(&pk);uint8_t hash[32];size_t written=0;bool hashed=psa_crypto_init()==PSA_SUCCESS&&psa_hash_compute(PSA_ALG_SHA_256,(const uint8_t*)&h,offsetof(UpdateHeader,signature_bytes),hash,32,&written)==PSA_SUCCESS&&written==32;bool ok=hashed&&mbedtls_pk_parse_public_key(&pk,key_start,key_end-key_start)==0&&mbedtls_pk_verify(&pk,MBEDTLS_MD_SHA256,hash,32,h.signature,h.signature_bytes)==0;mbedtls_pk_free(&pk);return ok;}
}
esp_err_t update(httpd_req_t *r){if(!ready_layout()){return reply(r,409,"Wymagana jednorazowa migracja partycji0.3, OTA nie zapisuje tablicy partycji.");}
if(mutation_busy()){return reply(r,409,"Bramka jest zajeta.");}
struct Guard{~Guard(){mutation_done();}} guard;if(quick_add_pairing_busy()){return reply(r,409,"Poczekaj na zakonczenie parowania.");}

 UpdateHeader h{};if(r->content_len<sizeof(h)||r->content_len>kAppLimit+sizeof(h)||!receive(r,&h,sizeof(h))||!update_header(h,FW_BUILD,r->content_len)||!signature(h)){return reply(r,400,"Niepoprawny podpis, wersja, model lub rozmiar pakietu. Niczego nie zapisano.");}

 constexpr size_t kChunkBytes = 4096;
 auto buffer = std::unique_ptr<uint8_t[]>(new(std::nothrow) uint8_t[kChunkBytes]);
 if(!buffer){return reply(r,503,"Brak pamieci tymczasowej na bufor OTA.");}

 const auto *target=esp_ota_get_next_update_partition(nullptr);esp_ota_handle_t ota;if(!target||esp_ota_begin(target,OTA_WITH_SEQUENTIAL_WRITES,&ota)!=ESP_OK){return reply(r,500,"Nie mozna otworzyc nieaktywnego obrazu.");}

 psa_hash_operation_t sha=PSA_HASH_OPERATION_INIT;bool hash_ready=psa_hash_setup(&sha,PSA_ALG_SHA_256)==PSA_SUCCESS;size_t remaining=h.image_bytes;bool ok=hash_ready;while(remaining&&ok){size_t n=remaining>kChunkBytes?kChunkBytes:remaining;if(!receive(r,buffer.get(),n)||esp_ota_write(ota,buffer.get(),n)!=ESP_OK){ok=false;break;}if(psa_hash_update(&sha,buffer.get(),n)!=PSA_SUCCESS){ok=false;break;}remaining-=n;}uint8_t digest[32];size_t digest_length=0;if(psa_hash_finish(&sha,digest,sizeof(digest),&digest_length)!=PSA_SUCCESS||digest_length!=32)ok=false;psa_hash_abort(&sha);
 if(!ok||memcmp(digest,h.sha,32)){esp_ota_abort(ota);return reply(r,400,"Przerwano transfer lub suma obrazu jest bledna. Poprzednia aplikacja pozostaje aktywna.");}
 if(esp_ota_end(ota)!=ESP_OK){return reply(r,400,"Obraz ESP32-S3 nie przeszedl weryfikacji.");}
if(esp_ota_set_boot_partition(target)!=ESP_OK){return reply(r,500,"Nie zatwierdzono nowego obrazu.");}
auto e=reply(r,200,"Podpis i obraz poprawne. Restart do nowej wersji; aktywna kontrola startu i rollback.");restart_later();return e;}
esp_err_t rollback(httpd_req_t *r){if(mutation_busy()){return reply(r,409,"Bramka zajeta.");}
struct Guard{~Guard(){mutation_done();}} guard;auto *j=body_json(r,256);const auto *v=j?cJSON_GetObjectItemCaseSensitive(j,"confirm"):nullptr;bool confirmed=cJSON_IsString(v)&&!strcmp(v->valuestring,"ROLLBACK");cJSON_Delete(j);if(!confirmed||quick_add_pairing_busy()){return reply(r,400,"Wymagane jawne potwierdzenie ROLLBACK.");}
const auto *running=esp_ota_get_running_partition();const auto *target=esp_ota_get_next_update_partition(running);esp_ota_img_states_t state;
 if(!target||esp_ota_get_state_partition(target,&state)!=ESP_OK||state!=ESP_OTA_IMG_VALID){target=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_FACTORY,"factory");}
if(!target||target==running){return reply(r,409,"Brak poprzedniego potwierdzonego obrazu.");}
if(esp_ota_set_boot_partition(target)!=ESP_OK){return reply(r,500,"Nie wybrano poprzedniej aplikacji.");}
auto e=reply(r,200,"Wybrano poprzedni obraz lub obraz odzyskiwania. NVS i historia bez zmian.");restart_later();return e;}
void ota_health(bool local_ready){
 static bool completed=false;
 if(completed)return;
 esp_ota_img_states_t state;
 const auto *p=esp_ota_get_running_partition();
 if(esp_ota_get_state_partition(p,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY){completed=true;return;}
 int64_t up=esp_timer_get_time();
 // Validate S3 image once it runs past 90s with healthy internal RAM and RCP contact,
 // or past 180s without panic even if the external C6 radio is temporarily unresponsive.
 if((up>90000000&&local_ready) || up>180000000){
  if(esp_ota_mark_app_valid_cancel_rollback()==ESP_OK)completed=true;
 }
}
}
