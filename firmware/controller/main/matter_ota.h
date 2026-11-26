#pragma once
#include <cstdint>
#include <esp_err.h>
#include <cJSON.h>
namespace matter_ota {
void init();
void forget(uint64_t node);
bool candidate_allowed(uint64_t node,uint32_t version);
void observed(uint64_t node,uint32_t version,const char *text);
esp_err_t check(uint64_t node);
esp_err_t install(uint64_t node,uint32_t version);
cJSON *snapshot(uint64_t node);
}
