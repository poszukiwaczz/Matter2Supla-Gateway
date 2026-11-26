#pragma once
#include <cstdint>
#include <lib/core/TLVReader.h>

namespace m2s::electrical {
constexpr int64_t kMeasurementLimit = INT64_C(1) << 62;

// EnergyMeasurementStruct: field 0 is cumulative energy in mWh.
inline bool read_energy(chip::TLV::TLVReader &reader, int64_t &value)
{
    if (reader.GetType() != chip::TLV::kTLVType_Structure) return false;
    chip::TLV::TLVType container;
    if (reader.EnterContainer(container) != CHIP_NO_ERROR) return false;
    bool found = false;
    CHIP_ERROR status;
    while ((status = reader.Next()) == CHIP_NO_ERROR) {
        if (reader.GetTag() != chip::TLV::ContextTag(0)) continue;
        if (found || reader.Get(value) != CHIP_NO_ERROR) return false;
        found = true;
    }
    return found && status == CHIP_END_OF_TLV && reader.ExitContainer(container) == CHIP_NO_ERROR &&
           value >= 0 && value <= kMeasurementLimit;
}

}
