#include "device_telemetry.h"
#include "quick_add.h"
#include "stack_try_guard.h"
#include <cJSON.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_read_command.h>
#include <app/data-model/Decode.h>
#include <clusters/GeneralDiagnostics/Attributes.h>
#include <clusters/ThreadNetworkDiagnostics/Attributes.h>
#include <openthread/thread_ftd.h>
#include <openthread/ip6.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <memory>
#include <new>
namespace device_telemetry {
namespace {
constexpr size_t kSamples=128,kNodes=32; // Keep spinlock-guarded tables in fast internal SRAM so IRQ-off critical sections stay sub-microsecond.
Reading readings[kSamples];
Contact contacts[kNodes];
bool overflow=false;
portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
Contact *contact(uint64_t node) {
 for(auto &c:contacts)if(c.node==node)return &c;
 for(auto &c:contacts)if(!c.node){c.node=node;return &c;}
 overflow=true;return nullptr;
}
Reading *reading(uint64_t node,uint16_t ep,Metric metric) {
 for(auto &r:readings)if(r.node==node && r.endpoint==ep && r.metric==metric)return &r;
 for(auto &r:readings)if(!r.node){r.node=node;r.endpoint=ep;r.metric=metric;return &r;}
 overflow=true;return nullptr;
}
}
bool lookup(uint64_t node,uint16_t ep,Metric metric,Reading &out) {
 bool found=false;portENTER_CRITICAL(&guard);
 for(const auto &r:readings)if(r.node==node&&r.endpoint==ep&&r.metric==metric){out=r;found=true;break;}
 portEXIT_CRITICAL(&guard);return found;
}
void observe(uint64_t node,uint16_t ep,Metric metric,bool valid,int64_t raw,Source source) {
 if(!node)return;
 const int64_t now=esp_timer_get_time();
 portENTER_CRITICAL(&guard);
 if(auto *r=reading(node,ep,metric)) {
  if(!r->received || r->valid!=valid || (valid && r->raw!=raw))r->changed=now;
  r->valid=valid;r->raw=raw;r->received=now;r->source=source;
 }
 portEXIT_CRITICAL(&guard);
}
void applied(uint64_t node,uint16_t ep,Metric metric,bool valid,int64_t raw,int channel) {
 portENTER_CRITICAL(&guard);
 for(auto &r:readings)if(r.node==node && r.endpoint==ep && r.metric==metric && r.valid==valid && (!valid || r.raw==raw)) {
  r.applied=esp_timer_get_time();r.channel=channel;break;
 }
 portEXIT_CRITICAL(&guard);
}
void subscription(uint64_t node,bool active) {
 portENTER_CRITICAL(&guard);if(auto *c=contact(node))c->active=active;portEXIT_CRITICAL(&guard);
}
void report(uint64_t node) {
 portENTER_CRITICAL(&guard);if(auto *c=contact(node)){c->report=esp_timer_get_time();++c->reports;}portEXIT_CRITICAL(&guard);
}
void intervals(uint64_t node,uint16_t minimum,uint16_t maximum) {
 portENTER_CRITICAL(&guard);if(auto *c=contact(node)){c->minimum=minimum;c->maximum=maximum;}portEXIT_CRITICAL(&guard);
}
void forget(uint64_t node) {
 portENTER_CRITICAL(&guard);
 for(auto &r:readings)if(r.node==node)r={};
 for(auto &c:contacts)if(c.node==node)c={};
 portEXIT_CRITICAL(&guard);
}
size_t copy(uint64_t node,Reading *out,size_t capacity,Contact &c,bool &truncated) {
 size_t count=0;c={};
 portENTER_CRITICAL(&guard);truncated=overflow;
 for(const auto &r:readings)if(r.node==node){if(count<capacity)out[count++]=r;else truncated=true;}
 for(const auto &v:contacts)if(v.node==node)c=v;
 portEXIT_CRITICAL(&guard);return count;
}

namespace {
void add_age(cJSON *o,const char *key,int64_t now,int64_t stamp) {
 if(stamp && stamp<=now)cJSON_AddNumberToObject(o,key,(now-stamp)/1000000);
 else cJSON_AddNullToObject(o,key);
}
namespace gd = chip::app::Clusters::GeneralDiagnostics;
namespace td = chip::app::Clusters::ThreadNetworkDiagnostics;
using Role = td::RoutingRoleEnum;

constexpr int64_t kSecond = 1000000;
constexpr int64_t kRouterRefresh = 45 * kSecond;
constexpr int64_t kDeviceRefresh = 300 * kSecond;
constexpr int64_t kIdentityLifetime = 900 * kSecond;
constexpr int64_t kTableLifetime = 100 * kSecond;
constexpr size_t kRemoteChildren = 8;
constexpr int8_t kInvalidRssi = 127;

struct RemoteChild {
 uint64_t address = 0;
 uint32_t age = 0;
 int8_t average = kInvalidRssi, last = kInvalidRssi;
 uint8_t quality = 0;
 bool sleepy = false;
};

struct ThreadPeer {
 uint64_t node = 0, address = 0;
 int64_t next_read = 0, identity_at = 0, role_at = 0, table_at = 0;
 Role role = Role::kUnspecified;
 RemoteChild children[kRemoteChildren];
};

// Read and write under the CHIP lock. Stored in PSRAM to preserve internal DRAM for TLS/Wi-Fi.
EXT_RAM_BSS_ATTR ThreadPeer thread_peers[kNodes];
uint32_t thread_read_generation = 0;
int64_t thread_read_started = 0;

bool is_router(Role role) {
 return role == Role::kRouter || role == Role::kLeader;
}

bool fresh(int64_t stamp, int64_t now, int64_t lifetime) {
 return stamp > 0 && stamp <= now && now - stamp <= lifetime;
}

uint64_t extended_address(const uint8_t *bytes) {
 uint64_t value=0;
 for(unsigned i=0;i<8;++i)value=(value<<8)|bytes[i];
 return value;
}

ThreadPeer *thread_peer(uint64_t node) {
 for(auto &peer:thread_peers)if(peer.node==node)return &peer;
 return nullptr;
}

void read_thread_attribute(ThreadPeer &peer, const chip::app::ConcreteDataAttributePath &path,
                           chip::TLV::TLVReader &data) {
 const auto now=esp_timer_get_time();
 if(path.mEndpointId!=0)return;
 if(path.mClusterId==gd::Id && path.mAttributeId==gd::Attributes::NetworkInterfaces::Id) {
  gd::Attributes::NetworkInterfaces::TypeInfo::DecodableType list;
  if(chip::app::DataModel::Decode(data,list)!=CHIP_NO_ERROR)return;
  uint64_t address=0;
  bool ambiguous=false;
  auto it=list.begin();
  while(it.Next()) {
   const auto &iface=it.GetValue();
   if(iface.type!=gd::InterfaceTypeEnum::kThread || !iface.isOperational || iface.hardwareAddress.size()!=8)continue;
   const auto id=extended_address(iface.hardwareAddress.data());
   if(address && address!=id)ambiguous=true;
   address=id;
  }
  if(it.GetStatus()!=CHIP_NO_ERROR)return;
  peer.address=ambiguous?0:address;
  peer.identity_at=now;
 } else if(path.mClusterId==td::Id && path.mAttributeId==td::Attributes::RoutingRole::Id) {
  td::Attributes::RoutingRole::TypeInfo::DecodableType role;
  if(chip::app::DataModel::Decode(data,role)!=CHIP_NO_ERROR)return;
  peer.role=role.IsNull()?Role::kUnspecified:role.Value();
  peer.role_at=now;
  if(is_router(peer.role))peer.next_read=now+kRouterRefresh;
  else peer.table_at=0;
 } else if(path.mClusterId==td::Id && path.mAttributeId==td::Attributes::NeighborTable::Id) {
  td::Attributes::NeighborTable::TypeInfo::DecodableType list;
  if(chip::app::DataModel::Decode(data,list)!=CHIP_NO_ERROR)return;
  RemoteChild children[kRemoteChildren];
  size_t count=0;
  auto it=list.begin();
  while(it.Next()) {
   const auto &entry=it.GetValue();
   if(!entry.isChild || !entry.extAddress || count==kRemoteChildren)continue;
   children[count++]={entry.extAddress,entry.age,
     entry.averageRssi.IsNull()?kInvalidRssi:entry.averageRssi.Value(),
     entry.lastRssi.IsNull()?kInvalidRssi:entry.lastRssi.Value(),entry.lqi,!entry.rxOnWhenIdle};
  }
  if(it.GetStatus()!=CHIP_NO_ERROR)return;
  std::copy_n(children,kRemoteChildren,peer.children);
  peer.table_at=now;
 }
}

void add_link(cJSON *out, const char *source, int average, int last, unsigned quality,
              uint64_t last_heard, bool sleepy) {
 cJSON_AddStringToObject(out,"source",source);
 if(average>=-127 && average<=0)cJSON_AddNumberToObject(out,"average_rssi_dbm",average);
 if(last>=-127 && last<=0)cJSON_AddNumberToObject(out,"last_rssi_dbm",last);
 if(quality<=3)cJSON_AddNumberToObject(out,"quality",quality);
 cJSON_AddNumberToObject(out,"last_heard_s",static_cast<double>(last_heard));
 cJSON_AddBoolToObject(out,"sleepy",sleepy);
}

const char *role_label(Role role) {
 switch(role) {
  case Role::kLeader:return "leader";
  case Role::kRouter:return "router";
  case Role::kSleepyEndDevice:return "sleepy_end_device";
  case Role::kEndDevice:return "end_device";
  case Role::kReed:return "router_eligible";
  default:return nullptr;
 }
}

void thread_signal(uint64_t node,cJSON *out) {
 const auto now=esp_timer_get_time();
 char text[64]{};
 uint64_t address_eui=0, parent=0;
 Role remote_role=Role::kUnspecified;
 RemoteChild remote_child{};
 uint64_t remote_age=0;
 {
  // Snapshot Matter data, then release the CHIP lock before taking the OpenThread lock.
  gateway::TryStackLock lock(chip::DeviceLayer::PlatformMgr());
  if(!lock.owned()){
   cJSON_AddStringToObject(out,"reason","matter_busy");
   return;
  }
  if(const auto *peer=thread_peer(node)) {
   if(fresh(peer->identity_at,now,kIdentityLifetime))address_eui=peer->address;
   if(fresh(peer->role_at,now,kIdentityLifetime))remote_role=peer->role;
  }
  if(address_eui && !is_router(remote_role)) {
   bool ambiguous=false;
   for(const auto &router:thread_peers) {
    if(!router.node || router.node==node || !is_router(router.role) ||
       !fresh(router.table_at,now,kTableLifetime))continue;
    for(const auto &child:router.children) {
     if(child.address!=address_eui)continue;
     if(parent && parent!=router.node)ambiguous=true;
     parent=router.node;
     remote_child=child;
     remote_age=uint64_t(child.age)+(now-router.table_at)/kSecond;
     break;
    }
   }
   if(ambiguous)parent=0;
  }
  auto *controller=esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
  chip::Transport::PeerAddress peer;
  if(controller && controller->GetPeerAddress(node,peer)==CHIP_NO_ERROR)
   peer.GetIPAddress().ToString(text,sizeof(text));
 }

 otIp6Address address{};
 const bool peer_ipv6=text[0] && otIp6AddressFromString(text,&address)==OT_ERROR_NONE;
 otChildInfo child_match{};
 otNeighborInfo router_match{};
 bool direct_child=false, direct_router=false, available=false;
 if(auto *instance=esp_openthread_get_instance();instance && esp_openthread_lock_acquire(pdMS_TO_TICKS(20))) {
  const auto role=otThreadGetDeviceRole(instance);
  available=role==OT_DEVICE_ROLE_ROUTER || role==OT_DEVICE_ROLE_LEADER;
  if(available) {
   for(uint16_t i=0;i<otThreadGetMaxAllowedChildren(instance);++i) {
    otChildInfo child{};
    if(otThreadGetChildInfoByIndex(instance,i,&child)!=OT_ERROR_NONE || child.mIsStateRestoring)continue;
    if(address_eui && extended_address(child.mExtAddress.m8)==address_eui)direct_child=true;
    if(!direct_child && peer_ipv6) {
     otChildIp6AddressIterator iterator=OT_CHILD_IP6_ADDRESS_ITERATOR_INIT;
     otIp6Address registered{};
     while(otThreadGetChildNextIp6Address(instance,i,&iterator,&registered)==OT_ERROR_NONE) {
      if(memcmp(&registered,&address,sizeof(address))==0){direct_child=true;break;}
     }
    }
    if(direct_child){child_match=child;break;}
   }
   if(address_eui && !direct_child) {
    otNeighborInfoIterator iterator=OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo neighbor{};
    while(otThreadGetNextNeighborInfo(instance,&iterator,&neighbor)==OT_ERROR_NONE) {
     if(neighbor.mIsChild || (neighbor.mRloc16&0x03ff) ||
        extended_address(neighbor.mExtAddress.m8)!=address_eui)continue;
     router_match=neighbor;
     direct_router=true;
     break;
    }
   }
  }
  esp_openthread_lock_release();
 }
 cJSON_AddBoolToObject(out,"direct_child",direct_child);
 if(available && direct_child) {
  cJSON_AddStringToObject(out,"role",child_match.mRxOnWhenIdle?"end_device":"sleepy_end_device");
  add_link(out,"c6_child",child_match.mAverageRssi,child_match.mLastRssi,
           child_match.mLinkQualityIn,child_match.mAge,!child_match.mRxOnWhenIdle);
  return;
 }
 if(direct_router) {
  cJSON_AddStringToObject(out,"role",remote_role==Role::kLeader?"leader":"router");
  add_link(out,"c6_router",router_match.mAverageRssi,router_match.mLastRssi,
           router_match.mLinkQualityIn,router_match.mAge,false);
  return;
 }
 if(const auto *label=role_label(remote_role))cJSON_AddStringToObject(out,"role",label);
 if(parent && available && remote_age<=uint64_t(kTableLifetime/kSecond)) {
  char parent_id[24];
  snprintf(parent_id,sizeof(parent_id),"%llu",static_cast<unsigned long long>(parent));
  cJSON_AddStringToObject(out,"parent_node",parent_id);
  add_link(out,"matter_parent",remote_child.average,remote_child.last,remote_child.quality,
           remote_age,remote_child.sleepy);
  return;
 }
 cJSON_AddStringToObject(out,"reason",available?"thread_link_unconfirmed":"thread_unavailable");
}
}

void refresh_thread(uint64_t node) {
 // Called by the existing Matter worker with the CHIP lock acquired.
 const auto now=esp_timer_get_time();
 if(fresh(thread_read_started,now,60*kSecond))return;
 thread_read_started=0;
 auto *controller=esp_matter::controller::matter_controller_client::get_instance().get_commissioner();
 chip::Transport::PeerAddress address;
 if(!controller || controller->GetPeerAddress(node,address)!=CHIP_NO_ERROR)return;
 auto *peer=thread_peer(node);
 if(!peer) {
  for(auto &entry:thread_peers)if(!entry.node){entry.node=node;peer=&entry;break;}
 }
 if(!peer)return;
 if(now<peer->next_read)return;
 const bool router=is_router(peer->role);
 const bool include_identity=!fresh(peer->identity_at,now,kDeviceRefresh);
 peer->next_read=now+(router?kRouterRefresh:kDeviceRefresh);
 chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> paths;
 if(!paths.Alloc(1+unsigned(router)+unsigned(include_identity)))return;
 size_t i=0;
 paths[i++]=chip::app::AttributePathParams(0,td::Id,td::Attributes::RoutingRole::Id);
 if(include_identity)paths[i++]=chip::app::AttributePathParams(0,gd::Id,gd::Attributes::NetworkInterfaces::Id);
 if(router)paths[i]=chip::app::AttributePathParams(0,td::Id,td::Attributes::NeighborTable::Id);
 chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> events;
 const uint32_t generation=++thread_read_generation;
 auto finish=[generation]() {
  if(thread_read_generation==generation)thread_read_started=0;
 };
 auto *command=chip::Platform::New<esp_matter::controller::read_command>(
  node,std::move(paths),std::move(events),
  [generation](uint64_t id,const auto &path,auto *data,const auto &status) {
   if(thread_read_generation!=generation || !thread_read_started || !data || !status.IsSuccess() || !quick_add_device_exists(id))return;
   if(auto *entry=thread_peer(id))read_thread_attribute(*entry,path,*data);
  },
  [finish](uint64_t,const auto &,const auto &){finish();},
  nullptr,
  [finish](void *,const auto &,CHIP_ERROR){finish();},
  nullptr,true);
 if(command) {
  thread_read_started=now;
  if(command->send_command()!=ESP_OK)finish();
 }
}

void forget_thread(uint64_t node) {
 // Called under the CHIP lock during Matter worker cleanup.
 if(auto *peer=thread_peer(node))*peer={};
}

void append_json(uint64_t node,cJSON *device) {
 auto snapshot=std::unique_ptr<Reading[]>(new(std::nothrow) Reading[kSamples]);if(!snapshot)return;
 Contact c;bool truncated=false;const size_t count=copy(node,snapshot.get(),kSamples,c,truncated);
 auto *out=cJSON_AddObjectToObject(device,"telemetry");if(!out)return;
 const int64_t now=esp_timer_get_time();int64_t last=c.report;
 cJSON_AddBoolToObject(out,"subscription_active",c.active);cJSON_AddBoolToObject(out,"truncated",truncated);
 add_age(out,"last_report_age_s",now,c.report);cJSON_AddNumberToObject(out,"report_count",c.reports);
 if(c.maximum){cJSON_AddNumberToObject(out,"minimum_s",c.minimum);cJSON_AddNumberToObject(out,"maximum_s",c.maximum);}
 auto *values=cJSON_AddArrayToObject(out,"values");
 for(size_t i=0;i<count;++i) {
  const auto &r=snapshot[i];last=std::max(last,r.received);
  auto *v=cJSON_CreateObject();if(!v)break;cJSON_AddItemToArray(values,v);
  cJSON_AddNumberToObject(v,"endpoint",r.endpoint);
  const char *kind="unknown", *unit="";double divisor=1;
  switch(r.metric) {
    case Metric::Temperature:kind="temperature";unit="°C";divisor=100;break;
    case Metric::Humidity:kind="humidity";unit="%";divisor=100;break;
    case Metric::Battery:kind="battery";unit="%";divisor=2;break;
    case Metric::OnOff:kind="onoff";break;
    case Metric::Voltage:kind="voltage";unit="V";divisor=1000;break;
    case Metric::Current:kind="current";unit="A";divisor=1000;break;
    case Metric::ActivePower:kind="active_power";unit="W";divisor=1000;break;
    case Metric::ImportedEnergy:kind="imported_energy";unit="kWh";divisor=1000000;break;
    case Metric::ExportedEnergy:kind="exported_energy";unit="kWh";divisor=1000000;break;
  }
  cJSON_AddStringToObject(v,"unit",unit);
  char exact[24];snprintf(exact,sizeof(exact),"%lld",static_cast<long long>(r.raw));
  if(r.valid)cJSON_AddStringToObject(v,"raw",exact);else cJSON_AddNullToObject(v,"raw");
  cJSON_AddStringToObject(v,"kind",kind);cJSON_AddBoolToObject(v,"valid",r.valid);
  if(r.valid)cJSON_AddNumberToObject(v,"value",static_cast<double>(r.raw)/divisor);
  else cJSON_AddNullToObject(v,"value");
  cJSON_AddStringToObject(v,"source",r.source==Source::Subscription?"subscription":"read");
  add_age(v,"received_age_s",now,r.received);add_age(v,"changed_age_s",now,r.changed);add_age(v,"applied_age_s",now,r.applied);
  if(r.channel>=0)cJSON_AddNumberToObject(v,"supla_channel",r.channel);
 }
 add_age(out,"last_contact_age_s",now,last);
 auto *signal=cJSON_AddObjectToObject(out,"thread");if(signal)thread_signal(node,signal);
}
}
