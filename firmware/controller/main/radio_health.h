#pragma once
#include <cstdint>
namespace gateway {
// A completed counter window, not evidence of RF interference or a broken radio.
// No autonomous reset, CCA threshold adjustment, or Thread channel migration.
inline bool radio_blocked(uint32_t tx,uint32_t cca){return tx>=5 && cca==tx;}
}
