#pragma once
#include <cstdint>
#include <cJSON.h>
#include "mapping_core.h"

namespace m2s::proposal {
struct Thermohygrometer {
    uint16_t temperature_endpoint=mapping::kNone, humidity_endpoint=mapping::kNone;
    uint16_t channel=mapping::kNone; // automatic selection when not specified
};
struct ElectricityMeter {
    uint16_t relay_endpoint=mapping::kNone, meter_endpoint=mapping::kNone;
};
mapping::Error prepare(uint64_t node, cJSON **result, const Thermohygrometer *merge=nullptr,
                       const ElectricityMeter *meter=nullptr);
mapping::Error commit(const char *token);
}
