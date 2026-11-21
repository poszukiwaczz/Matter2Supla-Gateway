#pragma once
#include "mapping_core.h"
namespace m2s { namespace mapping {
class NvsBackend final:public Backend {
public:
    Error read(Key key,uint8_t *out,size_t capacity,size_t &length) override;
    Error write(Key key,const uint8_t *bytes,size_t length) override;
};
class EspCrypto final:public Crypto {
public:
    bool sha256(const uint8_t *bytes,size_t length,uint8_t out[32]) override;
    bool random(uint8_t *out,size_t length) override;
};
// Stage B safety guard: the legacy bridge cannot interpret an active v2 map.
// No migration is triggered at boot; public prepare/commit integration comes later.
Error v2_presence(bool &present);
}}
