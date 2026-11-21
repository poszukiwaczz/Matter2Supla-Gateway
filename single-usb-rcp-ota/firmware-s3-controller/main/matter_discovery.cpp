#include "matter_discovery.h"
#include "matter_energy_tlv.h"
#include "mapping_core.h"
#include "quick_add.h"
#include "matter_ota.h"
#include "input_validation.h"
#include <cstring>
#include <esp_log.h>

#include <esp_matter.h>
#include <esp_matter_controller_read_command.h>
#include <esp_timer.h>
#include <lib/core/TLVReader.h>
#include <lib/support/CHIPMem.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <utility>
#include <cstdio>

namespace matter_discovery {
namespace {
constexpr size_t kEndpoints = 24;
constexpr size_t kClusters = 32;
constexpr int64_t kTimeoutUs = 60000000;
constexpr uint32_t kDescriptor = 0x001d;
constexpr uint32_t kPowerMeasurement = 0x0090;
constexpr uint32_t kEnergyMeasurement = 0x0091;
using m2s::electrical::kMeasurementLimit;
using m2s::electrical::read_energy;

struct ElectricalPath {
    uint32_t cluster;
    uint32_t attribute;
    const char *key;
    bool cumulative;
};
constexpr ElectricalPath kElectricalPaths[] = {
    {kPowerMeasurement, 0, "power_mode", false},
    {kPowerMeasurement, 4, "voltage_mv", false},
    {kPowerMeasurement, 5, "active_current_ma", false},
    {kPowerMeasurement, 8, "active_power_mw", false},
    {kPowerMeasurement, 0xfffc, "power_features", false},
    {kEnergyMeasurement, 1, "imported_mwh", true},
    {kEnergyMeasurement, 2, "exported_mwh", true},
    {kEnergyMeasurement, 0xfffc, "energy_features", false},
    {0x0006, 0, "onoff", false},
    {0x0006, 0xfff9, "onoff_commands", false},
    {0x0402, 0, "temperature", false},
    {0x0405, 0, "humidity", false},
};
constexpr size_t kElectricalPathCount = sizeof(kElectricalPaths) / sizeof(kElectricalPaths[0]);
enum class SampleState : uint8_t { Missing, Value, Null, Error };
struct Sample {
    int64_t value = 0;
    SampleState state = SampleState::Missing;
};
struct ElectricalEndpoint {
    uint16_t id = 0;
    Sample samples[kElectricalPathCount]{};
};
enum class State { Idle, Reading, Complete, Incomplete, Limit, Failed };

struct Endpoint {
    uint16_t id;
    uint32_t clusters[kClusters];
    size_t count;
};
struct Metadata {
    char product[40]{};
    char label[40]{};
    uint16_t vid=0, pid=0;
    bool has_vid=false, has_pid=false, has_version=false;
    uint32_t version=0;char version_string[40]{};
};
struct Snapshot {
    Metadata metadata{};
    State state = State::Idle;
    uint64_t node = 0;
    uint32_t generation = 0;
    int64_t started = 0;
    Endpoint endpoints[kEndpoints]{};
    size_t count = 0;
    uint16_t parts[kEndpoints]{};
    size_t part_count = 0;
    bool has_parts = false;
    bool pending = false;
    ElectricalEndpoint electrical[kEndpoints]{};
    size_t electrical_count = 0;
    bool electrical_truncated = false;
};
portMUX_TYPE mutex = portMUX_INITIALIZER_UNLOCKED;
Snapshot current;

void finish(uint32_t generation, State state, bool terminal = true)
{
    portENTER_CRITICAL(&mutex);
    if (current.generation == generation && current.pending) {
        if (current.state == State::Reading) current.state = state;
        if (terminal) current.pending = false;
    }
    portEXIT_CRITICAL(&mutex);
}

bool received_endpoint(uint16_t id)
{
    for (size_t i = 0; i < current.count; ++i) {
        if (current.endpoints[i].id == id) return true;
    }
    return false;
}

void complete(uint32_t generation)
{
    Metadata metadata;
    uint64_t node=0;
    portENTER_CRITICAL(&mutex);
    if(current.generation==generation && current.pending) {
        metadata=current.metadata;
        node=current.node;
    }
    if (current.generation == generation) current.pending = false;
    if (current.generation == generation && current.state == State::Reading) {
        bool complete = current.has_parts && received_endpoint(0);
        for (size_t i = 0; i < current.part_count; ++i) complete &= received_endpoint(current.parts[i]);
        current.state = complete ? State::Complete : State::Incomplete;
    }
    portEXIT_CRITICAL(&mutex);
    if(node && metadata.has_version)matter_ota::observed(node,metadata.version,metadata.version_string);
    // NVS work must stay outside the snapshot critical section.
    if(node && !quick_add_device_metadata(node,metadata.product,metadata.label,
                                         metadata.vid,metadata.pid,metadata.has_vid,metadata.has_pid))
        ESP_LOGW("matter_discovery","Could not persist reported metadata for node %u",(unsigned)node);
}

bool read_onoff_commands(chip::TLV::TLVReader &reader)
{
    if (reader.GetType() != chip::TLV::kTLVType_Array) return false;
    chip::TLV::TLVType container;
    if (reader.EnterContainer(container) != CHIP_NO_ERROR) return false;
    bool off = false, on = false;
    CHIP_ERROR status;
    while ((status = reader.Next()) == CHIP_NO_ERROR) {
        uint32_t command;
        if (reader.Get(command) != CHIP_NO_ERROR) return false;
        off |= command == 0;
        on |= command == 1;
    }
    return status == CHIP_END_OF_TLV && reader.ExitContainer(container) == CHIP_NO_ERROR && off && on;
}

void electrical_report(uint32_t generation, const chip::app::ConcreteDataAttributePath &path,
                       chip::TLV::TLVReader *data, const chip::app::StatusIB &status)
{
    size_t index = 0;
    while (index < kElectricalPathCount &&
           (kElectricalPaths[index].cluster != path.mClusterId ||
            kElectricalPaths[index].attribute != path.mAttributeId)) ++index;
    if (index == kElectricalPathCount || path.mEndpointId == 0xffff) return;
    Sample sample;
    sample.state = SampleState::Error;
    if (status.ToChipError() == CHIP_NO_ERROR && data) {
        if (data->GetType() == chip::TLV::kTLVType_Null) {
            sample.state = SampleState::Null;
        } else if (path.mClusterId == 0x0006) {
            bool value = false;
            if (path.mAttributeId == 0 ? data->Get(value) == CHIP_NO_ERROR : read_onoff_commands(*data)) {
                sample.value = value;
                sample.state = SampleState::Value;
            }
        } else if (path.mClusterId == 0x0405 && path.mAttributeId == 0) {
            // RelativeHumidityMeasurement.MeasuredValue is nullable uint16, not int64.
            uint16_t value;
            if (data->Get(value) == CHIP_NO_ERROR && value <= 10000) {
                sample.value = value;
                sample.state = SampleState::Value;
            }
        } else if (kElectricalPaths[index].cumulative) {
            if (read_energy(*data, sample.value)) sample.state = SampleState::Value;
        } else if (path.mAttributeId == 0xfffc || (path.mClusterId == kPowerMeasurement && path.mAttributeId == 0)) {
            uint32_t value;
            if (data->Get(value) == CHIP_NO_ERROR && (path.mAttributeId != 0 || value <= 2)) {
                sample.value = value;
                sample.state = SampleState::Value;
            }
        } else if (data->Get(sample.value) == CHIP_NO_ERROR &&
                   sample.value >= -kMeasurementLimit && sample.value <= kMeasurementLimit) {
            sample.state = SampleState::Value;
        }
    }

    portENTER_CRITICAL(&mutex);
    if (current.generation == generation && current.pending) {
        size_t endpoint = 0;
        while (endpoint < current.electrical_count && current.electrical[endpoint].id != path.mEndpointId) ++endpoint;
        if (endpoint == kEndpoints) {
            current.electrical_truncated = true;
        } else {
            if (endpoint == current.electrical_count) {
                current.electrical[endpoint] = {};
                current.electrical[endpoint].id = path.mEndpointId;
                ++current.electrical_count;
            }
            current.electrical[endpoint].samples[index] = sample;
        }
    }
    portEXIT_CRITICAL(&mutex);
}

void report(uint32_t generation, const chip::app::ConcreteDataAttributePath &path,
            chip::TLV::TLVReader *data, const chip::app::StatusIB &status)
{
    if(path.mEndpointId==0 && path.mClusterId==0x0028) {
        if(status.ToChipError()!=CHIP_NO_ERROR || !data)return;
        if(path.mAttributeId==9) {
            uint32_t version;if(data->Get(version)!=CHIP_NO_ERROR)return;
            portENTER_CRITICAL(&mutex);
            if(current.generation==generation && current.pending){current.metadata.version=version;current.metadata.has_version=true;}
            portEXIT_CRITICAL(&mutex);return;
        }
        char text[40]{};
        uint16_t id=0;
        const bool is_text=path.mAttributeId==3 || path.mAttributeId==5 || path.mAttributeId==10;
        if(is_text) {
            chip::CharSpan value;
            if(data->Get(value)!=CHIP_NO_ERROR || value.empty() || value.size()>=sizeof(text) ||
               memchr(value.data(),0,value.size()))return;
            memcpy(text,value.data(),value.size());
            if(!m2s::valid_name(text))return;
        } else if((path.mAttributeId!=2 && path.mAttributeId!=4) || data->Get(id)!=CHIP_NO_ERROR)return;
        portENTER_CRITICAL(&mutex);
        if(current.generation==generation && current.pending) {
            if(path.mAttributeId==3)memcpy(current.metadata.product,text,sizeof(text));
            if(path.mAttributeId==5)memcpy(current.metadata.label,text,sizeof(text));
            if(path.mAttributeId==10)memcpy(current.metadata.version_string,text,sizeof(text));
            if(path.mAttributeId==2){current.metadata.vid=id;current.metadata.has_vid=true;}
            if(path.mAttributeId==4){current.metadata.pid=id;current.metadata.has_pid=true;}
        }
        portEXIT_CRITICAL(&mutex);
        return;
    }
    if (path.mClusterId != kDescriptor) {
        electrical_report(generation, path, data, status);
        return;
    }
    const bool parts = path.mAttributeId == 3 && path.mEndpointId == 0;
    if (path.mClusterId != kDescriptor || (!parts && path.mAttributeId != 1)) return;
    if (status.ToChipError() != CHIP_NO_ERROR || !data ||
        data->GetType() != chip::TLV::kTLVType_Array) {
        finish(generation, State::Incomplete, false);
        return;
    }
    uint32_t values[kClusters]{};
    size_t count = 0;
    chip::TLV::TLVType outer;
    if (data->EnterContainer(outer) != CHIP_NO_ERROR) {
        finish(generation, State::Incomplete, false);
        return;
    }
    CHIP_ERROR next;
    while ((next = data->Next()) == CHIP_NO_ERROR) {
        if (count == (parts ? kEndpoints : kClusters)) {
            finish(generation, State::Limit, false);
            return;
        }
        if (data->Get(values[count]) != CHIP_NO_ERROR || (parts && values[count] >= 0xffff)) {
            finish(generation, State::Incomplete, false);
            return;
        }
        ++count;
    }
    if (next != CHIP_END_OF_TLV || data->ExitContainer(outer) != CHIP_NO_ERROR) {
        finish(generation, State::Incomplete, false);
        return;
    }

    portENTER_CRITICAL(&mutex);
    if (current.generation == generation && current.state == State::Reading) {
        if (parts) {
            current.has_parts = true;
            current.part_count = count;
            for (size_t i = 0; i < count; ++i) current.parts[i] = static_cast<uint16_t>(values[i]);
        } else if (received_endpoint(path.mEndpointId)) {
            current.state = State::Incomplete;
        } else if (current.count == kEndpoints) {
            current.state = State::Limit;
        } else {
            auto &endpoint = current.endpoints[current.count++];
            endpoint.id = path.mEndpointId;
            endpoint.count = count;
            for (size_t i = 0; i < count; ++i) endpoint.clusters[i] = values[i];
        }
    }
    portEXIT_CRITICAL(&mutex);
}

void expire()
{
    if (current.pending && esp_timer_get_time() - current.started >= kTimeoutUs) {
        current.pending = false;
        if (current.state == State::Reading) current.state = State::Failed;
    }
}

const char *state_name(State state)
{
    switch (state) {
    case State::Reading: return "reading";
    case State::Complete: return "complete";
    case State::Incomplete: return "incomplete";
    case State::Limit: return "capacity_exceeded";
    case State::Failed: return "failed";
    default: return "idle";
    }
}
}

esp_err_t start(uint64_t node)
{
    portENTER_CRITICAL(&mutex);
    expire();
    if (current.pending) {
        portEXIT_CRITICAL(&mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation = current.generation + 1;
    current.generation = generation;
    current.node = node;
    current.metadata = {};
    current.count = 0;
    current.part_count = 0;
    current.electrical_count = 0;
    current.electrical_truncated = false;
    current.has_parts = false;
    current.started = esp_timer_get_time();
    current.state = State::Reading;
    current.pending = true;
    portEXIT_CRITICAL(&mutex);

    chip::Platform::ScopedMemoryBufferWithSize<chip::app::AttributePathParams> paths;
    if (!paths.Alloc(8 + kElectricalPathCount)) {
        finish(generation, State::Failed);
        return ESP_ERR_NO_MEM;
    }
    paths[0] = chip::app::AttributePathParams(0xffff, kDescriptor, 1);
    paths[1] = chip::app::AttributePathParams(0, kDescriptor, 3);
    paths[2] = chip::app::AttributePathParams(0, 0x0028, 2); // VendorID
    paths[3] = chip::app::AttributePathParams(0, 0x0028, 3); // ProductName
    paths[4] = chip::app::AttributePathParams(0, 0x0028, 4); // ProductID
    paths[5] = chip::app::AttributePathParams(0, 0x0028, 5); // NodeLabel
    paths[6] = chip::app::AttributePathParams(0,0x0028,9);
    paths[7] = chip::app::AttributePathParams(0,0x0028,10);
    for (size_t i = 0; i < kElectricalPathCount; ++i) {
        paths[i + 8] = chip::app::AttributePathParams(0xffff, kElectricalPaths[i].cluster,
                                                     kElectricalPaths[i].attribute);
    }
    chip::Platform::ScopedMemoryBufferWithSize<chip::app::EventPathParams> events;
    auto *command = chip::Platform::New<esp_matter::controller::read_command>(
        node, std::move(paths), std::move(events),
        [generation](uint64_t, const auto &path, auto *data, const auto &status) {
            report(generation, path, data, status);
        },
        [generation](uint64_t, const auto &, const auto &) { complete(generation); },
        nullptr,
        [generation](void *, const auto &, CHIP_ERROR) { finish(generation, State::Failed); },
        [generation](uint64_t, CHIP_ERROR) { finish(generation, State::Failed); },
        true);
    if (!command) {
        finish(generation, State::Failed);
        return ESP_ERR_NO_MEM;
    }
    const esp_err_t result = command->send_command();
    if (result != ESP_OK) finish(generation, State::Failed);
    return result;
}

cJSON *snapshot_json()
{
    auto *snapshot = chip::Platform::New<Snapshot>();
    if (!snapshot) return nullptr;
    portENTER_CRITICAL(&mutex);
    expire();
    *snapshot = current;
    portEXIT_CRITICAL(&mutex);
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddNumberToObject(root, "node", static_cast<double>(snapshot->node));
        cJSON_AddNumberToObject(root, "generation", snapshot->generation);
        cJSON_AddStringToObject(root, "state", state_name(snapshot->state));
        auto *endpoints = cJSON_AddArrayToObject(root, "endpoints");
        bool ok = endpoints != nullptr;
        for (size_t i = 0; ok && i < snapshot->count; ++i) {
            auto *endpoint = cJSON_CreateObject();
            ok = endpoint && cJSON_AddItemToArray(endpoints, endpoint);
            if (!ok) {
                cJSON_Delete(endpoint);
                break;
            }
            const auto &source = snapshot->endpoints[i];
            auto *servers = cJSON_AddArrayToObject(endpoint, "servers");
            ok = servers && cJSON_AddNumberToObject(endpoint, "id", source.id);
            for (size_t j = 0; ok && j < source.count; ++j) {
                auto *cluster = cJSON_CreateNumber(source.clusters[j]);
                ok = cluster && cJSON_AddItemToArray(servers, cluster);
                if (!ok) cJSON_Delete(cluster);
            }
        }
        auto *electrical = cJSON_AddArrayToObject(root, "electrical");
        ok = ok && electrical && cJSON_AddBoolToObject(root, "electrical_truncated", snapshot->electrical_truncated);
        for (size_t i = 0; ok && i < snapshot->electrical_count; ++i) {
            const auto &source = snapshot->electrical[i];
            auto *entry = cJSON_CreateObject();
            if (!entry || !cJSON_AddItemToArray(electrical, entry)) {
                cJSON_Delete(entry);
                ok = false;
                break;
            }
            ok = cJSON_AddNumberToObject(entry, "endpoint", source.id) != nullptr;
            for (size_t j = 0; ok && j < kElectricalPathCount; ++j) {
                const auto &sample = source.samples[j];
                const char *key = kElectricalPaths[j].key;
                if (sample.state == SampleState::Missing) continue;
                if (sample.state == SampleState::Null) {
                    ok = cJSON_AddNullToObject(entry, key) != nullptr;
                } else if (sample.state == SampleState::Error) {
                    ok = cJSON_AddFalseToObject(entry, key) != nullptr;
                } else {
                    // cJSON numbers are doubles. Use exact decimal text for 64-bit readings.
                    char value[32];
                    snprintf(value, sizeof(value), "%lld", static_cast<long long>(sample.value));
                    ok = cJSON_AddStringToObject(entry, key, value) != nullptr;
                }
            }
        }
        if (!ok) {
            cJSON_Delete(root);
            root = nullptr;
        }
    }
    chip::Platform::Delete(snapshot);
    return root;
}
bool confirmation(uint64_t node, Confirmation &out)
{
    using m2s::mapping::Role;
    const auto bit = [](Role role) { return uint16_t(1u << static_cast<unsigned>(role)); };
    out = {};
    portENTER_CRITICAL(&mutex);
    const bool fresh = current.node == node && current.state == State::Complete && !current.pending &&
                       !current.electrical_truncated && esp_timer_get_time() - current.started < 120000000;
    if (fresh) {
        out.generation = current.generation;
        for (size_t i = 0; i < current.electrical_count; ++i) {
            const auto &source = current.electrical[i];
            if (!received_endpoint(source.id)) continue;
            const auto has = [&](size_t field) { return source.samples[field].state == SampleState::Value; };
            const auto value = [&](size_t field) { return source.samples[field].value; };
            uint16_t roles = 0;
            if (has(8) && has(9)) roles |= bit(Role::OnOff);
            if (has(10) && value(10) >= -27315 && value(10) <= 32767) roles |= bit(Role::Temperature);
            if (has(11) && value(11) >= 0 && value(11) <= 10000) roles |= bit(Role::Humidity);
            const bool single_phase_ac = has(0) && value(0) == 2 && has(4) && (value(4) & 6) == 2;
            if (single_phase_ac && has(5) && has(7) && (value(7) & 5) == 5) {
                roles |= bit(Role::ImportedEnergy);
                if (has(1) && value(1) >= 0 && value(1) <= 655359) roles |= bit(Role::Voltage);
                if (has(2) && value(2) >= 0 && value(2) <= 655349) roles |= bit(Role::Current);
                if (has(3) && value(3) >= -21474836470LL && value(3) <= 21474836470LL) roles |= bit(Role::ActivePower);
                if (has(6) && (value(7) & 6) == 6) roles |= bit(Role::ExportedEnergy);
            }
            if (roles) out.endpoints[out.count++] = {source.id, roles};
        }
    }
    portEXIT_CRITICAL(&mutex);
    return fresh;
}

}
