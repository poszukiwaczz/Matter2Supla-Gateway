#pragma once
#include <cstddef>
#include <cstdint>
struct cJSON;
namespace device_telemetry {
enum class Metric:uint8_t {Temperature,Humidity,Battery,OnOff,Voltage,Current,ActivePower,ImportedEnergy,ExportedEnergy};
enum class Source:uint8_t {Read,Subscription};
struct Reading {
 uint64_t node=0;uint16_t endpoint=0;Metric metric{};Source source{};
 bool valid=false;int64_t raw=0,received=0,changed=0,applied=0;int channel=-1;
};
struct Contact {uint64_t node=0;bool active=false;int64_t report=0;uint16_t minimum=0,maximum=0;uint32_t reports=0;};
void observe(uint64_t,uint16_t,Metric,bool,int64_t,Source);
void applied(uint64_t,uint16_t,Metric,bool,int64_t,int);
void subscription(uint64_t,bool);
void report(uint64_t);
void intervals(uint64_t,uint16_t,uint16_t);
void forget(uint64_t);
bool lookup(uint64_t,uint16_t,Metric,Reading&);
size_t copy(uint64_t,Reading*,size_t,Contact&,bool&);
void refresh_thread(uint64_t);
void forget_thread(uint64_t);
void append_json(uint64_t,cJSON*);
}
