#include "channel_proposal.h"

#include "mapping_gate.h"
#include "mapping_nvs.h"
#include "matter_discovery.h"
#include "quick_add.h"
#include "supla_config.h"
#include <esp_timer.h>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <new>

namespace m2s::proposal {
namespace {
using namespace mapping;

class Proof final : public Observations {
public:
    explicit Proof(uint64_t node) : node_(node) { matter_discovery::confirmation(node, confirmation); }
    uint32_t generation() const override { return confirmation.generation; }
    bool supports(const Source &source) const override {
        if (source.node != node_) return false;
        for (size_t i = 0; i < confirmation.count; ++i) {
            const auto &entry = confirmation.endpoints[i];
            if (entry.id == source.endpoint)
                return entry.roles & (1u << static_cast<unsigned>(source.role));
        }
        return false;
    }
    matter_discovery::Confirmation confirmation;
private:
    uint64_t node_;
};

NvsBackend backend;
EspCrypto crypto;
std::unique_ptr<Store> pending;
Ticket ticket;
uint64_t pending_node = 0;
uint8_t pending_guid[16]{};

void hex(const uint8_t *data, size_t length, char *out)
{
    static constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    out[length * 2] = '\0';
}

Error relate_meter(Model &model,uint64_t node,const Proof &proof,const ElectricityMeter &request,bool &changed)
{
    if(!proof.supports({node,request.relay_endpoint,Role::OnOff}) ||
       !proof.supports({node,request.meter_endpoint,Role::ImportedEnergy}))return Error::Discovery;
    const int relay=source_channel(model,node,request.relay_endpoint,Role::OnOff);
    const int meter=source_channel(model,node,request.meter_endpoint,Role::ImportedEnergy);
    if(relay<0 || meter<0 || model.channels[relay].kind!=Kind::Relay ||
       model.channels[meter].kind!=Kind::ElectricityMeter)return Error::Invalid;
    auto &binding=model.channels[relay].related_meter;
    // An explicit EP pair may add a relation, never silently replace one.
    if(binding!=kNone && binding!=meter)return Error::Conflict;
    if(binding==kNone){binding=static_cast<uint16_t>(meter);changed=true;}
    return Error::Ok;
}

Error propose(Model &model, uint64_t node, const Proof &proof, const ElectricityMeter *pair)
{
    bool changed = false;
    for (size_t i = 0; i < proof.confirmation.count; ++i) {
        const auto ep = proof.confirmation.endpoints[i].id;
        const auto existing = [&](Role role) { return source_channel(model, node, ep, role); };
        const auto free_source = [&](Role role) { return existing(role) < 0 && proof.supports({node, ep, role}); };
        const auto append = [&](Kind kind, std::initializer_list<Role> roles) -> int {
            Channel channel{};
            channel.node = node;
            channel.kind = kind;
            bool has_source = false;
            for (auto role : roles) {
                if (!free_source(role)) continue;
                channel.endpoint[static_cast<unsigned>(role)] = ep;
                has_source = true;
            }
            if (!has_source) return -1;
            if (model.count == kMaxChannels) return -2;
            const int index = model.count++;
            model.channels[index] = channel;
            changed = true;
            return index;
        };
        const int relay = append(Kind::Relay, {Role::OnOff});
        const int therm = append(Kind::Thermohygrometer, {Role::Temperature, Role::Humidity});
        if (relay == -2 || therm == -2) return Error::Capacity;
        if (free_source(Role::ImportedEnergy)) {
            const int meter = append(Kind::ElectricityMeter, {Role::Voltage, Role::Current, Role::ActivePower,
                                                            Role::ImportedEnergy, Role::ExportedEnergy});
            if (meter == -2) return Error::Capacity;
            const int switch_channel = existing(Role::OnOff);
            if (meter >= 0 && switch_channel >= 0 && model.channels[switch_channel].related_meter == kNone)
                model.channels[switch_channel].related_meter = static_cast<uint16_t>(meter);
        }
    }
    if(pair) {
        const auto error=relate_meter(model,node,proof,*pair,changed);
        if(error!=Error::Ok)return error;
    }
    if (!changed) return Error::NotFound;
    size_t position = 0;
    while (position < model.policy_count && model.policies[position].node < node) ++position;
    if (position == model.policy_count || model.policies[position].node != node) {
        if (model.policy_count == kMaxPolicies) return Error::Capacity;
        for (size_t j = model.policy_count; j > position; --j) model.policies[j] = model.policies[j - 1];
        model.policies[position] = {node, 15};
        ++model.policy_count;
    }
    if (model.revision == UINT32_MAX) return Error::Conflict;
    ++model.revision;
    return Error::Ok;
}

Error combine(Model &model,uint64_t node,const Proof &proof,const Thermohygrometer &request)
{
    if(!proof.supports({node,request.temperature_endpoint,Role::Temperature}) ||
       !proof.supports({node,request.humidity_endpoint,Role::Humidity}))return Error::Discovery;
    bool changed=false;
    int target=request.channel==kNone?-1:request.channel;
    if(target>=0 && (target>=model.count || model.channels[target].node!=node ||
        model.channels[target].kind!=Kind::Thermohygrometer || model.channels[target].retired))return Error::Invalid;
    const int temp=source_channel(model,node,request.temperature_endpoint,Role::Temperature);
    const int hum=source_channel(model,node,request.humidity_endpoint,Role::Humidity);
    if(target<0)target=temp<0?hum:(hum<0?temp:std::min(temp,hum));
    if(target<0) {
        if(model.count==kMaxChannels)return Error::Capacity;
        target=model.count++;changed=true;
        model.channels[target]={};model.channels[target].node=node;
        model.channels[target].kind=Kind::Thermohygrometer;
    }
    auto &destination=model.channels[target];
    // Never replace an existing, independently confirmed measurement implicitly.
    for(Role role:{Role::Temperature,Role::Humidity}) {
        const auto old=destination.endpoint[static_cast<unsigned>(role)];
        const auto selected=role==Role::Temperature?request.temperature_endpoint:request.humidity_endpoint;
        if(old!=kNone && old!=selected && proof.supports({node,old,role}))return Error::Conflict;
    }
    changed|=destination.endpoint[static_cast<unsigned>(Role::Temperature)]!=request.temperature_endpoint ||
             destination.endpoint[static_cast<unsigned>(Role::Humidity)]!=request.humidity_endpoint;
    destination.endpoint[static_cast<unsigned>(Role::Temperature)]=request.temperature_endpoint;
    destination.endpoint[static_cast<unsigned>(Role::Humidity)]=request.humidity_endpoint;
    for(size_t i=0;i<model.count;i++) {
        auto &channel=model.channels[i];
        if(static_cast<int>(i)==target || (static_cast<int>(i)!=temp && static_cast<int>(i)!=hum) ||
           channel.node!=node || channel.kind!=Kind::Thermohygrometer || channel.retired)continue;
        for(Role role:{Role::Temperature,Role::Humidity}) {
            auto &ep=channel.endpoint[static_cast<unsigned>(role)];
            const auto selected=role==Role::Temperature?request.temperature_endpoint:request.humidity_endpoint;
            if(ep!=kNone && (ep==selected || !proof.supports({node,ep,role}))){ep=kNone;changed=true;}
        }
        bool empty=true;for(auto ep:channel.endpoint)empty&=ep==kNone;
        if(empty)channel.retired=true;
    }
    if(!changed)return Error::NotFound;
    size_t position=0;while(position<model.policy_count && model.policies[position].node<node)++position;
    if(position==model.policy_count || model.policies[position].node!=node) {
        if(model.policy_count==kMaxPolicies)return Error::Capacity;
        for(size_t i=model.policy_count;i>position;--i)model.policies[i]=model.policies[i-1];
        model.policies[position]={node,15};++model.policy_count;
    }
    if(model.revision==UINT32_MAX)return Error::Conflict;
    ++model.revision;
    return Error::Ok;
}

cJSON *describe(const Model &before, const Model &after, const Ticket &receipt, const uint8_t guid[16])
{
    auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[kEncodedMax]);
    auto encoded = std::unique_ptr<char[]>(new (std::nothrow) char[kEncodedMax * 2 + 1]);
    size_t size = 0;
    if (!bytes || !encoded || encode(before, guid, bytes.get(), kEncodedMax, size) != Error::Ok) return nullptr;
    hex(bytes.get(), size, encoded.get());
    char hash[65], token[33];
    hex(receipt.backup_sha256, 32, hash);
    hex(receipt.nonce, 16, token);
    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;
    auto *channels = cJSON_AddArrayToObject(root, "channels");
    bool ok = channels && cJSON_AddBoolToObject(root, "ok", true) &&
              cJSON_AddStringToObject(root, "token", token) &&
              cJSON_AddStringToObject(root, "backup_hex", encoded.get()) &&
              cJSON_AddStringToObject(root, "backup_sha256", hash);
    for (size_t i = 0; ok && i < after.count; ++i) {
        if(i<before.count && memcmp(&before.channels[i],&after.channels[i],sizeof(Channel))==0)continue;
        const auto &channel = after.channels[i];
        auto *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(channels, item)) {
            cJSON_Delete(item);
            ok = false;
            break;
        }
        cJSON_AddStringToObject(item,"action",i>=before.count?"added":channel.retired?"retired":"updated");
        cJSON_AddBoolToObject(item,"retired",channel.retired);
        auto *sources = cJSON_AddArrayToObject(item, "sources");
        ok = sources && cJSON_AddNumberToObject(item, "number", i) &&
             cJSON_AddNumberToObject(item, "kind", static_cast<unsigned>(channel.kind));
        for (unsigned role = 0; ok && role < kRoleCount; ++role) {
            if (channel.endpoint[role] == kNone) continue;
            auto *source = cJSON_CreateObject();
            if (!source || !cJSON_AddItemToArray(sources, source)) {
                cJSON_Delete(source);
                ok = false;
                break;
            }
            ok = cJSON_AddNumberToObject(source, "role", role) &&
                 cJSON_AddNumberToObject(source, "ep", channel.endpoint[role]);
        }
    }
    auto *relations = cJSON_AddArrayToObject(root, "relations");
    ok = ok && relations;
    for (size_t i = 0; ok && i < after.count; ++i) {
        const auto meter = after.channels[i].related_meter;
        if (meter == kNone || (i < before.count && before.channels[i].related_meter == meter)) continue;
        auto *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(relations, item)) {
            cJSON_Delete(item);
            ok = false;
            break;
        }
        ok = cJSON_AddNumberToObject(item, "relay", i) && cJSON_AddNumberToObject(item, "meter", meter);
    }
    if(ok) {
        cJSON_AddStringToObject(root,"history_note","Numery i typy pozostaja. Historia nie jest przenoszona miedzy kanalami. Wycofany kanal zachowuje swoj numer; mozna wylaczyc jego funkcje w SUPLA Cloud bez usuwania historii.");
        return root;
    }
    cJSON_Delete(root);
    return nullptr;
}
}

mapping::Error prepare(uint64_t node, cJSON **result, const Thermohygrometer *merge, const ElectricityMeter *meter)
{
    *result = nullptr;
    if(merge && meter)return Error::Invalid;
    MapLock lock;
    pending.reset();
    if (!quick_add_device_exists(node)) return Error::Invalid;
    Proof proof(node);
    if (!proof.generation()) return Error::Discovery;
    supla_cfg_t cfg{};
    if (supla_config_copy(&cfg) != ESP_OK || !cfg.configured) return Error::Identity;
    auto store = std::unique_ptr<Store>(new (std::nothrow) Store(backend, crypto));
    auto model = std::unique_ptr<Model>(new (std::nothrow) Model);
    if (!store || !model) return Error::Capacity;
    auto error = store->load(cfg.guid);
    if (error != Error::Ok) return error;
    *model = store->current();
    error = merge?combine(*model,node,proof,*merge):propose(*model, node, proof,meter);
    if (error != Error::Ok) return error;
    Impact impact;
    error = store->prepare(*model, proof, esp_timer_get_time() / 1000, ticket, impact);
    if (error != Error::Ok) return error;
    *result = describe(store->current(), *model, ticket, cfg.guid);
    if (!*result) return Error::Capacity;
    memcpy(pending_guid, cfg.guid, sizeof(pending_guid));
    pending_node = node;
    pending = std::move(store);
    return Error::Ok;
}

mapping::Error commit(const char *token)
{
    MapLock lock;
    if (!pending || !token) return Error::Ticket;
    char expected[33];
    hex(ticket.nonce, 16, expected);
    if (strcmp(token, expected) != 0) return Error::Ticket;
    supla_cfg_t cfg{};
    if (supla_config_copy(&cfg) != ESP_OK || memcmp(cfg.guid, pending_guid, sizeof(pending_guid)) != 0 ||
        !quick_add_device_exists(pending_node)) {
        pending.reset();
        return Error::Conflict;
    }
    Proof proof(pending_node);
    const auto error = pending->commit(ticket, proof, esp_timer_get_time() / 1000);
    const bool faulted = pending->faulted();
    pending.reset();
    return faulted ? Error::Faulted : error;
}

}
