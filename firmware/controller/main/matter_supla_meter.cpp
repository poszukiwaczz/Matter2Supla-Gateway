#include "matter_supla_meter.h"

namespace m2s::electrical {
namespace {
constexpr unsigned kVoltage = 0;
constexpr unsigned kCurrent = 1;
constexpr unsigned kPower = 2;
constexpr unsigned kImported = 3;
constexpr unsigned kExported = 4;

bool representable(unsigned field, int64_t raw)
{
    switch (field) {
    case kVoltage: return raw >= 0 && raw <= INT64_C(655359);
    case kCurrent: return raw >= 0 && raw <= INT64_C(655349);
    case kPower: return raw >= -INT64_C(21474836470) && raw <= INT64_C(21474836470);
    case kImported:
    case kExported: return raw >= 0 && raw <= (INT64_C(1) << 62);
    default: return false;
    }
}
}

MatterSuplaMeter::MatterSuplaMeter()
{
    getChannel()->unsetFlag(SUPLA_CHANNEL_FLAG_CALCFG_RESET_COUNTERS);
    getChannel()->setStateOffline();
}

int MatterSuplaMeter::handleCalcfgFromServer(TSD_DeviceCalCfgRequest *)
{
    return SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
}

void MatterSuplaMeter::updateChannelValues()
{
    // The SDK also calls this on startup and on registration.
    if (published_mask_) Supla::Sensor::ElectricityMeter::updateChannelValues();
}

void MatterSuplaMeter::sample(mapping::Role role, bool valid, int64_t raw, uint32_t now)
{
    const unsigned index = static_cast<unsigned>(role) - static_cast<unsigned>(mapping::Role::Voltage);
    if (index >= 5) return;
    const bool usable = valid && representable(index, raw);
    auto &reading = readings_[index];
    dirty_ |= reading.valid != usable || (usable && reading.value != raw);
    reading = {raw, now, usable};
}

void MatterSuplaMeter::publish(uint32_t now, uint32_t max_age_ms, bool registered, bool reporting)
{
    uint8_t mask = 0;
    if (registered) {
        for (unsigned i = 0; i < 5; ++i) {
            const auto &r = readings_[i];
            if (r.valid && (reporting || uint32_t(now - r.received_ms) <= max_age_ms)) mask |= 1u << i;
        }
    }
    // SUPLA's basic meter value includes the imported counter. A missing
    // imported counter must not appear as a fresh measurement of 0 kWh.
    if (!(mask & (1u << kImported))) {
        if (published_mask_) getChannel()->setStateOffline();
        published_mask_ = 0;
        return;
    }
    if (!dirty_ && mask == published_mask_) return;

    // The SDK reset helper does not clear the advertised measurement mask.
    resetReadParameters();
    emValue.measured_values = 0;
    if (mask & (1u << kVoltage)) setVoltage(0, static_cast<uint16_t>(readings_[kVoltage].value / 10));
    if (mask & (1u << kCurrent)) setCurrent(0, static_cast<uint32_t>(readings_[kCurrent].value));
    if (mask & (1u << kPower)) setPowerActive(0, readings_[kPower].value * 100);
    setFwdActEnergy(0, static_cast<uint64_t>(readings_[kImported].value / 10));
    if (mask & (1u << kExported)) setRvrActEnergy(0, static_cast<uint64_t>(readings_[kExported].value / 10));

    // Force transmission of mask changes, including the disappearance of a reading.
    valueChanged = true;
    lastChannelUpdateTime = 0;
    published_mask_ = mask;
    updateChannelValues();
    getChannel()->setStateOnline();
    dirty_ = false;
}

}
