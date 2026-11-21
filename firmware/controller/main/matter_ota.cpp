#include "matter_ota.h"
#include "matter_discovery.h"
#include "quick_add.h"
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_ota_provider.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>
#include <memory>
namespace matter_ota {
namespace {
using esp_matter::ota_provider::EspOtaProvider;
struct State {uint64_t node=0;uint32_t current=0,approved=0;char text[40]{};bool has_current=false;int64_t checked=0;};
State states[64]; // Scale64 registry-bound storage
const State *find_state(uint64_t node) {
    if (!node) return nullptr;
    for(const auto &s:states)if(s.node==node)return &s;
    return nullptr;
}
State *state(uint64_t node) {
    if (!node || !quick_add_device_exists(node)) return nullptr;
    for(auto &s:states)if(s.node==node)return &s;
    for(auto &s:states)if(!s.node){s.node=node;return &s;}
    return nullptr;
}
chip::ScopedNodeId peer(uint64_t node) {
    auto *comm = esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
    return chip::ScopedNodeId(node, comm ? comm->GetFabricIndex() : chip::kUndefinedFabricIndex);
}
int requestor_endpoint(uint64_t node) {
    std::unique_ptr<cJSON,decltype(&cJSON_Delete)> s(matter_discovery::snapshot_json(),cJSON_Delete);
    if(!s || cJSON_GetNumberValue(cJSON_GetObjectItem(s.get(),"node"))!=node)return -1;
    const auto *state=cJSON_GetStringValue(cJSON_GetObjectItem(s.get(),"state"));
    if(!state || strcmp(state,"complete"))return -1;
    const auto *eps=cJSON_GetObjectItem(s.get(),"endpoints");
    for(int i=0;i<cJSON_GetArraySize(eps);++i) {
        const auto *ep=cJSON_GetArrayItem(eps,i),*clusters=cJSON_GetObjectItem(ep,"servers");
        for(int j=0;j<cJSON_GetArraySize(clusters);++j)
            if(cJSON_GetNumberValue(cJSON_GetArrayItem(clusters,j))==0x2a)
                return cJSON_GetNumberValue(cJSON_GetObjectItem(ep,"id"));
    }
    return -1;
}
esp_err_t announce(uint64_t node,int endpoint) {
    auto *comm = esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
    if (!comm) return ESP_ERR_INVALID_STATE;
    const auto provider=comm->GetNodeId();
    char payload[192];
    snprintf(payload,sizeof(payload),"{\"0:U64\":\"%llu\",\"1:U16\":%u,\"2:U8\":0,\"4:U16\":0}",
             (unsigned long long)provider,(unsigned)CONFIG_ESP_MATTER_CONTROLLER_VENDOR_ID);
    return esp_matter::controller::send_invoke_cluster_command(node,endpoint,0x2a,0,payload);
}
}
void init(){EspOtaProvider::GetInstance().SetOtaAllowedDefault(false);}
void forget(uint64_t node){
    EspOtaProvider::GetInstance().DisableOtaForNode(peer(node));
    EspOtaProvider::GetInstance().RemoveOtaRequestorEntry(peer(node));
    for(auto &s:states)if(s.node==node)s={};
}
bool candidate_allowed(uint64_t node,uint32_t version){const auto *s=find_state(node);return s && s->approved==version;}
void observed(uint64_t node,uint32_t version,const char *text) {
    auto *s=state(node);if(!s)return;s->current=version;s->has_current=true;
    if(text)snprintf(s->text,sizeof(s->text),"%s",text);
}
esp_err_t check(uint64_t node) {
    if(!quick_add_device_exists(node))return ESP_ERR_NOT_FOUND;
    int ep=requestor_endpoint(node);if(ep<0)return ESP_ERR_NOT_SUPPORTED;
    auto &provider=EspOtaProvider::GetInstance();auto *entry=provider.FindOtaRequestorEntry(peer(node));
    if(entry && (entry->mOtaAllowed || entry->mOtaAllowedOnce))return ESP_ERR_INVALID_STATE;
    auto *s=state(node);if(!s)return ESP_ERR_NO_MEM;
    if(s->checked && esp_timer_get_time()-s->checked<30000000)return ESP_ERR_INVALID_STATE;
    if(entry){entry->mHasNewVersion=false;entry->mSoftwareVersion=0;entry->mSoftwareVersionString[0]=0;}
    auto error=announce(node,ep);if(error==ESP_OK)s->checked=esp_timer_get_time();return error;
}
esp_err_t install(uint64_t node,uint32_t version) {
    if(!quick_add_device_exists(node))return ESP_ERR_NOT_FOUND;
    int ep=requestor_endpoint(node);if(ep<0)return ESP_ERR_NOT_SUPPORTED;
    auto *s=state(node);auto &provider=EspOtaProvider::GetInstance();auto *entry=provider.FindOtaRequestorEntry(peer(node));
    if(!s || !s->has_current || !entry || !entry->mHasNewVersion || entry->mSoftwareVersion!=version || version<=s->current ||
       !s->checked || esp_timer_get_time()-s->checked>600000000)return ESP_ERR_INVALID_STATE;
    // Only the checked node may perform one update. Other requestors stay denied.
    s->approved=version;entry->mHasNewVersion=false;
    auto error=provider.EnableOtaForNode(peer(node),true);if(error!=ESP_OK)return error;
    error=announce(node,ep);if(error!=ESP_OK)provider.DisableOtaForNode(peer(node));return error;
}
cJSON *snapshot(uint64_t node) {
    auto *out=cJSON_CreateObject();if(!out)return nullptr;
    const auto *s=find_state(node);auto *entry=EspOtaProvider::GetInstance().FindOtaRequestorEntry(peer(node));
    const bool available=entry && entry->mHasNewVersion && s && s->has_current && entry->mSoftwareVersion>s->current;
    const bool allowed=entry && (entry->mOtaAllowed || entry->mOtaAllowedOnce);
    const char *status=allowed?"authorized":available?"available":!s || !s->checked?"idle":
        esp_timer_get_time()-s->checked<90000000?"waiting":"no_confirmed_update";
    cJSON_AddBoolToObject(out,"ok",true);cJSON_AddNumberToObject(out,"node",node);
    cJSON_AddStringToObject(out,"state",status);cJSON_AddStringToObject(out,"catalog","CSA DCL MainNet");
    cJSON_AddBoolToObject(out,"supported",requestor_endpoint(node)>=0);
    if(s && s->has_current){cJSON_AddNumberToObject(out,"current_version",s->current);cJSON_AddStringToObject(out,"current_version_string",s->text);}
    if(available){cJSON_AddNumberToObject(out,"available_version",entry->mSoftwareVersion);cJSON_AddStringToObject(out,"available_version_string",entry->mSoftwareVersionString);cJSON_AddNumberToObject(out,"image_bytes",entry->mOtaImageSize);}
    return out;
}
}
