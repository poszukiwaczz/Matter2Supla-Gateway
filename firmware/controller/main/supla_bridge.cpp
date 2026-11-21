/*
 * supla_bridge.cpp - warstwa Supla bramki Matter (supla-device + esp-matter).
 *
 * Architektura:
 *  - PiggybackNet - klasa sieci supla-device korzystajaca z ISTNIEJACEGO
 *    Wi-Fi (wifi_manager / otbr). Nie inicjuje radia, nie dotyka NVS sieci;
 *    isReady() = warstwa STA ma adres IP. Klient TLS: Supla::EspIdfClient.
 *  - supla_task - wlasne zadanie: begin() + petla iterate() + oproznianie
 *    kolejki zmian (wszystkie operacje na elementach/kanałach dzieja sie
 *    w tym jednym zadaniu - supla-device nie jest thread-safe).
 *  - poll_task maintains Matter subscriptions. Reports use the existing update
 *    queue; periodic reads are fallback only and honor the device interval.
 *  - kanaly powstaja leniwie przy pierwszym udanym odczycie; nowe kanaly
 *    powoduja restart protokolu Supla (chmura dostaje świeża liste kanalow).
 */

#include "supla_bridge.h"
#include "mapping_nvs.h"
#include "mapping_gate.h"
#include "matter_energy_tlv.h"
#include "matter_supla_meter.h"
#include <memory>
#include <new>

#include <cstdio>
#include <cstring>
#include <utility>
#include <atomic>
#include <cmath>
#include <limits>
#include "version.h"
#include "reporting_policy.h"
#include "device_telemetry.h"
#include "gateway_subdevices.h"
#include "gateway_services.h"
#include <supla/device/register_device.h>
#include <supla/protocol/protocol_layer.h>
#include <cJSON.h>
#include <esp_matter_controller_subscribe_command.h>
#include <esp_matter_controller_client.h>
#include <app/InteractionModelEngine.h>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <esp_matter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <SuplaDevice.h>
#include <supla/channels/channel.h>
#include <supla/control/virtual_relay.h>
#include <supla/network/network.h>
#include <supla/sensor/therm_hygro_meter.h>
#include <supla/time.h>
#include <esp_idf_client.h>

#include <app/AttributePathParams.h>
#include <app/ConcreteAttributePath.h>
#include <app/EventPathParams.h>
#include <lib/core/TLVReader.h>
#include <lib/support/CHIPMem.h>

#include <esp_matter_controller_cluster_command.h>
#include <esp_matter_controller_read_command.h>
#include <esp_matter_controller_utils.h>

#include "quick_add.h"
#include "supla_config.h"

static const char *TAG = "supla_bridge";

/* Certyfikaty wbudowane w komponent supla-device (EXTRA_COMPONENT_DIRS).
 * Nazwy symboli pochodza z EMBED_TXTFILES (tylko nazwa pliku). */
extern const uint8_t supla_org_cert_pem_start[] asm("_binary_supla_org_cert_pem_start");
extern const uint8_t supla_3rd_cert_pem_start[] asm("_binary_supla_3rd_cert_pem_start");

static std::atomic<bool> s_active{false};
static std::atomic<bool> s_starting{false};
static std::atomic<bool> s_poll_started{false};
static std::atomic<bool> s_begin_done{false};
static std::atomic<bool> s_reload_name{false};
static std::atomic<int> s_status_code{STATUS_UNKNOWN};
static portMUX_TYPE s_status_mutex=portMUX_INITIALIZER_UNLOCKED;
static char s_status_msg[160] = "uruchamianie...";

/* ------------------------------------------------------------------------- */
/* Siec: piggyback na Wi-Fi zarzadzanym przez wifi_manager                   */
/* ------------------------------------------------------------------------- */

static bool sta_has_ip(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    return netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0;
}

static uint32_t sta_ip_u32(void)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif != nullptr && esp_netif_get_ip_info(netif, &info) == ESP_OK) {
        return info.ip.addr;
    }
    return 0;
}

class PiggybackNet : public Supla::Network {
 public:
  PiggybackNet()
  {
    intfType = IntfType::WiFi;
  }

  void setup() override
  {
    /* Siec jest juz podniesiona przez wifi_manager - nic do zrobienia. */
  }

  void disable() override
  {
  }

  bool isReady() override
  {
    return sta_has_ip();
  }

  uint32_t getIP() override
  {
    return sta_ip_u32();
  }

  bool getMacAddr(uint8_t *out) override
  {
    return out != nullptr && esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
  }

  Supla::Client *createClient() override
  {
    return new Supla::EspIdfClient();
  }

  const char *getIntfName() const override
  {
    return "WiFi";
  }
};

/* ------------------------------------------------------------------------- */
/* Elementy Supla mapowane na urzadzenia Matter                              */
/* ------------------------------------------------------------------------- */

/* Przekaznik: kanal on/off. Sterowanie z chmury -> komenda Matter OnOff.
 * Zmiana stanu z urzadzenia -> applyFromMatter() (bez wysylania komend). */
class MatterRelayElement : public Supla::Control::VirtualRelay {
 public:
  MatterRelayElement(uint64_t node, uint16_t endpoint)
      : node_(node), endpoint_(endpoint)
  {
  }

  void applyFromMatter(bool on)
  {
    confirmed_state_ = on;
    state = on;
    channel.setNewValue(on);
  }

 protected:
  void setNewChannelValue(bool value) override
  {
    if (!s_begin_done) {
      Supla::Control::VirtualRelay::setNewChannelValue(value);
      return;
    }
    // VirtualRelay sets its state before this hook. Publish only Matter readback.
    state = confirmed_state_;
    if (endpoint_ == m2s::mapping::kNone || !quick_add_device_exists(node_)) return;
    const bool on = value;
    esp_err_t err = gateway::relay_enqueue(node_, endpoint_, on, true);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Supla -> Matter (queued): OnOff=%d (node %llu, ep %u)",
               on ? 1 : 0, (unsigned long long)node_, (unsigned)endpoint_);
    } else {
      ESP_LOGW(TAG, "Matter enqueue OnOff=%d (node %llu, ep %u): blad %d",
               on ? 1 : 0, (unsigned long long)node_, (unsigned)endpoint_, (int)err);
    }
  }

 private:
  bool confirmed_state_ = false;
  uint64_t node_;
  uint16_t endpoint_;
};

/* Termometr + higrometr: jeden kanal (temperatura + wilgotnosc). */
class MatterThermHygroElement : public Supla::Sensor::ThermHygroMeter {
 public:
  MatterThermHygroElement(uint64_t node, uint16_t endpoint)
      : node_(node), endpoint_(endpoint)
  {
    setRefreshIntervalMs(1000);
  }

  double getTemp() override
  {
    return temp_;
  }

  double getHumi() override
  {
    return hum_;
  }

  void setTempFromMatter(double value)
  {
    temp_ = value;
  }

  void setHumiFromMatter(double value)
  {
    hum_ = value;
  }

 private:
  uint64_t node_;
  uint16_t endpoint_;
  double temp_ = TEMPERATURE_NOT_AVAILABLE;
  double hum_ = HUMIDITY_NOT_AVAILABLE;
};

struct ElemSlot {
  uint64_t node;
  uint16_t endpoint;
  MatterRelayElement *relay;
  MatterThermHygroElement *therm;
  uint32_t relay_updated;
  uint32_t temp_updated;
  uint32_t hum_updated;
};

static const size_t kMaxSlots = 64;
static ElemSlot s_slots[kMaxSlots] = {};
static size_t s_slot_count = 0;
static int s_channels_created = 0;

// Append-only identity map. A deleted/offline peer keeps its channel number.
// Rebuild all channels before begin(), so network response order cannot rename them.
struct ChannelBinding {
  uint64_t node;
  uint16_t endpoint;
  uint8_t kind;
  // 1 = relay, 2 = temperature/humidity
  uint8_t reserved[5];
};
struct ChannelMap {
  uint32_t version;
  uint32_t count;
  ChannelBinding entries[64];
};
static ChannelMap s_map = {1, 0, {}};

static bool load_channel_map()
{
  bool v2_present=false;
  if(m2s::mapping::v2_presence(v2_present)!=m2s::mapping::Error::Ok || v2_present) {
    ESP_LOGE(TAG,"Active v2 mapping requires the v2 SUPLA adapter; legacy fallback refused");
    return false;
  }

  nvs_handle_t handle;
  esp_err_t err = nvs_open("m2s_channels", NVS_READONLY, &handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return true;
  if (err != ESP_OK) return false;
  size_t length = sizeof(s_map);
  err = nvs_get_blob(handle, "map", &s_map, &length);
  nvs_close(handle);
  if (err == ESP_ERR_NVS_NOT_FOUND) return true;
  if (err != ESP_OK || length != sizeof(s_map) || s_map.version != 1 || s_map.count > 64) return false;
  for (size_t i = 0; i < s_map.count; ++i) {
    const auto &entry = s_map.entries[i];
    if (entry.node == 0 || entry.endpoint == 0xffff || (entry.kind != 1 && entry.kind != 2)) return false;
    for (size_t j = 0; j < i; ++j) {
      const auto &previous = s_map.entries[j];
      if (entry.node == previous.node && entry.endpoint == previous.endpoint && entry.kind == previous.kind) return false;
    }
  }
  return true;
}

static int reserve_channel(uint64_t node, uint16_t endpoint, uint8_t kind)
{
  for (size_t i = 0; i < s_map.count; ++i) {
    const auto &entry = s_map.entries[i];
    if (entry.node == node && entry.endpoint == endpoint && entry.kind == kind) return (int)i;
  }
  m2s::mapping::MapLock lock;
  bool active = false;
  if (m2s::mapping::v2_presence(active) != m2s::mapping::Error::Ok || active || s_map.count >= 64) return -1;
  const uint32_t index = s_map.count;
  s_map.entries[index] = {node, endpoint, kind, {}};
  ++s_map.count;
  nvs_handle_t handle;
  esp_err_t err = nvs_open("m2s_channels", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_blob(handle, "map", &s_map, sizeof(s_map));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err != ESP_OK) {
    --s_map.count;
    ESP_LOGE(TAG, "Cannot persist channel map: %s", esp_err_to_name(err));
    return -1;
  }
  return (int)index;
}

static ElemSlot *slot_for(uint64_t node, uint16_t endpoint)
{
  for (size_t i = 0; i < s_slot_count; i++) {
    if (s_slots[i].node == node && s_slots[i].endpoint == endpoint) {
      return &s_slots[i];
    }
  }
  if (s_slot_count >= kMaxSlots) {
    return nullptr;
  }
  ElemSlot *slot = &s_slots[s_slot_count++];
  slot->node = node;
  slot->endpoint = endpoint;
  slot->relay = nullptr;
  slot->therm = nullptr;
  return slot;
}

static void apply_caption(Supla::Channel *channel, uint64_t node)
{
  char name[40];
  quick_add_device_name_copy(node,name,sizeof(name));
  if (channel != nullptr && name[0] != 0) {
    channel->setInitialCaption(name);
  }
}

namespace mapping = m2s::mapping;
using m2s::electrical::MatterSuplaMeter;

struct MappedElement {
  MatterRelayElement *relay = nullptr;
  MatterThermHygroElement *therm = nullptr;
  MatterSuplaMeter *meter = nullptr;
  uint32_t updated[3]{};
  bool seen[3]{};
};
static MappedElement s_mapped_elements[mapping::kMaxChannels];
static std::atomic<const mapping::Model *> s_runtime_map{nullptr};

static esp_err_t read_active_mapping(mapping::Model &model)
{
  mapping::NvsBackend backend;
  auto bytes = std::unique_ptr<uint8_t[]>(new (std::nothrow) uint8_t[mapping::kEncodedMax]);
  if (!bytes) return ESP_ERR_NO_MEM;
  supla_cfg_t cfg{};
  if (supla_config_copy(&cfg) != ESP_OK) return ESP_ERR_INVALID_STATE;
  size_t size = 0;
  const auto err = backend.read(mapping::Key::Active, bytes.get(), mapping::kEncodedMax, size);
  if (err == mapping::Error::NotFound) return ESP_ERR_NOT_FOUND;
  if (err != mapping::Error::Ok) return ESP_ERR_INVALID_STATE;
  return mapping::decode(bytes.get(), size, cfg.guid, model) == mapping::Error::Ok
      ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static bool build_mapped_elements(const mapping::Model &model)
{
  for (size_t i = 0; i < model.count; ++i) {
    const auto &binding = model.channels[i];
    auto &element = s_mapped_elements[i];
    Supla::Channel *channel = nullptr;
    switch (binding.kind) {
      case mapping::Kind::Relay:
        element.relay = new (std::nothrow) MatterRelayElement(binding.node, binding.endpoint[0]);
        if (!element.relay) return false;
        channel = element.relay->getChannel();
        channel->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
        if (binding.related_meter != mapping::kNone)
          element.relay->setDefaultRelatedMeterChannelNo(binding.related_meter);
        break;
      case mapping::Kind::Thermohygrometer:
        element.therm = new (std::nothrow) MatterThermHygroElement(binding.node, mapping::kNone);
        if (!element.therm) return false;
        channel = element.therm->getChannel();
        break;
      case mapping::Kind::ElectricityMeter:
        element.meter = new (std::nothrow) MatterSuplaMeter();
        if (!element.meter) return false;
        channel = element.meter->getChannel();
        break;
      default:
        return false;
    }
    if (!channel->setChannelNumber(static_cast<int>(i))) return false;
    channel->setSubDeviceId(gateway::subdevice_id(model,binding.node));
    if(gateway::subdevice_id(model,binding.node)==i+1)new(std::nothrow) gateway::SubdeviceInfo(binding.node,gateway::subdevice_id(model,binding.node));
    channel->setStateOffline();
    apply_caption(channel, binding.node);
  }
  return true;
}

static MatterRelayElement *ensure_relay(ElemSlot *slot)
{
  if (slot->relay != nullptr) {
    return slot->relay;
  }
  int number = reserve_channel(slot->node, slot->endpoint, 1);
  if (number < 0) return nullptr;
  auto *relay = new MatterRelayElement(slot->node, slot->endpoint);
  relay->getChannel()->setChannelNumber(number);
  relay->getChannel()->setStateOffline();
  apply_caption(relay->getChannel(), slot->node);
  relay->getChannel()->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
  slot->relay = relay;
  s_channels_created++;
  return relay;
}

static MatterThermHygroElement *ensure_therm(ElemSlot *slot)
{
  if (slot->therm != nullptr) {
    return slot->therm;
  }
  int number = reserve_channel(slot->node, slot->endpoint, 2);
  if (number < 0) return nullptr;
  auto *therm = new MatterThermHygroElement(slot->node, slot->endpoint);
  therm->getChannel()->setChannelNumber(number);
  therm->getChannel()->setStateOffline();
  apply_caption(therm->getChannel(), slot->node);
  slot->therm = therm;
  s_channels_created++;
  return therm;
}

/* ------------------------------------------------------------------------- */
/* Kolejka zmian atrybutow (poll task / CHIP -> supla_task)                   */
/* ------------------------------------------------------------------------- */

enum : uint8_t {
  UPD_RELAY = 1,
  UPD_TEMP = 2,
  UPD_HUM = 3,
  UPD_ELECTRICAL = 4,
};

struct Update {
  uint64_t node;
  uint16_t endpoint;
  uint8_t kind;
  float value;
  int64_t electrical_value;
  bool valid;
};

static const size_t kQueueCapacity = 192;
static Update s_queue[kQueueCapacity];
static size_t s_queue_head = 0;
static size_t s_queue_tail = 0;
static SemaphoreHandle_t s_queue_mutex = nullptr;

static void queue_update(uint64_t node, uint16_t endpoint, uint8_t kind, float value,
                         int64_t electrical_value = 0, bool valid = true)
{
  if (s_queue_mutex == nullptr) {
    return;
  }
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(20)) != pdTRUE) {
    return;
  }
  size_t next = (s_queue_head + 1) % kQueueCapacity;
  if (next == s_queue_tail) {
    /* Kolejka pelna - porzuc najstarsza pozycje. */
    s_queue_tail = (s_queue_tail + 1) % kQueueCapacity;
  }
  s_queue[s_queue_head].node = node;
  s_queue[s_queue_head].endpoint = endpoint;
  s_queue[s_queue_head].kind = kind;
  s_queue[s_queue_head].value = value;
  s_queue[s_queue_head].electrical_value = electrical_value;
  s_queue[s_queue_head].valid = valid;
  s_queue_head = next;
  xSemaphoreGive(s_queue_mutex);
}

static bool dequeue_update(Update *out)
{
  if (s_queue_mutex == nullptr) {
    return false;
  }
  bool ok = false;
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (s_queue_tail != s_queue_head) {
      *out = s_queue[s_queue_tail];
      s_queue_tail = (s_queue_tail + 1) % kQueueCapacity;
      ok = true;
    }
    xSemaphoreGive(s_queue_mutex);
  }
  return ok;
}

/* ------------------------------------------------------------------------- */
/* Odczyt atrybutow Matter                                                   */
/* ------------------------------------------------------------------------- */

static void on_attribute_report(uint64_t node,
                                const chip::app::ConcreteDataAttributePath &path,
                                chip::TLV::TLVReader *data,
                                const chip::app::StatusIB &status,
                                device_telemetry::Source source=device_telemetry::Source::Read)
{
  if(!quick_add_device_exists(node))return;
  if(path.mClusterId==0x002f && path.mAttributeId==0) return;
  if(path.mClusterId==0x002f && path.mAttributeId==0x000c) {
    if(status.ToChipError()==CHIP_NO_ERROR && data) {
      uint8_t raw=0;bool valid=data->Get(raw)==CHIP_NO_ERROR && raw<=200;
      device_telemetry::observe(node,path.mEndpointId,device_telemetry::Metric::Battery,valid,raw,source);
    }
    return;
  }
  if (path.mClusterId == 0x0090 || path.mClusterId == 0x0091) {
    for (uint8_t role = static_cast<uint8_t>(mapping::Role::Voltage);
         role <= static_cast<uint8_t>(mapping::Role::ExportedEnergy); ++role) {
      uint32_t cluster, attribute;
      mapping::source_path(static_cast<mapping::Role>(role), cluster, attribute);
      if (cluster != path.mClusterId || attribute != path.mAttributeId) continue;
      int64_t value = 0;
      bool valid = status.ToChipError() == CHIP_NO_ERROR && data &&
                   data->GetType() != chip::TLV::kTLVType_Null;
      if (valid) {
        valid = cluster == 0x0091 ? m2s::electrical::read_energy(*data, value)
                                  : data->Get(value) == CHIP_NO_ERROR;
      }
      if(status.ToChipError()==CHIP_NO_ERROR && data)
        device_telemetry::observe(node,path.mEndpointId,static_cast<device_telemetry::Metric>(
          static_cast<unsigned>(device_telemetry::Metric::Voltage)+role-static_cast<unsigned>(mapping::Role::Voltage)),valid,value,source);
      queue_update(node, path.mEndpointId, UPD_ELECTRICAL + role, 0, value, valid);
      return;
    }
    return;
  }
  if (status.ToChipError() != CHIP_NO_ERROR || path.mAttributeId != 0) {
    return;
  }

  switch (path.mClusterId) {
    case 0x0006: {  /* OnOff */
      bool on = false;
      if (data != nullptr && data->Get(on) == CHIP_NO_ERROR) {
        device_telemetry::observe(node,path.mEndpointId,device_telemetry::Metric::OnOff,true,on?1:0,source);
        queue_update(node, path.mEndpointId, UPD_RELAY, on ? 1.0f : 0.0f);
      }
      break;
    }
    case 0x0402: {  /* TemperatureMeasurement, setne stopnie C */
      int16_t value = 0;
      bool valid=data && data->Get(value)==CHIP_NO_ERROR && value>=-27315;
      device_telemetry::observe(node,path.mEndpointId,device_telemetry::Metric::Temperature,valid,value,source);
      queue_update(node,path.mEndpointId,UPD_TEMP,valid?(float)value/100.0f:std::numeric_limits<float>::quiet_NaN());
      break;
    }
    case 0x0405: {  /* RelativeHumidityMeasurement, setne % */
      uint16_t value = 0;
      bool valid=data && data->Get(value)==CHIP_NO_ERROR && value<=10000;
      device_telemetry::observe(node,path.mEndpointId,device_telemetry::Metric::Humidity,valid,value,source);
      queue_update(node,path.mEndpointId,UPD_HUM,valid?(float)value/100.0f:std::numeric_limits<float>::quiet_NaN());
      break;
    }
    default:
      break;
  }
}

static bool measurement_paths(uint64_t node, chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &paths)
{
  bool read_electrical = false;
  if (const auto *model = s_runtime_map.load()) {
    for (size_t i = 0; i < model->count; ++i) {
      const auto &channel = model->channels[i];
      if (channel.node == node && !channel.retired && channel.kind == mapping::Kind::ElectricityMeter) {
        read_electrical = true;
        break;
      }
    }
  }
  if (!paths.Alloc(read_electrical ? 9 : 4)) return false;
  paths[0] = chip::app::AttributePathParams(0xffff, 0x0006, 0);
  paths[1] = chip::app::AttributePathParams(0xffff, 0x0402, 0);
  paths[2] = chip::app::AttributePathParams(0xffff, 0x0405, 0);
  paths[read_electrical?8:3]=chip::app::AttributePathParams(0xffff,0x002f,0x000c);
  if (read_electrical) {
    for (uint8_t role = static_cast<uint8_t>(mapping::Role::Voltage);
         role <= static_cast<uint8_t>(mapping::Role::ExportedEnergy); ++role) {
      uint32_t cluster, attribute;
      mapping::source_path(static_cast<mapping::Role>(role), cluster, attribute);
      paths[role] = chip::app::AttributePathParams(0xffff, cluster, attribute);
    }
  }

  return true;
}


// Transport ownership stays on the CHIP thread/lock. Only the small liveness
// snapshot crosses to the SUPLA task; channel values still use the existing queue.
struct ReportingState {
  uint64_t node=0;
  bool owned=false, has_attempt=false, has_poll=false;
  uint32_t attempted=0, polled=0;
};
struct ReportingLive {
  uint64_t node=0;
  bool active=false;
  uint32_t report=0;
  uint16_t maximum=600;
};
static constexpr size_t kMaxReportingNodes = 64;
static constexpr size_t kMaxNewSubscribesPerTick = 3;
static ReportingState s_reporting[kMaxReportingNodes];
static ReportingLive s_reporting_live[kMaxReportingNodes];
static portMUX_TYPE s_reporting_mutex=portMUX_INITIALIZER_UNLOCKED;
static void reporting_live(size_t slot,bool active,bool report=false,uint16_t maximum=0)
{
  portENTER_CRITICAL(&s_reporting_mutex);
  auto &live=s_reporting_live[slot];live.node=s_reporting[slot].node;live.active=active;
  if(report)live.report=millis();
  if(maximum)live.maximum=maximum;
  portEXIT_CRITICAL(&s_reporting_mutex);
  device_telemetry::subscription(s_reporting[slot].node,active);
}
static bool reporting_fresh(uint64_t node,uint32_t now)
{
  bool fresh=false;
  portENTER_CRITICAL(&s_reporting_mutex);
  for(const auto &live:s_reporting_live)if(live.node==node)
    fresh=m2s::reporting::alive(live.active,now,live.report,live.maximum);
  portEXIT_CRITICAL(&s_reporting_mutex);
  return fresh;
}

// Exactly one owner per node: replace its prior server-side subscriptions on
// reconnect, including handlers left behind by a gateway restart. Other
// subscriber node/fabric identities are unaffected.
class BridgeSubscription final : public esp_matter::controller::subscribe_command {
public:
  BridgeSubscription(size_t slot,uint64_t node,uint16_t min_interval_s,
      chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> &&paths)
    : subscribe_command(node,std::move(paths),{},min_interval_s,600,true,
        [](uint64_t peer,const auto &path,auto *data,const auto &status){
          if(quick_add_device_exists(peer))on_attribute_report(peer,path,data,status,device_telemetry::Source::Subscription);
        },nullptr,nullptr,nullptr,nullptr,false),slot_(slot),node_(node) {}
  ~BridgeSubscription() override {
    auto &state=s_reporting[slot_];
    reporting_live(slot_,false);
    state.owned=false;state.attempted=millis();state.has_attempt=true;
    ESP_LOGI(TAG,"Matter reporting node %llu: released, retry after backoff",(unsigned long long)node_);
  }
  void OnSubscriptionEstablished(chip::SubscriptionId id) override {
    reporting_live(slot_,true,true,600);
    ESP_LOGI(TAG,"Matter reporting node %llu: established, id %lu",(unsigned long long)node_,(unsigned long)id);
    subscribe_command::OnSubscriptionEstablished(id);
  }
  void OnReportEnd() override {
    // Priming reports may precede SubscribeResponse: do not mark established early.
    portENTER_CRITICAL(&s_reporting_mutex);
    s_reporting_live[slot_].report=millis();
    portEXIT_CRITICAL(&s_reporting_mutex);
    device_telemetry::report(node_);
  }
  void OnUnsolicitedMessageFromPublisher(chip::app::ReadClient *client) override {
    uint16_t minimum=0,maximum=0;
    if(client->GetReportingIntervals(minimum,maximum)==CHIP_NO_ERROR) {
      portENTER_CRITICAL(&s_reporting_mutex);
      s_reporting_live[slot_].maximum=maximum;
      portEXIT_CRITICAL(&s_reporting_mutex);
      device_telemetry::intervals(node_,minimum,maximum);
    }
  }
  CHIP_ERROR OnResubscriptionNeeded(chip::app::ReadClient *client,CHIP_ERROR cause) override {
    reporting_live(slot_,false);
    ESP_LOGW(TAG,"Matter reporting node %llu: resubscribe (%s)",(unsigned long long)node_,chip::ErrorStr(cause));
    if(!quick_add_device_exists(node_))return cause;
    // The SDK wrapper stops after two retries. Keep CHIP's bounded exponential
    // backoff for sleepy devices instead of permanently abandoning reporting.
    return client->DefaultResubscribePolicy(cause);
  }
  void OnError(CHIP_ERROR error) override {
    reporting_live(slot_,false);
    subscribe_command::OnError(error);
  }
private:
  size_t slot_;uint64_t node_;
};

// Returns true while subscription establishment/reporting owns the normal path.
static bool maintain_reporting(uint64_t node,uint16_t min_interval_s,bool allow_new_subscribe,bool &started_new)
{
  started_new=false;
  // Caller holds CHIP lock. Never replace a live owner after an arbitrary timeout.
  size_t slot=0;
  while(slot<kMaxReportingNodes && s_reporting[slot].node!=node)++slot;
  if(slot==kMaxReportingNodes) {
    for(slot=0;slot<kMaxReportingNodes && s_reporting[slot].node;++slot){}
    if(slot==kMaxReportingNodes)return false;
    s_reporting[slot].node=node;
  }
  auto &state=s_reporting[slot];const uint32_t now=millis();
  if(!state.owned && allow_new_subscribe && m2s::reporting::retry_due(state.has_attempt,now,state.attempted)) {
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> paths;
    if(!measurement_paths(node,paths))return false;
    state.attempted=now;state.has_attempt=true;
    auto *command=chip::Platform::New<BridgeSubscription>(slot,node,min_interval_s,std::move(paths));
    if(command) {
      state.owned=true;
      started_new=true;
      ESP_LOGI(TAG,"Matter reporting node %llu: subscribing (min=%us)",(unsigned long long)node,(unsigned)min_interval_s);
      // send_command may synchronously delete command on failure.
      command->send_command();
    }
  }
  return reporting_fresh(node,now) || (state.owned && uint32_t(now-state.attempted)<60000);
}
static void release_removed_reporting()
{
  for(size_t i=0;i<kMaxReportingNodes;++i) {
    auto &state=s_reporting[i];
    if(!state.node || quick_add_device_exists(state.node))continue;
    reporting_live(i,false);
    device_telemetry::forget(state.node);
    device_telemetry::forget_thread(state.node);
    if(state.owned)chip::app::InteractionModelEngine::GetInstance()->ShutdownSubscriptions(
      esp_matter::controller::matter_controller_client::get_instance().get_fabric_index(),state.node);
    // A CASE connect may still own its callback. Keep its slot until destruction.
    if(!state.owned) {
      portENTER_CRITICAL(&s_reporting_mutex);s_reporting_live[i]={};portEXIT_CRITICAL(&s_reporting_mutex);
      state={};
    }
  }
}

// Accessed only with the CHIP lock or from a CHIP callback.
struct PendingRead {
  uint64_t node = 0;
  uint32_t id = 0;
  uint32_t started = 0;
};
static PendingRead s_reads[16];
static uint32_t s_read_id = 0;

static PendingRead *find_read(uint64_t node, uint32_t id)
{
  for (auto &read : s_reads)
    if (read.node == node && read.id == id) return &read;
  return nullptr;
}

static void finish_read(uint64_t node, uint32_t id)
{
  if (auto *read = find_read(node, id)) *read = {};
}

// False means all slots are occupied. The caller can resume at this device.
static bool poll_device(uint64_t node,uint16_t min_interval_s,bool allow_new_subscribe,bool &started_new)
{
  esp_matter::lock::ScopedChipStackLock chip_lock(portMAX_DELAY);
  const uint32_t now = millis();
  release_removed_reporting();
  const bool reporting = maintain_reporting(node,min_interval_s,allow_new_subscribe,started_new);
  if(reporting_fresh(node,now))device_telemetry::refresh_thread(node);
  if(reporting)return true;
  // If we held back a new subscription due to per-tick pacing, wait for the next 1s tick before fallback polling.
  if(!allow_new_subscribe)return true;
  // Poll only as a fallback and respect the saved per-device interval.
  ReportingState *policy=nullptr;
  for(auto &entry:s_reporting)if(entry.node==node)policy=&entry;
  const auto *model=s_runtime_map.load();
  const uint32_t interval=model?mapping::interval(*model,node):15;
  if(policy && !m2s::reporting::poll_due(policy->has_poll,now,policy->polled,interval))return true;
  PendingRead *free_slot = nullptr;
  for (auto &read : s_reads) {
    if (read.node && uint32_t(now - read.started) >= 60000) read = {};
    if (read.node == node) return true;
    if (!read.node && !free_slot) free_slot = &read;
  }
  if (!free_slot) return false;

  chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> paths;
  if (!measurement_paths(node, paths)) return true;

  const uint32_t id = ++s_read_id;
  chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> events;
  auto *command = chip::Platform::New<esp_matter::controller::read_command>(
      node, std::move(paths), std::move(events),
      [id](uint64_t peer, const auto &path, auto *data, const auto &status) {
        if (find_read(peer, id)) on_attribute_report(peer, path, data, status);
      },
      [id](uint64_t peer, const auto &, const auto &) { finish_read(peer, id); },
      nullptr,
      [node, id](void *, const auto &, CHIP_ERROR) { finish_read(node, id); },
      [id](uint64_t peer, CHIP_ERROR) { finish_read(peer, id); });
  if (!command) return true;

  if(policy){policy->has_poll=true;policy->polled=now;}
  *free_slot = {node, id, now};
  if (command->send_command() != ESP_OK) finish_read(node, id);
  return true;
}

static void poll_task(void *)
{
  /* Daj chwile Matter/Thread na start po restarcie. */
  vTaskDelay(pdMS_TO_TICKS(6000));

  size_t cursor = 0;
  for (;;) {
    // Matter reporting & telemetry for WebUI/Android/MQTT run regardless of SUPLA Cloud configuration.
    {esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);release_removed_reporting();}
    uint64_t nodes[kMaxReportingNodes];
    size_t count = quick_add_devices_copy(nodes, sizeof(nodes) / sizeof(nodes[0]));
    // Adaptive MinInterval: 0s for <=8 devices, 2s for 9..24 devices, 5s for 25..64 devices
    const uint16_t min_interval_s = (count <= 8) ? 0 : (count <= 24 ? 2 : 5);
    size_t new_subscribes_this_tick = 0;
    if (count) cursor %= count;
    for (size_t i = 0; i < count; ++i) {
      bool started_new = false;
      const bool allow_new = (new_subscribes_this_tick < kMaxNewSubscribesPerTick);
      if (!poll_device(nodes[cursor], min_interval_s, allow_new, started_new)) break;
      if (started_new) ++new_subscribes_this_tick;
      cursor = (cursor + 1) % count;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

static void ensure_poll_task_started(void)
{
  if (s_poll_started.exchange(true)) return;
  if (xTaskCreate(poll_task, "supla_poll", 6144, nullptr, 4, nullptr) != pdPASS) {
    s_poll_started.store(false);
    ESP_LOGE(TAG, "Nie utworzono zadania odczytow Matter");
  }
}

/* ------------------------------------------------------------------------- */
/* Petla supla-device (jedyne miejsce modyfikacji elementow)                  */
/* ------------------------------------------------------------------------- */

static void drain_mapped(const mapping::Model &model)
{
  Update update;
  const uint32_t now = millis();
  while (dequeue_update(&update)) {
    if (!quick_add_device_exists(update.node)) continue;
    const auto role = static_cast<mapping::Role>(
        update.kind >= UPD_ELECTRICAL ? update.kind - UPD_ELECTRICAL : update.kind - 1);
    const int index = mapping::source_channel(model, update.node, update.endpoint, role);
    if (index < 0) continue;
    auto &element = s_mapped_elements[index];
    if(role==mapping::Role::Temperature || role==mapping::Role::Humidity) {
      const bool valid=std::isfinite(update.value);
      device_telemetry::applied(update.node,update.endpoint,role==mapping::Role::Temperature?device_telemetry::Metric::Temperature:device_telemetry::Metric::Humidity,
        valid,valid?std::lround(update.value*100):0,index);
    }
    if (element.meter) {
      if(role>=mapping::Role::Voltage && role<=mapping::Role::ExportedEnergy)
        device_telemetry::applied(update.node,update.endpoint,static_cast<device_telemetry::Metric>(
          static_cast<unsigned>(device_telemetry::Metric::Voltage)+static_cast<unsigned>(role)-static_cast<unsigned>(mapping::Role::Voltage)),
          update.valid,update.electrical_value,index);
      element.meter->sample(role, update.valid, update.electrical_value, now);
      continue;
    }
    const auto field = static_cast<unsigned>(role);
    if (field >= 3) continue;
    element.updated[field] = now;
    element.seen[field] = std::isfinite(update.value);
    if (element.relay) {
      device_telemetry::applied(update.node,update.endpoint,device_telemetry::Metric::OnOff,true,update.value>=0.5f?1:0,index);
      element.relay->applyFromMatter(update.value >= 0.5f);
    } else if (element.therm && role == mapping::Role::Temperature) {
      element.therm->setTempFromMatter(element.seen[field] ? update.value : TEMPERATURE_NOT_AVAILABLE);
    } else if (element.therm && role == mapping::Role::Humidity) {
      element.therm->setHumiFromMatter(element.seen[field] ? update.value : HUMIDITY_NOT_AVAILABLE);
    }
  }
  for (size_t i = 0; i < model.count; ++i) {
    const auto &binding = model.channels[i];
    auto &element = s_mapped_elements[i];
    const bool registered = !binding.retired && quick_add_device_exists(binding.node);
    const uint32_t limit = mapping::stale_after(model, binding.node, element.relay != nullptr) * 1000;
    const bool reporting=reporting_fresh(binding.node,now);
    const auto fresh = [&](unsigned field) {
      return registered && element.seen[field] && (reporting || uint32_t(now - element.updated[field]) <= limit);
    };
    if (element.meter) {
      element.meter->publish(now, limit, registered,reporting);
    } else if (element.relay) {
      if (fresh(0)) element.relay->getChannel()->setStateOnline();
      else element.relay->getChannel()->setStateOffline();
    } else if (element.therm) {
      const bool temp = fresh(1);
      const bool hum = fresh(2);
      if (!temp) element.therm->setTempFromMatter(TEMPERATURE_NOT_AVAILABLE);
      if (!hum) element.therm->setHumiFromMatter(HUMIDITY_NOT_AVAILABLE);
      if (temp || hum) element.therm->getChannel()->setStateOnline();
      else element.therm->getChannel()->setStateOffline();
    }
  }
}

static void drain_updates(void)
{
  if (const auto *model = s_runtime_map.load()) {
    drain_mapped(*model);
    return;
  }
  Update update;
  while (dequeue_update(&update)) {
    if(!quick_add_device_exists(update.node))continue;
    ElemSlot *slot=nullptr;
    if(!std::isfinite(update.value)){for(size_t i=0;i<s_slot_count;i++)if(s_slots[i].node==update.node&&s_slots[i].endpoint==update.endpoint)slot=&s_slots[i];}
    else slot=slot_for(update.node,update.endpoint);
    if (slot == nullptr) {
      continue;
    }
    if (update.kind == UPD_RELAY) {
      MatterRelayElement *relay = ensure_relay(slot);
      if (relay != nullptr) {
        relay->applyFromMatter(update.value >= 0.5f);
        slot->relay_updated = millis();
        relay->getChannel()->setStateOnline();
      }
    } else if (update.kind == UPD_TEMP || update.kind == UPD_HUM) {
      MatterThermHygroElement *therm = std::isfinite(update.value)?ensure_therm(slot):slot->therm;
      if (therm == nullptr) {
        continue;
      }
      if (update.kind == UPD_TEMP) {
        therm->setTempFromMatter(std::isfinite(update.value)?update.value:TEMPERATURE_NOT_AVAILABLE);
        slot->temp_updated=std::isfinite(update.value)?millis():0;
      } else {
        therm->setHumiFromMatter(std::isfinite(update.value)?update.value:HUMIDITY_NOT_AVAILABLE);
        slot->hum_updated=std::isfinite(update.value)?millis():0;
      }
      const bool valid=std::isfinite(update.value);
      device_telemetry::applied(update.node,update.endpoint,update.kind==UPD_TEMP?device_telemetry::Metric::Temperature:device_telemetry::Metric::Humidity,
        valid,valid?std::lround(update.value*100):0,therm->getChannel()->getChannelNumber());

    }
  }

  const uint32_t now = millis();
  for (size_t i = 0; i < s_slot_count; ++i) {
    auto &slot = s_slots[i];

    const bool registered=quick_add_device_exists(slot.node);
    const bool reporting=reporting_fresh(slot.node,now);
    if(slot.relay&&(!registered||!slot.relay_updated||(!reporting&&(uint32_t)(now-slot.relay_updated)>180000)))slot.relay->getChannel()->setStateOffline();
    if(slot.therm){
      bool temperature=registered&&slot.temp_updated&&(reporting||(uint32_t)(now-slot.temp_updated)<=900000);
      bool humidity=registered&&slot.hum_updated&&(reporting||(uint32_t)(now-slot.hum_updated)<=900000);
      if(!temperature)slot.therm->setTempFromMatter(TEMPERATURE_NOT_AVAILABLE);
      if(!humidity)slot.therm->setHumiFromMatter(HUMIDITY_NOT_AVAILABLE);
      if(temperature||humidity)slot.therm->getChannel()->setStateOnline();
      else slot.therm->getChannel()->setStateOffline();
    }
  }

  if (s_channels_created > 0) {
    ESP_LOGI(TAG, "Nowe kanaly Supla: %d - restart protokolu", s_channels_created);
    s_channels_created = 0;
    SuplaDevice.scheduleProtocolsRestart(0);
  }
}

static void supla_task(void *)
{
  auto model = std::unique_ptr<mapping::Model>(new (std::nothrow) mapping::Model);
  const auto active = model ? read_active_mapping(*model) : ESP_ERR_NO_MEM;
  if (active == ESP_OK && build_mapped_elements(*model)) {
    s_runtime_map.store(model.release());
  } else if (active != ESP_ERR_NOT_FOUND || !load_channel_map()) {
    ESP_LOGE(TAG, "Invalid channel map; refusing to renumber existing SUPLA channels");
    s_active=false;
    vTaskDelete(nullptr);
    return;
  }
  for (size_t i = 0; !s_runtime_map.load() && i < s_map.count; ++i) {
    const auto &entry = s_map.entries[i];
    ElemSlot *slot = slot_for(entry.node, entry.endpoint);
    if (slot == nullptr) {
      ESP_LOGE(TAG, "Channel map exceeds endpoint capacity");
      s_active=false;
      vTaskDelete(nullptr);
      return;
    }
    if (entry.kind == 1) ensure_relay(slot);
    else ensure_therm(slot);
  }
  s_channels_created = 0;
  supla_cfg_t cfg_value{};
  if(supla_config_copy(&cfg_value)!=ESP_OK){s_active=false;
  vTaskDelete(nullptr);
  return;}
  const supla_cfg_t *cfg=&cfg_value;
  ESP_LOGI(TAG, "Start Supla: serwer='%s', e-mail='%s'", cfg->server, cfg->email);

  char gateway_name[gateway::gateway_name_capacity];
  gateway::read_gateway_name(gateway_name);
  SuplaDevice.setName(gateway_name);
  s_begin_done = false;
  bool ok = SuplaDevice.begin((const char *)cfg->guid,
                              cfg->server,
                              cfg->email,
                              (const char *)cfg->authkey,25);
  s_begin_done = true;
  ESP_LOGI(TAG, "SuplaDevice.begin(): %s", ok ? "OK" : "blad");

  for (;;) {
    if (s_reload_name.exchange(false)) {
      gateway::read_gateway_name(gateway_name);
      if (strcmp(gateway_name, Supla::RegisterDevice::getName()) != 0) {
        SuplaDevice.setName(gateway_name);
        for (auto *protocol = Supla::Protocol::ProtocolLayer::first(); protocol; protocol = protocol->next()) {
          if (protocol->getSdc() == &SuplaDevice && !protocol->isMqtt()) protocol->disconnect();
        }
        ESP_LOGI(TAG, "SUPLA registration name changed; reconnect requested");
      }
    }
    drain_updates();
    SuplaDevice.iterate();
    vTaskDelay(1);
  }
}

/* ------------------------------------------------------------------------- */
/* Start / status dla panelu                                                 */
/* ------------------------------------------------------------------------- */

static void supla_status_cb(int status, const char *msg)
{
  s_status_code = status;
  portENTER_CRITICAL(&s_status_mutex);
  snprintf(s_status_msg, sizeof(s_status_msg), "%s", msg != nullptr ? msg : "");
  portEXIT_CRITICAL(&s_status_mutex);
  ESP_LOGI(TAG, "status %d", status);
}

extern "C" void supla_bridge_request_name_reload(void)
{
  s_reload_name.store(true);
}

extern "C" void supla_bridge_start(void)
{
  ensure_poll_task_started();
  if (s_active || s_starting.exchange(true)) {
    return;
  }
  struct ClearStarting { ~ClearStarting() { s_starting.store(false); } } clear_starting;

  supla_config_load();
  supla_cfg_t cfg_value{};
  if(supla_config_copy(&cfg_value)!=ESP_OK){s_active=false;
  return;}
  const supla_cfg_t *cfg=&cfg_value;
  if (!cfg->configured) {
    ESP_LOGI(TAG, "Supla nieaktywna - brak konfiguracji (panel -> zakladka Supla)");
    return;
  }

  if (s_queue_mutex == nullptr) {
    s_queue_mutex = xSemaphoreCreateMutex();
  }
  if (s_queue_mutex == nullptr) {
    ESP_LOGE(TAG, "Brak pamieci na mutex kolejki Supla");
    return;
  }

  /* Instancja sieci musi zyc przez caly czas programu (rejestruje sie
   * w statycznej liscie supla-device). */
  static PiggybackNet *network = new PiggybackNet();
  (void)network;

  SuplaDevice.setSwVersion("m2s-" FW_VERSION);
  SuplaDevice.setSuplaCACert((const char *)supla_org_cert_pem_start);
  SuplaDevice.setSupla3rdPartyCACert((const char *)supla_3rd_cert_pem_start);
  SuplaDevice.setStatusFuncImpl(supla_status_cb);
  SuplaDevice.setAutomaticResetOnConnectionProblem(0);

  s_active = true;

  if (xTaskCreate(supla_task, "supla", 12288, nullptr, 5, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "Nie udalo sie utworzyc zadania Supla");
    s_active = false;
    return;
  }

  ESP_LOGI(TAG, "Warstwa Supla uruchomiona");
}

extern "C" bool supla_bridge_is_started(void)
{
  return s_active;
}

extern "C" int supla_bridge_status_code(void)
{
  return s_status_code;
}


extern "C" void supla_bridge_status_message_copy(char *out,size_t size) {
  if(!out||!size)return;
  portENTER_CRITICAL(&s_status_mutex);
  snprintf(out,size,"%s",s_status_msg);
  portEXIT_CRITICAL(&s_status_mutex);
}
extern "C" esp_err_t supla_bridge_max_reserved_node(uint64_t *maximum) {
  if(!maximum)return ESP_ERR_INVALID_ARG;
  *maximum=0;
  bool v2_present=false;
  if (mapping::v2_presence(v2_present) != mapping::Error::Ok) return ESP_ERR_INVALID_STATE;
  if (v2_present) {
    auto model = std::unique_ptr<mapping::Model>(new (std::nothrow) mapping::Model);
    if (!model) return ESP_ERR_NO_MEM;
    const auto err = read_active_mapping(*model);
    if (err != ESP_OK) return err;
    *maximum = mapping::max_node(*model);
    return ESP_OK;
  }
  nvs_handle_t h;
  esp_err_t err=nvs_open("m2s_channels",NVS_READONLY,&h);
  if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
  if(err!=ESP_OK)return err;
  auto map = std::unique_ptr<ChannelMap>(new(std::nothrow) ChannelMap{});
  if (!map) { nvs_close(h); return ESP_ERR_NO_MEM; }
  size_t n=sizeof(*map);
  err=nvs_get_blob(h,"map",map.get(),&n);
  nvs_close(h);
  if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
  if(err!=ESP_OK)return err;
  if(n!=sizeof(*map)||map->version!=1||map->count>64)return ESP_ERR_INVALID_STATE;
  for(size_t i=0;i<map->count;i++)if(map->entries[i].node>*maximum)*maximum=map->entries[i].node;
  return ESP_OK;
}
extern "C" const char *supla_bridge_state_name(void)
{
  if (!s_active) {
    supla_cfg_t cfg{};
    return supla_config_copy(&cfg)==ESP_OK&&cfg.configured?"starting":"not_configured";
  }

  switch (s_status_code.load()) {
    case STATUS_REGISTERED_AND_READY:
      return "connected";
    case STATUS_BAD_CREDENTIALS:
    case STATUS_INVALID_AUTHKEY:
    case STATUS_INVALID_GUID:
    case STATUS_MISSING_CREDENTIALS:
    case STATUS_REGISTRATION_DISABLED:
    case STATUS_DEVICE_IS_DISABLED:
    case STATUS_LOCATION_IS_DISABLED:
    case STATUS_DEVICE_LIMIT_EXCEEDED:
      return "error";
    default:
      return "connecting";
  }
}

// Use a direct OnOff read when the command exchange finishes. A live
// subscription can still be waiting for its next report.
static void read_relay_after_command(uint64_t node, uint16_t endpoint)
{
  if (!quick_add_device_exists(node)) return;
  using namespace esp_matter::controller;
  auto *read = chip::Platform::New<read_command>(
      node, endpoint, 0x0006, 0, READ_ATTRIBUTE,
      [endpoint](uint64_t peer, const auto &path, auto *data, const auto &status) {
        if (path.mEndpointId == endpoint && path.mClusterId == 0x0006 && path.mAttributeId == 0) {
          on_attribute_report(peer, path, data, status);
        }
      },
      nullptr, nullptr);
  if (!read) {
    ESP_LOGW(TAG, "OnOff read-back allocation failed");
    return;
  }
  if (read->send_command() != ESP_OK) {
    ESP_LOGW(TAG, "OnOff read-back could not be sent");
  }
}

extern "C" esp_err_t supla_bridge_mqtt_command(uint64_t node,uint16_t ep,bool on,bool require_proof=true){
 if(!quick_add_device_exists(node)||ep==0||ep==0xffff)return ESP_ERR_INVALID_ARG;
 if(require_proof){
  device_telemetry::Reading proof;
  if(!device_telemetry::lookup(node,ep,device_telemetry::Metric::OnOff,proof)||!proof.valid)return ESP_ERR_INVALID_ARG;
 }
 esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
 using esp_matter::controller::cluster_command;
 auto *command = chip::Platform::New<cluster_command>(
     node, ep, 0x0006, on ? 1 : 0, nullptr, chip::NullOptional,
     [node, ep](void *, const auto &, const auto &status, auto *) {
       if (status.ToChipError() == CHIP_NO_ERROR) {
         read_relay_after_command(node, ep);
       } else {
         ESP_LOGW(TAG, "OnOff command rejected: %s", chip::ErrorStr(status.ToChipError()));
       }
     },
     [node, ep](void *, CHIP_ERROR error) {
       ESP_LOGW(TAG, "OnOff command exchange failed: %s", chip::ErrorStr(error));
       // A lost response does not tell us whether the relay changed.
       read_relay_after_command(node, ep);
     },
     [](void *, const auto &, CHIP_ERROR error) {
       ESP_LOGW(TAG, "OnOff CASE connection failed: %s", chip::ErrorStr(error));
     });
 return command ? command->send_command() : ESP_ERR_NO_MEM;
}
extern "C" uint32_t supla_bridge_stale_seconds(uint64_t node,bool relay){const auto *model=s_runtime_map.load();return model?mapping::stale_after(*model,node,relay):(relay?180:900);}

extern "C" cJSON *supla_bridge_cloud_map(){const auto *model=s_runtime_map.load();if(!model)return nullptr;supla_cfg_t cfg{};if(supla_config_copy(&cfg)!=ESP_OK)return nullptr;auto *j=cJSON_CreateObject();if(!j)return nullptr;char guid[33];supla_config_bytes_to_hex(cfg.guid,guid);cJSON_AddStringToObject(j,"guid",guid);auto *channels=cJSON_AddArrayToObject(j,"channels");for(size_t i=0;i<model->count;++i){auto *c=cJSON_CreateObject();if(!c){cJSON_Delete(j);return nullptr;}cJSON_AddItemToArray(channels,c);cJSON_AddNumberToObject(c,"number",i);cJSON_AddNumberToObject(c,"subdevice_id",gateway::subdevice_id(*model,model->channels[i].node));cJSON_AddBoolToObject(c,"retired",model->channels[i].retired);}return j;}
