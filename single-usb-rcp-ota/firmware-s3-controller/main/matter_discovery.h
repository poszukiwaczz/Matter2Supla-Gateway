#pragma once
#include <cstddef>
#include <cstdint>
#include <esp_err.h>
#include <cJSON.h>

namespace matter_discovery {
// A single bounded diagnostic snapshot, independent of SUPLA.
esp_err_t start(uint64_t node);
cJSON *snapshot_json();

struct ConfirmedEndpoint {
    uint16_t id = 0;
    uint16_t roles = 0;
};
struct Confirmation {
    static constexpr size_t capacity = 24;
    uint32_t generation = 0;
    size_t count = 0;
    ConfirmedEndpoint endpoints[capacity]{};
};
// Fresh, complete device reports only. Role bits follow mapping::Role.
bool confirmation(uint64_t node, Confirmation &out);
}
