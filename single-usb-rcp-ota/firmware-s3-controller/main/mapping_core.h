#pragma once
// Pure C++17 model and transaction engine. No Matter, SUPLA or NVS side effects here.
#include <cstddef>
#include <cstdint>

namespace m2s { namespace mapping {
constexpr size_t kMaxChannels=64, kMaxPolicies=64, kRoleCount=8;
constexpr uint16_t kNone=0xffff;
constexpr size_t kLegacySize=1032;
constexpr size_t kEncodedMax=36+kMaxChannels*28+kMaxPolicies*12;
enum class Kind:uint8_t { None=0, Relay=1, Thermohygrometer=2, ElectricityMeter=3 };
enum class Role:uint8_t { OnOff, Temperature, Humidity, Voltage, Current, ActivePower, ImportedEnergy, ExportedEnergy };
enum class Error:uint8_t { Ok, NotFound, Invalid, Corrupt, Identity, Capacity, Conflict,
    Discovery, Expired, Ticket, Io, NoSpace, Faulted, Crypto };
enum class Key:uint8_t { Legacy, Active, BackupEven, BackupOdd };
struct Channel {
    uint64_t node=0;
    Kind kind=Kind::None;
    bool retired=false;
    uint16_t related_meter=kNone;
    uint16_t endpoint[kRoleCount]={kNone,kNone,kNone,kNone,kNone,kNone,kNone,kNone};
};
struct Policy { uint64_t node=0; uint32_t seconds=15; };
struct Model {
    uint32_t revision=0;
    uint16_t count=0, policy_count=0;
    Channel channels[kMaxChannels]{};
    Policy policies[kMaxPolicies]{};
};
struct Source { uint64_t node; uint16_t endpoint; Role role; };
bool source_path(Role role,uint32_t &cluster,uint32_t &attribute);
Key backup_key(uint32_t revision);
Error validate(const Model &model);
Error transition(const Model &before,const Model &after);
Error decode_legacy(const uint8_t *bytes,size_t length,Model &out);
Error encode(const Model &model,const uint8_t guid[16],uint8_t *out,size_t capacity,size_t &written);
Error decode(const uint8_t *bytes,size_t length,const uint8_t guid[16],Model &out);
int source_channel(const Model &model,uint64_t node,uint16_t endpoint,Role role);
uint32_t interval(const Model &model,uint64_t node);
uint32_t stale_after(const Model &model,uint64_t node,bool relay);
uint64_t max_node(const Model &model);

class Backend {
public:
    virtual ~Backend()=default;
    virtual Error read(Key key,uint8_t *out,size_t capacity,size_t &length)=0;
    // Single-record durable replace. Error may mean ambiguous persistence.
    virtual Error write(Key key,const uint8_t *bytes,size_t length)=0;
};
class Crypto {
public:
    virtual ~Crypto()=default;
    virtual bool sha256(const uint8_t *bytes,size_t length,uint8_t out[32])=0;
    virtual bool random(uint8_t *out,size_t length)=0;
};
class Observations {
public:
    virtual ~Observations()=default;
    virtual uint32_t generation() const=0;
    // Must come from server-owned, complete discovery; never client-supplied approval.
    virtual bool supports(const Source &source) const=0;
};
enum Change:uint8_t { Unchanged=0, Append=1, Retire=2, Sources=4, Relation=8, Reactivate=16 };
struct Impact {
    uint16_t before_count=0,after_count=0;
    uint8_t channels[kMaxChannels]{};
    bool policies_changed=false;
    // History is never transferred or merged. Retired channels keep number/type.
};
struct Ticket {
    uint32_t base_revision=0,generation=0;
    uint8_t proposal_sha256[32]{},backup_sha256[32]{},nonce[16]{};
    uint64_t expires_ms=0;
};
// One owner / external mutex is required. ESP allocates this on the heap, not a task stack.
// This engine is not yet exposed by a public HTTP commit route.
class Store {
public:
    Store(Backend &backend,Crypto &crypto);
    Error load(const uint8_t guid[16]);
    const Model &current() const { return active_; }
    bool loaded() const { return loaded_; }
    bool faulted() const { return faulted_; }
    Error prepare(const Model &proposal,const Observations &observations,uint64_t now_ms,Ticket &ticket,Impact &impact);
    Error commit(const Ticket &ticket,const Observations &observations,uint64_t now_ms);
    Error rollback_proposal(Model &out);
    void cancel();
private:
    Backend &backend_;
    Crypto &crypto_;
    Model active_{},pending_{};
    uint8_t guid_[16]{},base_[kEncodedMax]{};
    uint8_t scratch_[2][kEncodedMax]{};
    size_t base_length_=0;
    Key base_key_=Key::Legacy;
    bool base_missing_=false,loaded_=false,faulted_=false,prepared_=false;
    uint64_t prepared_at_=0;
    Ticket ticket_{};
    Error check_base();
    Error verify_sources(const Model &proposal,const Observations &observations);
};
const char *error_name(Error error);
}} // namespace
