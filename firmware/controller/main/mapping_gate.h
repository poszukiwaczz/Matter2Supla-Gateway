#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace m2s::mapping {
// Serializes the legacy allocator with prepare/commit on the HTTP task.
inline SemaphoreHandle_t map_mutex()
{
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&storage);
    return mutex;
}
class MapLock {
public:
    MapLock() { xSemaphoreTake(map_mutex(), portMAX_DELAY); }
    ~MapLock() { xSemaphoreGive(map_mutex()); }
    MapLock(const MapLock &) = delete;
    MapLock &operator=(const MapLock &) = delete;
};
}
