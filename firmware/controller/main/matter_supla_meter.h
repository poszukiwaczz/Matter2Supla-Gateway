#pragma once

#include "mapping_core.h"
#include <supla/sensor/electricity_meter.h>

namespace m2s::electrical {

class MatterSuplaMeter final : public Supla::Sensor::ElectricityMeter {
public:
    MatterSuplaMeter();
    void sample(mapping::Role role, bool valid, int64_t raw, uint32_t now);
    void publish(uint32_t now, uint32_t max_age_ms, bool registered, bool reporting=false);
    void readValuesFromDevice() override {}
    void updateChannelValues() override;
    int handleCalcfgFromServer(TSD_DeviceCalCfgRequest *request) override;

private:
    struct Reading {
        int64_t value = 0;
        uint32_t received_ms = 0;
        bool valid = false;
    };
    Reading readings_[5]{};
    bool dirty_ = false;
    uint8_t published_mask_ = 0;
};

}
