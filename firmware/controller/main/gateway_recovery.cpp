#include "gateway_services.h"
#include "gateway_formats.h"
#include "quick_add.h"
#include "supla_config.h"
#include <esp_partition.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <psa/crypto.h>
#include <mbedtls/base64.h>
#include <esp_attr.h>
#include <esp_log.h>
#include <memory>
#include <cstring>
namespace gateway {namespace {
// Flash writes only clear bits: pending -> done / failed are both reachable.
constexpr uint32_t pending=0xfffffffe,done=0xfffffffc,failed=0xfffffff8;
constexpr uint32_t kAttemptMagic=0x4d32524eu,kMaxAttempts=3;
constexpr const char *kTag="gateway_restore";
// Survives panic/software resets (not power loss): bounds restore retries.
RTC_NOINIT_ATTR uint32_t s_attempt_magic,s_attempts;
struct Stage {BackupHeader header;uint32_t state;};
struct Lease {bool owned;Lease():owned(!mutation_busy()){}~Lease(){if(owned)mutation_done();}};
const esp_partition_t *nvs_part(){return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_NVS,"nvs");}
const esp_partition_t *stage_part(){return esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_ANY,"recovery");}
bool digest(const void *p,size_t n,uint8_t out[32]){size_t written=0;return psa_crypto_init()==PSA_SUCCESS&&psa_hash_compute(PSA_ALG_SHA_256,(const uint8_t*)p,n,out,32,&written)==PSA_SUCCESS&&written==32;}
bool same_target(const BackupHeader &h){supla_cfg_t c{};uint8_t mac[6];return supla_config_copy(&c)==ESP_OK&&esp_read_mac(mac,ESP_MAC_WIFI_STA)==ESP_OK&&!memcmp(mac,h.mac,6)&&!memcmp(c.guid,h.guid,16);}
}
esp_err_t backup(httpd_req_t *req){Lease lease;if(!lease.owned||quick_add_pairing_busy()){return reply(req,409,"Bramka jest zajeta. Poczekaj na zakonczenie operacji.");}
const auto *p=nvs_part();if(!p||p->size!=kNvsBytes||p->encrypted){return reply(req,409,"Nieobslugiwany format NVS.");}

 const size_t bytes=sizeof(BackupHeader)+p->size;auto image=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[bytes]);auto check=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[p->size]);if(!image||!check){return reply(req,503,"Za malo pamieci.");}
auto &h=*(BackupHeader*)image.get();h={};memcpy(h.magic,"M2SBK03",8);h.version=3;h.bytes=p->size;supla_cfg_t c{};if(supla_config_copy(&c)!=ESP_OK){return reply(req,500,"Brak tozsamosci.");}
memcpy(h.guid,c.guid,16);if(esp_read_mac(h.mac,ESP_MAC_WIFI_STA)!=ESP_OK){return reply(req,500,"Nie odczytano identyfikatora plytki.");}
 if(esp_partition_read(p,0,image.get()+sizeof(h),p->size)!=ESP_OK||esp_partition_read(p,0,check.get(),p->size)!=ESP_OK||memcmp(image.get()+sizeof(h),check.get(),p->size)){return reply(req,409,"NVS zmienilo sie podczas odczytu. Ponow kopie.");}
if(!digest(check.get(),p->size,h.sha)){return reply(req,500,"Blad SHA256. Kopia nie zostala utworzona.");}
 size_t length=0;mbedtls_base64_encode(nullptr,0,&length,image.get(),bytes);auto encoded=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[length+1]);if(!encoded){return reply(req,503,"Za malo pamieci.");}
if(mbedtls_base64_encode(encoded.get(),length+1,&length,image.get(),bytes)){return reply(req,500,"Blad kodowania.");}
encoded[length]=0;
 auto *j=cJSON_CreateObject();if(!j){return ESP_ERR_NO_MEM;}
cJSON_AddBoolToObject(j,"ok",true);cJSON_AddNumberToObject(j,"format",3);cJSON_AddStringToObject(j,"image",(char*)encoded.get());cJSON_AddStringToObject(j,"target","Same S3, same SUPLA identity; NVS with Matter fabrics, Thread and secrets");uint64_t nodes[32];cJSON_AddNumberToObject(j,"devices",quick_add_devices_copy(nodes,32));char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);memset(image.get(),0,bytes);memset(check.get(),0,p->size);if(!text){return ESP_ERR_NO_MEM;}
httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");auto e=httpd_resp_sendstr(req,text);memset(text,0,strlen(text));cJSON_free(text);return e;}
esp_err_t restore(httpd_req_t *req){Lease lease;if(!lease.owned||quick_add_pairing_busy()){return reply(req,409,"Bramka jest zajeta.");}
const auto *stage=stage_part(),*original=nvs_part();if(!stage||!original||original->size!=kNvsBytes||original->encrypted){return reply(req,409,"Najpierw wymagana jest migracja partycji do wydania0.3.");}
auto *j=body_json(req,100000);if(!j){return reply(req,400,"Niepoprawna kopia.");}
auto *value=cJSON_GetObjectItemCaseSensitive(j,"image");const char *b64=cJSON_IsString(value)?value->valuestring:nullptr;size_t n=0;const size_t capacity=sizeof(BackupHeader)+kNvsBytes;auto image=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[capacity]);bool valid=b64&&strlen(b64)<50000&&image&&mbedtls_base64_decode(image.get(),capacity,&n,(const uint8_t*)b64,strlen(b64))==0;cJSON_Delete(j);
 if(!valid||n<sizeof(BackupHeader)){return reply(req,400,"Uszkodzona kopia.");}
auto h=*(BackupHeader*)image.get();uint8_t sha[32];if(!digest(image.get()+sizeof(h),n-sizeof(h),sha)){return reply(req,500,"Blad SHA256. NVS bez zmian.");}if(!backup_header(h,n)||!same_target(h)||memcmp(sha,h.sha,32)){return reply(req,409,"Kopia nie pasuje do tej plytki/tozsamosci lub jest uszkodzona.");}

 if(esp_partition_erase_range(stage,0,stage->size)!=ESP_OK||esp_partition_write(stage,4096,image.get()+sizeof(h),h.bytes)!=ESP_OK){return reply(req,500,"Nie zapisano kopii roboczej. Biezace NVS bez zmian.");}

 esp_partition_t check=*stage;check.address+=4096;check.size=h.bytes;check.type=ESP_PARTITION_TYPE_DATA;check.subtype=ESP_PARTITION_SUBTYPE_DATA_NVS;strncpy(check.label,"restorecheck",sizeof(check.label));
 // The SUPLA identity lives in "cfg_v1" (legacy key "guid" before 0.3); both are accepted.
 esp_err_t init=nvs_flash_init_partition_ptr(&check);bool identity=false;if(init==ESP_OK){uint8_t guid[16];identity=supla_config_guid_from_partition(check.label,guid)==ESP_OK&&!memcmp(guid,h.guid,16);memset(guid,0,sizeof(guid));nvs_flash_deinit_partition(check.label);}
 if(!identity){return reply(req,409,"Niepoprawny magazyn NVS lub tozsamosc. Odtwarzanie anulowane.");}

 if(esp_partition_read(stage,4096,image.get(),h.bytes)!=ESP_OK){return reply(req,500,"Blad weryfikacji kopii roboczej.");}
// Compare the read-back staging copy with the backup's own hash (never overwrite h.sha).
uint8_t staged[32];if(!digest(image.get(),h.bytes,staged)||memcmp(staged,h.sha,32)){return reply(req,500,"Blad SHA256 kopii roboczej. NVS bez zmian.");}memset(image.get(),0,capacity);Stage record{h,pending};if(esp_partition_write(stage,0,&record,sizeof(record))!=ESP_OK){return reply(req,500,"Nie zatwierdzono odtwarzania.");}
auto result=reply(req,200,"Kopia sprawdzona. Bramka zrestartuje sie i odtworzy NVS przed uruchomieniem Matter.");restart_later();return result;}
esp_err_t apply_pending_restore(){const auto *p=stage_part(),*target=nvs_part();if(!p||!target){return ESP_OK;}
Stage s{};if(esp_partition_read(p,0,&s,sizeof(s))!=ESP_OK){return ESP_FAIL;}
if(s.state!=pending||!backup_header(s.header,sizeof(BackupHeader)+target->size)){s_attempt_magic=0;return ESP_OK;}
auto finish=[p](uint32_t state){s_attempt_magic=0;return esp_partition_write(p,offsetof(Stage,state),&state,sizeof(state));};

 auto data=std::unique_ptr<uint8_t[]>(new(std::nothrow)uint8_t[s.header.bytes]);if(!data){return ESP_ERR_NO_MEM;}
uint8_t sha[32];if(esp_partition_read(p,4096,data.get(),s.header.bytes)!=ESP_OK){return ESP_FAIL;}
// A corrupt staging copy is abandoned before the live NVS is touched.
if(!digest(data.get(),s.header.bytes,sha)){return ESP_FAIL;}if(memcmp(sha,s.header.sha,32)){ESP_LOGE(kTag,"Staged backup corrupt; restore abandoned, NVS unchanged");finish(failed);return ESP_ERR_INVALID_CRC;}

 // Pending stays intact across power loss and is retried; panic/reset loops are
 // bounded to kMaxAttempts, after which NVS is left blank (boots to setup AP) so
 // the gateway never loops on a half-written NVS. The backup file still exists.
 if(s_attempt_magic!=kAttemptMagic){s_attempt_magic=kAttemptMagic;s_attempts=0;}
 for(;;){
  if(++s_attempts>kMaxAttempts){ESP_LOGE(kTag,"Restore failed %u times; clearing NVS and giving up",(unsigned)kMaxAttempts);esp_partition_erase_range(target,0,target->size);memset(data.get(),0,s.header.bytes);finish(failed);return ESP_ERR_INVALID_STATE;}
  esp_err_t e=esp_partition_read(p,4096,data.get(),s.header.bytes);
  if(e==ESP_OK)e=esp_partition_erase_range(target,0,target->size);
  if(e==ESP_OK)e=esp_partition_write(target,0,data.get(),s.header.bytes);
  if(e==ESP_OK)e=esp_partition_read(target,0,data.get(),s.header.bytes);
  const bool verified=e==ESP_OK&&digest(data.get(),s.header.bytes,sha)&&!memcmp(sha,s.header.sha,32);
  memset(data.get(),0,s.header.bytes);
  if(verified){ESP_LOGI(kTag,"NVS restored from verified backup");return finish(done);}
  ESP_LOGW(kTag,"Restore attempt %u failed (%s)",(unsigned)s_attempts,esp_err_to_name(e));
 }}
}
