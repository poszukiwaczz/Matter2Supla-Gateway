#include "mapping_core.h"
#include <cstring>
#include <limits>

namespace m2s { namespace mapping {
namespace {
uint32_t crc32(const uint8_t *data,size_t n) {
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<n;i++) {
        crc^=data[i];
        for(int bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    return ~crc;
}
struct Writer {
    uint8_t *p; size_t n=0;
    void put(uint64_t value,size_t bytes) { for(size_t i=0;i<bytes;i++)p[n++]=static_cast<uint8_t>(value>>(8*i)); }
};
struct Reader {
    const uint8_t *p; size_t n=0;
    uint64_t get(size_t bytes) { uint64_t v=0;for(size_t i=0;i<bytes;i++)v|=uint64_t(p[n++])<<(8*i);return v; }
};
bool equal(const uint8_t *a,const uint8_t *b,size_t n) {
    uint8_t diff=0;for(size_t i=0;i<n;i++)diff|=a[i]^b[i];return diff==0;
}
bool role_allowed(Kind kind,size_t role) {
    return (kind==Kind::Relay && role==0) ||
        (kind==Kind::Thermohygrometer && role>=1 && role<=2) ||
        (kind==Kind::ElectricityMeter && role>=3 && role<kRoleCount);
}
bool endpoints_equal(const Channel &a,const Channel &b) {
    for(size_t r=0;r<kRoleCount;r++)if(a.endpoint[r]!=b.endpoint[r])return false;
    return true;
}
Error add_policy(Model &model,uint64_t node,uint32_t seconds) {
    size_t at=0;
    while(at<model.policy_count && model.policies[at].node<node)at++;
    if(at<model.policy_count && model.policies[at].node==node)return Error::Ok;
    if(model.policy_count==kMaxPolicies)return Error::Capacity;
    for(size_t i=model.policy_count;i>at;i--)model.policies[i]=model.policies[i-1];
    model.policies[at]={node,seconds};model.policy_count++;
    return Error::Ok;
}
bool policies_equal(const Model &a,const Model &b) {
    if(a.policy_count!=b.policy_count)return false;
    for(size_t i=0;i<a.policy_count;i++)
        if(a.policies[i].node!=b.policies[i].node || a.policies[i].seconds!=b.policies[i].seconds)return false;
    return true;
}
}
Key backup_key(uint32_t revision) {return revision%2?Key::BackupOdd:Key::BackupEven;}
bool source_path(Role role,uint32_t &cluster,uint32_t &attribute) {
    static constexpr uint32_t clusters[kRoleCount]={6,0x402,0x405,0x90,0x90,0x90,0x91,0x91};
    static constexpr uint32_t attributes[kRoleCount]={0,0,0,4,5,8,1,2};
    size_t r=static_cast<size_t>(role);
    if(r>=kRoleCount)return false;
    cluster=clusters[r];attribute=attributes[r];return true;
}
uint32_t interval(const Model &model,uint64_t node) {
    for(size_t i=0;i<model.policy_count && i<kMaxPolicies;i++)
        if(model.policies[i].node==node)return model.policies[i].seconds;
    return 15;
}
uint32_t stale_after(const Model &model,uint64_t node,bool relay) {
    uint32_t value=interval(model,node)*4+30, minimum=relay?180:900;
    return value>minimum?value:minimum;
}
uint64_t max_node(const Model &model) {
    uint64_t maximum=0;
    for(size_t i=0;i<model.count && i<kMaxChannels;i++)if(model.channels[i].node>maximum)maximum=model.channels[i].node;
    for(size_t i=0;i<model.policy_count && i<kMaxPolicies;i++)if(model.policies[i].node>maximum)maximum=model.policies[i].node;
    return maximum;
}
int source_channel(const Model &model,uint64_t node,uint16_t endpoint,Role role) {
    size_t r=static_cast<size_t>(role);
    if(r>=kRoleCount || endpoint==kNone)return -1;
    for(size_t i=0;i<model.count && i<kMaxChannels;i++) {
        const Channel &c=model.channels[i];
        if(!c.retired && c.node==node && c.endpoint[r]==endpoint)return static_cast<int>(i);
    }
    return -1;
}
Error validate(const Model &model) {
    if(model.count>kMaxChannels || model.policy_count>kMaxPolicies)return Error::Capacity;
    for(size_t i=0;i<model.policy_count;i++) {
        const Policy &p=model.policies[i];
        if(!p.node || p.seconds<5 || p.seconds>86400 ||
           (i && model.policies[i-1].node>=p.node))return Error::Invalid;
    }
    for(size_t i=0;i<model.count;i++) {
        const Channel &c=model.channels[i];
        if(!c.node || c.kind<Kind::Relay || c.kind>Kind::ElectricityMeter)return Error::Invalid;
        bool policy=false;
        for(size_t j=0;j<model.policy_count;j++)if(model.policies[j].node==c.node)policy=true;
        if(!policy)return Error::Invalid;
        unsigned sources=0;
        for(size_t r=0;r<kRoleCount;r++) {
            if(c.endpoint[r]==kNone)continue;
            if(c.retired || !role_allowed(c.kind,r))return Error::Invalid;
            sources++;
            for(size_t j=0;j<i;j++) {
                const Channel &previous=model.channels[j];
                if(!previous.retired && previous.node==c.node && previous.endpoint[r]==c.endpoint[r])return Error::Invalid;
            }
        }
        if(c.retired?sources!=0:sources==0)return Error::Invalid;
        if(c.related_meter!=kNone) {
            if(c.kind!=Kind::Relay || c.retired || c.related_meter>=model.count)return Error::Invalid;
            const Channel &target=model.channels[c.related_meter];
            if(target.retired || target.kind!=Kind::ElectricityMeter || target.node!=c.node)return Error::Invalid;
        }
    }
    return Error::Ok;
}
Error transition(const Model &before,const Model &after) {
    Error err=validate(before);if(err!=Error::Ok)return err;
    err=validate(after);if(err!=Error::Ok)return err;
    if(before.revision==std::numeric_limits<uint32_t>::max() || after.revision!=before.revision+1)return Error::Conflict;
    if(after.count<before.count)return Error::Invalid;
    for(size_t i=0;i<before.count;i++)
        if(before.channels[i].node!=after.channels[i].node || before.channels[i].kind!=after.channels[i].kind)return Error::Identity;
    return Error::Ok;
}
Error decode_legacy(const uint8_t *bytes,size_t length,Model &out) {
    if(!bytes || length!=kLegacySize)return Error::Corrupt;
    Reader r{bytes};
    if(r.get(4)!=1)return Error::Corrupt;
    uint64_t count=r.get(4);if(count>kMaxChannels)return Error::Corrupt;
    Model next{};next.count=static_cast<uint16_t>(count);
    for(size_t i=0;i<count;i++) {
        Channel &c=next.channels[i];c.node=r.get(8);
        uint16_t ep=static_cast<uint16_t>(r.get(2));uint8_t kind=static_cast<uint8_t>(r.get(1));r.get(5);
        if(ep==kNone || (kind!=1 && kind!=2))return Error::Corrupt;
        c.kind=static_cast<Kind>(kind);
        if(c.kind==Kind::Relay)c.endpoint[0]=ep;
        else {c.endpoint[1]=ep;c.endpoint[2]=ep;}
        Error err=add_policy(next,c.node,15);if(err!=Error::Ok)return err;
    }
    if(validate(next)!=Error::Ok)return Error::Corrupt;
    out=next;return Error::Ok;
}
Error encode(const Model &model,const uint8_t guid[16],uint8_t *out,size_t capacity,size_t &written) {
    written=0;Error err=validate(model);if(err!=Error::Ok)return err;
    size_t length=36+model.count*28+model.policy_count*12;
    if(!guid || !out || capacity<length)return Error::Capacity;
    Writer w{out};w.put(0x3253324d,4);w.put(2,2);w.put(length,2);
    w.put(model.revision,4);w.put(model.count,2);w.put(model.policy_count,2);
    for(size_t i=0;i<16;i++)w.put(guid[i],1);
    for(size_t i=0;i<model.count;i++) {
        const Channel &c=model.channels[i];
        w.put(c.node,8);w.put(static_cast<uint8_t>(c.kind),1);w.put(c.retired?1:0,1);w.put(c.related_meter,2);
        for(uint16_t ep:c.endpoint)w.put(ep,2);
    }
    for(size_t i=0;i<model.policy_count;i++) {w.put(model.policies[i].node,8);w.put(model.policies[i].seconds,4);}
    uint32_t crc=crc32(out,w.n);w.put(crc,4);written=w.n;return Error::Ok;
}
Error decode(const uint8_t *bytes,size_t length,const uint8_t guid[16],Model &out) {
    if(!bytes || !guid || length<36 || length>kEncodedMax)return Error::Corrupt;
    Reader r{bytes};
    if(r.get(4)!=0x3253324d || r.get(2)!=2 || r.get(2)!=length)return Error::Corrupt;
    Reader tail{bytes+length-4};
    if(crc32(bytes,length-4)!=tail.get(4))return Error::Corrupt;
    Model next{};next.revision=static_cast<uint32_t>(r.get(4));
    next.count=static_cast<uint16_t>(r.get(2));next.policy_count=static_cast<uint16_t>(r.get(2));
    if(next.count>kMaxChannels || next.policy_count>kMaxPolicies ||
       length!=36+next.count*28+next.policy_count*12)return Error::Corrupt;
    if(!equal(bytes+16,guid,16))return Error::Identity;
    r.n+=16;
    for(size_t i=0;i<next.count;i++) {
        Channel &c=next.channels[i];c.node=r.get(8);c.kind=static_cast<Kind>(r.get(1));
        uint64_t flags=r.get(1);if(flags>1)return Error::Corrupt;c.retired=flags==1;
        c.related_meter=static_cast<uint16_t>(r.get(2));
        for(uint16_t &ep:c.endpoint)ep=static_cast<uint16_t>(r.get(2));
    }
    for(size_t i=0;i<next.policy_count;i++) {
        next.policies[i].node=r.get(8);next.policies[i].seconds=static_cast<uint32_t>(r.get(4));
    }
    if(validate(next)!=Error::Ok)return Error::Corrupt;
    out=next;return Error::Ok;
}
Store::Store(Backend &backend,Crypto &crypto):backend_(backend),crypto_(crypto) {}
void Store::cancel() { prepared_=false;ticket_={};pending_={}; }
Error Store::load(const uint8_t guid[16]) {
    cancel();loaded_=false;faulted_=false;
    if(!guid)return Error::Identity;
    memcpy(guid_,guid,16);
    base_length_=0;base_missing_=false;base_key_=Key::Active;
    Error err=backend_.read(base_key_,base_,sizeof(base_),base_length_);
    if(err==Error::NotFound) {
        base_key_=Key::Legacy;
        err=backend_.read(base_key_,base_,sizeof(base_),base_length_);
        if(err==Error::NotFound) { active_={};base_missing_=true;base_length_=0;err=Error::Ok; }
        else if(err==Error::Ok)err=decode_legacy(base_,base_length_,active_);
    } else if(err==Error::Ok)err=decode(base_,base_length_,guid_,active_);
    if(err==Error::Ok)loaded_=true;
    return err;
}
Error Store::check_base() {
    uint8_t *bytes=scratch_[1];size_t length=0;
    // A v2 record appearing while we still have a legacy snapshot is a conflict.
    if(base_key_==Key::Legacy) {
        Error err=backend_.read(Key::Active,bytes,kEncodedMax,length);
        if(err==Error::Ok)return Error::Conflict;
        if(err!=Error::NotFound)return err;
    }
    Error err=backend_.read(base_key_,bytes,kEncodedMax,length);
    if(err==Error::NotFound)return base_missing_?Error::Ok:Error::Conflict;
    if(err!=Error::Ok)return err;
    if(base_missing_ || length!=base_length_ || !equal(bytes,base_,length))return Error::Conflict;
    return Error::Ok;
}
Error Store::verify_sources(const Model &proposal,const Observations &observations) {
    for(size_t i=0;i<proposal.count;i++)for(size_t r=0;r<kRoleCount;r++) {
        const Channel &c=proposal.channels[i];
        if(c.endpoint[r]==kNone)continue;
        if(i<active_.count && active_.channels[i].endpoint[r]==c.endpoint[r] && !active_.channels[i].retired)continue;
        if(!observations.generation() || !observations.supports({c.node,c.endpoint[r],static_cast<Role>(r)}))return Error::Discovery;
    }
    return Error::Ok;
}
Error Store::prepare(const Model &proposal,const Observations &observations,uint64_t now_ms,Ticket &ticket,Impact &impact) {
    cancel();ticket={};impact={};
    if(faulted_)return Error::Faulted;
    if(!loaded_)return Error::Invalid;
    const uint32_t generation=observations.generation();
    Error err=transition(active_,proposal);if(err!=Error::Ok)return err;
    err=verify_sources(proposal,observations);if(err!=Error::Ok)return err;
    err=check_base();if(err!=Error::Ok)return err;
    uint8_t *backup=scratch_[0],*verified=scratch_[1];size_t backup_size=0,verified_size=0;
    err=encode(active_,guid_,backup,kEncodedMax,backup_size);if(err!=Error::Ok)return err;
    // Do not repeatedly rewrite an identical backup on retries.
    err=backend_.read(backup_key(active_.revision),verified,kEncodedMax,verified_size);
    if(err!=Error::Ok && err!=Error::NotFound)return err;
    if(err!=Error::Ok || verified_size!=backup_size || !equal(backup,verified,backup_size)) {
        err=backend_.write(backup_key(active_.revision),backup,backup_size);if(err!=Error::Ok)return err;
        err=backend_.read(backup_key(active_.revision),verified,kEncodedMax,verified_size);
        if(err!=Error::Ok)return err;
        if(verified_size!=backup_size || !equal(backup,verified,backup_size))return Error::Corrupt;
    }
    if(!crypto_.sha256(backup,backup_size,ticket_.backup_sha256))return Error::Crypto;
    err=encode(proposal,guid_,verified,kEncodedMax,verified_size);if(err!=Error::Ok)return err;
    if(!crypto_.sha256(verified,verified_size,ticket_.proposal_sha256) ||
       !crypto_.random(ticket_.nonce,sizeof(ticket_.nonce)))return Error::Crypto;
    if(now_ms>std::numeric_limits<uint64_t>::max()-120000)return Error::Expired;
    if(observations.generation()!=generation)return Error::Conflict;
    ticket_.base_revision=active_.revision;ticket_.generation=generation;
    ticket_.expires_ms=now_ms+120000;prepared_at_=now_ms;
    impact.before_count=active_.count;impact.after_count=proposal.count;
    impact.policies_changed=!policies_equal(active_,proposal);
    for(size_t i=0;i<proposal.count;i++) {
        if(i>=active_.count) {impact.channels[i]=Append;continue;}
        const Channel &a=active_.channels[i],&b=proposal.channels[i];
        if(!a.retired && b.retired)impact.channels[i]|=Retire;
        if(a.retired && !b.retired)impact.channels[i]|=Reactivate;
        if(!endpoints_equal(a,b))impact.channels[i]|=Sources;
        if(a.related_meter!=b.related_meter)impact.channels[i]|=Relation;
    }
    pending_=proposal;prepared_=true;ticket=ticket_;return Error::Ok;
}
Error Store::commit(const Ticket &ticket,const Observations &observations,uint64_t now_ms) {
    if(faulted_)return Error::Faulted;
    if(!loaded_ || !prepared_)return Error::Ticket;
    if(ticket.base_revision!=ticket_.base_revision || ticket.generation!=ticket_.generation ||
       ticket.expires_ms!=ticket_.expires_ms ||
       !equal(ticket.nonce,ticket_.nonce,16) ||
       !equal(ticket.proposal_sha256,ticket_.proposal_sha256,32) ||
       !equal(ticket.backup_sha256,ticket_.backup_sha256,32))return Error::Ticket;
    if(now_ms<prepared_at_ || now_ms>=ticket_.expires_ms) {cancel();return Error::Expired;}
    if(observations.generation()!=ticket_.generation)return Error::Conflict;
    Error err=check_base();if(err!=Error::Ok)return err;
    err=verify_sources(pending_,observations);if(err!=Error::Ok)return err;
    uint8_t *bytes=scratch_[0],digest[32];size_t length=0;
    err=backend_.read(backup_key(active_.revision),bytes,kEncodedMax,length);if(err!=Error::Ok)return err;
    if(!crypto_.sha256(bytes,length,digest))return Error::Crypto;
    if(!equal(digest,ticket_.backup_sha256,32))return Error::Corrupt;
    err=encode(pending_,guid_,bytes,kEncodedMax,length);if(err!=Error::Ok)return err;
    if(!crypto_.sha256(bytes,length,digest))return Error::Crypto;
    if(!equal(digest,ticket_.proposal_sha256,32))return Error::Corrupt;
    if(observations.generation()!=ticket_.generation)return Error::Conflict;
    err=backend_.write(Key::Active,bytes,length);
    // Never continue using stale RAM after an ambiguous set/commit/readback failure.
    if(err!=Error::Ok) {faulted_=true;cancel();return err;}
    memcpy(base_,bytes,length);base_length_=length;
    size_t verified_size=0;
    err=backend_.read(Key::Active,bytes,kEncodedMax,verified_size);
    if(err!=Error::Ok || verified_size!=base_length_ || !equal(bytes,base_,base_length_)) {
        faulted_=true;cancel();return err==Error::Ok?Error::Corrupt:err;
    }
    active_=pending_;base_key_=Key::Active;base_missing_=false;cancel();return Error::Ok;
}
Error Store::rollback_proposal(Model &out) {
    if(faulted_)return Error::Faulted;
    if(!loaded_)return Error::Invalid;
    if(active_.revision==0)return Error::NotFound;
    uint8_t *bytes=scratch_[1];size_t length=0;
    Error err=backend_.read(backup_key(active_.revision-1),bytes,kEncodedMax,length);if(err!=Error::Ok)return err;
    Model previous{};err=decode(bytes,length,guid_,previous);if(err!=Error::Ok)return err;
    if(previous.count>active_.count || previous.revision>=active_.revision)return Error::Conflict;
    for(size_t i=0;i<previous.count;i++)
        if(previous.channels[i].node!=active_.channels[i].node || previous.channels[i].kind!=active_.channels[i].kind)return Error::Identity;
    for(size_t i=previous.count;i<active_.count;i++) {
        Channel &c=previous.channels[i];c=active_.channels[i];c.retired=true;c.related_meter=kNone;
        for(uint16_t &ep:c.endpoint)ep=kNone;
    }
    previous.count=active_.count;
    for(size_t i=0;i<active_.policy_count;i++) {
        err=add_policy(previous,active_.policies[i].node,active_.policies[i].seconds);
        if(err!=Error::Ok)return err;
    }
    previous.revision=active_.revision+1;
    err=transition(active_,previous);if(err!=Error::Ok)return err;
    out=previous;return Error::Ok;
}
const char *error_name(Error e) {
    switch(e) {
#define M2S_ERROR(x) case Error::x:return #x;
    M2S_ERROR(Ok) M2S_ERROR(NotFound) M2S_ERROR(Invalid) M2S_ERROR(Corrupt)
    M2S_ERROR(Identity) M2S_ERROR(Capacity) M2S_ERROR(Conflict) M2S_ERROR(Discovery)
    M2S_ERROR(Expired) M2S_ERROR(Ticket) M2S_ERROR(Io) M2S_ERROR(NoSpace)
    M2S_ERROR(Faulted) M2S_ERROR(Crypto)
#undef M2S_ERROR
    }
    return "Unknown";
}
}}
