#pragma once
#include "mapping_core.h"
#include <supla/element.h>
#include <supla/protocol/supla_srpc.h>
#include "quick_add.h"
#include <cstring>
#include <esp_timer.h>
namespace gateway {
// IDs derive from append-only channel numbers, including tombstones. No new GUID.
inline uint8_t subdevice_id(const m2s::mapping::Model &m,uint64_t node){static_assert(m2s::mapping::kMaxChannels<=255,"Subdevice IDs must fit protocol");for(size_t i=0;i<m.count;++i)if(m.channels[i].node==node)return static_cast<uint8_t>(i+1);return 0;}
class SubdeviceInfo final:public Supla::Element {uint64_t node_;uint8_t id_;Supla::Protocol::SuplaSrpc *srpc_=nullptr;bool sent_=false;int64_t sent_at_=0;char sent_name_[40]{};
 public:SubdeviceInfo(uint64_t node,uint8_t id):node_(node),id_(id){}
 void onRegistered(Supla::Protocol::SuplaSrpc *s) override {Supla::Element::onRegistered(s);srpc_=s;sent_=false;}
 bool iterateConnected() override {if(!srpc_||!quick_add_device_exists(node_))return true;char name[40]{};quick_add_device_name_copy(node_,name,sizeof(name));if(sent_&&!strcmp(name,sent_name_)&&esp_timer_get_time()-sent_at_<60000000)return true;TDS_SubdeviceDetails details{};details.SubDeviceId=id_;strncpy(details.Name,name,sizeof(details.Name)-1);srpc_->sendSubdeviceDetails(&details);strncpy(sent_name_,name,sizeof(sent_name_)-1);sent_=true;sent_at_=esp_timer_get_time();return false;}
};
}
