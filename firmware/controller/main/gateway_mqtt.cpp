#include "gateway_services.h"
#include "device_telemetry.h"
#include "quick_add.h"
#include "supla_config.h"
#include <mqtt_client.h>
#include <esp_attr.h>
#include <esp_crt_bundle.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_crc.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <atomic>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
extern "C" esp_err_t supla_bridge_mqtt_command(uint64_t,uint16_t,bool,bool);
extern "C" uint32_t supla_bridge_stale_seconds(uint64_t,bool);
namespace gateway {namespace {
// Scale64: large MQTT config and tracking tables live in PSRAM and are guarded by a FreeRTOS mutex (no IRQ disabling).
struct Config {bool enabled=false;char uri[181]{},user[81]{},password[129]{},ca[4097]{};};
EXT_RAM_BSS_ATTR Config desired,running;
DRAM_ATTR StaticSemaphore_t lock_storage;
SemaphoreHandle_t lock_handle=nullptr;
portMUX_TYPE lock_init_mux=portMUX_INITIALIZER_UNLOCKED;
struct MqttLock {
 MqttLock() {
  if(!lock_handle) {
   portENTER_CRITICAL(&lock_init_mux);
   if(!lock_handle)lock_handle=xSemaphoreCreateMutexStatic(&lock_storage);
   portEXIT_CRITICAL(&lock_init_mux);
  }
  xSemaphoreTake(lock_handle,portMAX_DELAY);
 }
 ~MqttLock(){xSemaphoreGive(lock_handle);}
};
uint32_t generation=1,applied=0;esp_mqtt_client_handle_t client=nullptr;std::atomic<bool> connected{false},rediscover{false};char base[64]{},availability[88]{};QueueHandle_t commands=nullptr;
struct Command {uint64_t node;uint16_t ep;bool on;bool from_mqtt=false;bool from_supla=false;};
struct Track {uint64_t node=0;uint16_t ep=0;uint8_t metric=0;bool seen=false;uint32_t config_hash=0;int64_t raw=0,last_publish=0;bool last_valid=false,last_available=false;};
constexpr size_t kMaxTracked=256,kMaxNodes=64;
EXT_RAM_BSS_ATTR Track tracked[kMaxTracked];
struct Description {const char *key,*name,*unit,*device_class,*state_class;uint32_t divisor;};
const Description descriptions[]={{"temperature","Temperatura","°C","temperature","measurement",100},{"humidity","Wilgotność","%","humidity","measurement",100},{"battery","Bateria","%","battery","measurement",2},{"onoff","Zasilanie","",nullptr,nullptr,1},{"voltage","Napięcie","V","voltage","measurement",1000},{"current","Prąd","A","current","measurement",1000},{"active_power","Moc czynna","W","power","measurement",1000},{"imported_energy","Energia pobrana","kWh","energy","total_increasing",1000000},{"exported_energy","Energia oddana","kWh","energy","total_increasing",1000000}};
bool uri_valid(const char *s){if(!s){return false;}
size_t n=strlen(s);if(n<9||n>180||strpbrk(s,"@?#% \r\n\t")){return false;}
return !strncmp(s,"mqtt://",7)||!strncmp(s,"mqtts://",8);}
bool publish(const char *topic,const char *payload,bool retained=true){return client&&connected.load()&&esp_mqtt_client_enqueue(client,topic,payload,0,1,retained,false)>=0;}
constexpr char kTopicPrefix[]="matter2supla/";
const char *installation_id(){return base+sizeof(kTopicPrefix)-1;}
void config_topic(const Track &t,char *out,size_t n){char installation[24];snprintf(installation,sizeof(installation),"%s",installation_id());snprintf(out,n,"homeassistant/%s/m2s_%s_%llu_%u_%u/config",t.metric==3?"switch":"sensor",installation,(unsigned long long)t.node,t.ep,t.metric);}
void event(void*,esp_event_base_t,int32_t id,void *data){auto *e=(esp_mqtt_event_handle_t)data;if(id==MQTT_EVENT_CONNECTED){connected=true;rediscover=true;esp_mqtt_client_subscribe(client,(std::string(base)+"/node/+/ep/+/set").c_str(),1);}else if(id==MQTT_EVENT_DISCONNECTED)connected=false;
 else if(id==MQTT_EVENT_DATA){if(e->retain||e->current_data_offset||e->data_len!=e->total_data_len||e->topic_len<=0||e->topic_len>=150||e->data_len<2||e->data_len>3)return;char topic[150]{};memcpy(topic,e->topic,e->topic_len);size_t prefix=strlen(base);if(strncmp(topic,base,prefix))return;unsigned long long node=0;unsigned ep=0;int used=0;if(sscanf(topic+prefix,"/node/%llu/ep/%u/set%n",&node,&ep,&used)!=2||used!=(int)strlen(topic+prefix)||!node||ep<1||ep>=65535)return;bool on=e->data_len==2&&!memcmp(e->data,"ON",2),off=e->data_len==3&&!memcmp(e->data,"OFF",3);if(!on&&!off)return;Command command{node,(uint16_t)ep,on,true};if(commands&&xQueueSend(commands,&command,0)==pdTRUE)wake_gateway_worker();}}
Track *track(const device_telemetry::Reading &r){for(auto &t:tracked)if(t.node==r.node&&t.ep==r.endpoint&&t.metric==(uint8_t)r.metric){return &t;}
for(auto &t:tracked){if(!t.node){t.node=r.node;t.ep=r.endpoint;t.metric=(uint8_t)r.metric;return &t;}}
return nullptr;}
void decimal(int64_t raw,uint32_t divisor,char *out,size_t n){uint64_t magnitude=raw<0?uint64_t(-(raw+1))+1:uint64_t(raw);uint64_t whole=magnitude/divisor,rem=magnitude%divisor;if(divisor==2)snprintf(out,n,"%s%llu.%u",raw<0?"-":"",(unsigned long long)whole,rem?5:0);else{unsigned digits=0;for(uint32_t d=divisor;d>1;d/=10)++digits;if(!digits)snprintf(out,n,"%s%llu",raw<0?"-":"",(unsigned long long)whole);else snprintf(out,n,"%s%llu.%0*llu",raw<0?"-":"",(unsigned long long)whole,digits,(unsigned long long)rem);}}

// Fast lightweight hash of discovery inputs so we don't allocate/serialize cJSON trees every 2s.
uint32_t compute_discovery_hash(uint64_t node, uint16_t ep, unsigned metric, const char *name, const char *base_topic) {
 uint32_t h = esp_crc32_le(0, reinterpret_cast<const uint8_t*>(&node), sizeof(node));
 h = esp_crc32_le(h, reinterpret_cast<const uint8_t*>(&ep), sizeof(ep));
 h = esp_crc32_le(h, reinterpret_cast<const uint8_t*>(&metric), sizeof(metric));
 if (name) h = esp_crc32_le(h, reinterpret_cast<const uint8_t*>(name), strlen(name));
 if (base_topic) h = esp_crc32_le(h, reinterpret_cast<const uint8_t*>(base_topic), strlen(base_topic));
 return h ? h : 1;
}
}
esp_err_t relay_enqueue(uint64_t node,uint16_t ep,bool on,bool from_supla){
 if(!commands)return ESP_ERR_NO_MEM;
 if(!quick_add_device_exists(node)||!ep||ep==0xffff)return ESP_ERR_INVALID_ARG;
 if(quick_add_pairing_busy())return ESP_ERR_INVALID_STATE;
 if(!from_supla){
  device_telemetry::Reading proof;
  if(!device_telemetry::lookup(node,ep,device_telemetry::Metric::OnOff,proof)||!proof.valid)return ESP_ERR_NOT_SUPPORTED;
 }
 Command command{node,ep,on,false,from_supla};
 if(xQueueSend(commands,&command,0)!=pdTRUE)return ESP_ERR_TIMEOUT;
 wake_gateway_worker();
 return ESP_OK;
}
void mqtt_init(){commands=xQueueCreate(16,sizeof(Command));nvs_handle_t h;if(nvs_open("m2s_mqtt",NVS_READONLY,&h)==ESP_OK){size_t n=0;if(nvs_get_str(h,"config",nullptr,&n)==ESP_OK&&n<8192){auto text=std::unique_ptr<char[]>(new(std::nothrow)char[n]);if(text&&nvs_get_str(h,"config",text.get(),&n)==ESP_OK){auto *j=cJSON_Parse(text.get());if(j){auto *en=cJSON_GetObjectItem(j,"enabled");desired.enabled=cJSON_IsTrue(en);for(auto item:{std::pair<const char*,char*>{"uri",desired.uri},{"user",desired.user},{"password",desired.password},{"ca",desired.ca}}){auto *v=cJSON_GetObjectItem(j,item.first);if(cJSON_IsString(v)){size_t limit=!strcmp(item.first,"uri")?sizeof(desired.uri):!strcmp(item.first,"user")?sizeof(desired.user):!strcmp(item.first,"password")?sizeof(desired.password):sizeof(desired.ca);snprintf(item.second,limit,"%s",v->valuestring);}}cJSON_Delete(j);}memset(text.get(),0,n);}}nvs_close(h);}if(desired.enabled&&!uri_valid(desired.uri))desired.enabled=false;supla_cfg_t c{};if(supla_config_copy(&c)==ESP_OK){char hex[33];supla_config_bytes_to_hex(c.guid,hex);snprintf(base,sizeof(base),"%s%.12s",kTopicPrefix,hex);}snprintf(availability,sizeof(availability),"%s/availability",base);}
esp_err_t mqtt_get(httpd_req_t *r){auto c=std::unique_ptr<Config>(new(std::nothrow)Config);if(!c){return reply(r,503,"Za malo pamieci.");}
{MqttLock guard;*c=desired;}auto *j=cJSON_CreateObject();if(!j){return ESP_ERR_NO_MEM;}
cJSON_AddBoolToObject(j,"enabled",c->enabled);cJSON_AddBoolToObject(j,"connected",connected.load());cJSON_AddStringToObject(j,"uri",c->uri);cJSON_AddStringToObject(j,"user",c->user);cJSON_AddStringToObject(j,"ca",c->ca);cJSON_AddBoolToObject(j,"password_set",c->password[0]);cJSON_AddStringToObject(j,"topic",base);char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);memset(static_cast<void*>(c.get()),0,sizeof(Config));if(!text){return ESP_ERR_NO_MEM;}
httpd_resp_set_type(r,"application/json");auto e=httpd_resp_sendstr(r,text);cJSON_free(text);return e;}
esp_err_t mqtt_save(httpd_req_t *r){if(mutation_busy()){return reply(r,409,"Bramka zajeta.");}
struct Guard{~Guard(){mutation_done();}} guard;auto *j=body_json(r,8192);if(!j){return reply(r,400,"Niepoprawny JSON.");}
auto c=std::unique_ptr<Config>(new(std::nothrow)Config);if(!c){cJSON_Delete(j);return reply(r,503,"Za malo pamieci.");}{MqttLock mguard;*c=desired;}char previous_uri[181];snprintf(previous_uri,sizeof(previous_uri),"%s",c->uri);bool valid=cJSON_IsBool(cJSON_GetObjectItem(j,"enabled"));c->enabled=cJSON_IsTrue(cJSON_GetObjectItem(j,"enabled"));for(auto item:{std::pair<const char*,char*>{"uri",c->uri},{"user",c->user},{"password",c->password},{"ca",c->ca}}){auto *v=cJSON_GetObjectItem(j,item.first);size_t limit=!strcmp(item.first,"uri")?sizeof(c->uri):!strcmp(item.first,"user")?sizeof(c->user):!strcmp(item.first,"password")?sizeof(c->password):sizeof(c->ca);if(v){if(!cJSON_IsString(v)||strlen(v->valuestring)>=limit)valid=false;else if(strcmp(item.first,"password")||v->valuestring[0])snprintf(item.second,limit,"%s",v->valuestring);}}
 auto *new_password=cJSON_GetObjectItem(j,"password");if(strcmp(previous_uri,c->uri)&&(!cJSON_IsString(new_password)||!new_password->valuestring[0]))c->password[0]=0;
 cJSON_Delete(j);if(c->enabled&&!uri_valid(c->uri))valid=false;if(!valid){memset(static_cast<void*>(c.get()),0,sizeof(Config));return reply(r,400,"Niepoprawny adres lub pola MQTT. Hasla nie umieszczaj w adresie URI.");}
 auto *store=cJSON_CreateObject();if(!store){return reply(r,503,"Za malo pamieci.");}
cJSON_AddBoolToObject(store,"enabled",c->enabled);cJSON_AddStringToObject(store,"uri",c->uri);cJSON_AddStringToObject(store,"user",c->user);cJSON_AddStringToObject(store,"password",c->password);cJSON_AddStringToObject(store,"ca",c->ca);char *text=cJSON_PrintUnformatted(store);cJSON_Delete(store);nvs_handle_t h;esp_err_t e=ESP_FAIL;if(text&&nvs_open("m2s_mqtt",NVS_READWRITE,&h)==ESP_OK){e=nvs_set_str(h,"config",text);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}if(text){memset(text,0,strlen(text));cJSON_free(text);}if(e==ESP_OK){MqttLock mguard;desired=*c;++generation;}memset(static_cast<void*>(c.get()),0,sizeof(Config));return reply(r,e==ESP_OK?200:500,e==ESP_OK?"Ustawienia MQTT zapisane. Bramka polaczy sie bez restartu.":"Nie zapisano konfiguracji MQTT.");}
void mqtt_tick(){Command command;while(commands&&xQueueReceive(commands,&command,0)==pdTRUE){if(command.from_mqtt&&!connected.load())continue;
 const auto result=quick_add_pairing_busy()?ESP_ERR_INVALID_STATE:supla_bridge_mqtt_command(command.node,command.ep,command.on,!command.from_supla);
 if(!command.from_mqtt) {
  const char *origin = command.from_supla ? "SUPLA" : "WebUI";
  if(result==ESP_OK)ESP_LOGI("gateway_relay","%s OnOff=%d submitted to Matter (node %llu, ep %u)",origin,command.on,static_cast<unsigned long long>(command.node),static_cast<unsigned>(command.ep));
  else ESP_LOGW("gateway_relay","%s OnOff=%d rejected before completion (node %llu, ep %u): %s",origin,command.on,static_cast<unsigned long long>(command.node),static_cast<unsigned>(command.ep),esp_err_to_name(result));
 }}uint32_t gen;{MqttLock mguard;gen=generation;}if(gen!=applied){if(client){publish(availability,"offline");esp_mqtt_client_stop(client);esp_mqtt_client_destroy(client);client=nullptr;connected=false;}{MqttLock mguard;running=desired;applied=gen;}if(running.enabled){esp_mqtt_client_config_t c{};c.broker.address.uri=running.uri;c.credentials.username=running.user;c.credentials.authentication.password=running.password;c.session.last_will.topic=availability;c.session.last_will.msg="offline";c.session.last_will.qos=1;c.session.last_will.retain=1;c.outbox.limit=65536;if(!strncmp(running.uri,"mqtts://",8)){if(running.ca[0])c.broker.verification.certificate=running.ca;else c.broker.verification.crt_bundle_attach=esp_crt_bundle_attach;}client=esp_mqtt_client_init(&c);if(client){esp_mqtt_client_register_event(client,MQTT_EVENT_ANY,event,nullptr);esp_mqtt_client_start(client);}}}
 if(!client||!connected.load()){return;}
if(rediscover.exchange(false)){for(auto &t:tracked){t.config_hash=0;t.last_publish=0;}publish(availability,"online");}
 for(auto &t:tracked){t.seen=false;}
 uint64_t nodes[kMaxNodes];unsigned count=quick_add_devices_copy(nodes,kMaxNodes);auto readings=std::unique_ptr<device_telemetry::Reading[]>(new(std::nothrow)device_telemetry::Reading[kMaxTracked]);if(!readings)return;const int64_t now=esp_timer_get_time();
 for(unsigned i=0;i<count;++i){device_telemetry::Contact contact;bool truncated=false;size_t n=device_telemetry::copy(nodes[i],readings.get(),kMaxTracked,contact,truncated);char name[40]{};quick_add_device_name_copy(nodes[i],name,sizeof(name));
  for(size_t k=0;k<n;++k){const auto &r=readings[k];unsigned metric=(unsigned)r.metric;if(metric>=sizeof(descriptions)/sizeof(descriptions[0]))continue;auto *t=track(r);if(!t)continue;t->seen=true;const auto &d=descriptions[metric];char topic[160],available[180],state[180],cfg_topic[160];snprintf(topic,sizeof(topic),"%s/node/%llu/ep/%u/%s",base,(unsigned long long)r.node,r.endpoint,d.key);snprintf(available,sizeof(available),"%s/availability",topic);snprintf(state,sizeof(state),"%s/state",topic);
   // Only build and serialize cJSON discovery payload when fast input hash changes.
   const uint32_t fast_hash = compute_discovery_hash(r.node, r.endpoint, metric, name, base);
   if (fast_hash != t->config_hash) {
    char unique[100],device_id[90];
    config_topic(*t,cfg_topic,sizeof(cfg_topic));
    snprintf(unique,sizeof(unique),"m2s_%s_%llu_%u_%u",installation_id(),(unsigned long long)r.node,r.endpoint,metric);
    snprintf(device_id,sizeof(device_id),"m2s_%s_%llu",installation_id(),(unsigned long long)r.node);
    auto *j=cJSON_CreateObject();
    if(j){
     cJSON_AddStringToObject(j,"name",d.name);cJSON_AddStringToObject(j,"unique_id",unique);cJSON_AddStringToObject(j,"state_topic",state);auto *a=cJSON_AddArrayToObject(j,"availability");for(const char *av:{availability,available}){auto *o=cJSON_CreateObject();cJSON_AddStringToObject(o,"topic",av);cJSON_AddItemToArray(a,o);}cJSON_AddStringToObject(j,"availability_mode","all");auto *device=cJSON_AddObjectToObject(j,"device");auto *ids=cJSON_AddArrayToObject(device,"identifiers");cJSON_AddItemToArray(ids,cJSON_CreateString(device_id));cJSON_AddStringToObject(device,"name",name);cJSON_AddStringToObject(device,"manufacturer","Matter2Supla bridge");
     if(metric==3){char cmd[150];snprintf(cmd,sizeof(cmd),"%s/node/%llu/ep/%u/set",base,(unsigned long long)r.node,r.endpoint);cJSON_AddStringToObject(j,"command_topic",cmd);cJSON_AddStringToObject(j,"payload_on","ON");cJSON_AddStringToObject(j,"payload_off","OFF");cJSON_AddStringToObject(j,"state_on","1");cJSON_AddStringToObject(j,"state_off","0");}else{cJSON_AddStringToObject(j,"unit_of_measurement",d.unit);cJSON_AddStringToObject(j,"device_class",d.device_class);cJSON_AddStringToObject(j,"state_class",d.state_class);}
     char *encoded=cJSON_PrintUnformatted(j);cJSON_Delete(j);
     if(encoded){if(publish(cfg_topic,encoded))t->config_hash=fast_hash;cJSON_free(encoded);}
    }
   }
   bool alive=contact.active&&contact.report&&now-contact.report<=((int64_t)contact.maximum+90)*1000000;bool fresh=alive||(r.received&&now-r.received<=(int64_t)supla_bridge_stale_seconds(r.node,metric==3)*1000000);bool available_now=r.valid&&fresh;
   if(!t->last_publish||now-t->last_publish>60000000||t->raw!=r.raw||t->last_valid!=r.valid||t->last_available!=available_now){bool queued=publish(available,available_now?"online":"offline");if(available_now){char value[64];decimal(r.raw,d.divisor,value,sizeof(value));queued=publish(state,value)&&queued;}if(queued){t->last_publish=now;t->raw=r.raw;t->last_valid=r.valid;t->last_available=available_now;}}
  }}
 // Only remove retained Discovery belonging to nodes removed from our registry.
 for(auto &t:tracked)if(t.node&&!quick_add_device_exists(t.node)){char topic[160];config_topic(t,topic,sizeof(topic));publish(topic,"");t={};}
}
}
