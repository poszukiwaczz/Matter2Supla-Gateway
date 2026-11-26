#include "mapping_nvs.h"
#include <nvs.h>
#include <esp_random.h>
#include <psa/crypto.h>

namespace m2s { namespace mapping {
namespace {
const char *name(Key key) {
    switch(key) {case Key::Legacy:return "map";case Key::Active:return "map_v2";case Key::BackupEven:return "backup0_v2";case Key::BackupOdd:return "backup1_v2";}
    return nullptr;
}
Error convert(esp_err_t err) {
    if(err==ESP_OK)return Error::Ok;
    if(err==ESP_ERR_NVS_NOT_FOUND)return Error::NotFound;
    if(err==ESP_ERR_NVS_NOT_ENOUGH_SPACE || err==ESP_ERR_NVS_NO_FREE_PAGES)return Error::NoSpace;
    if(err==ESP_ERR_NVS_INVALID_LENGTH)return Error::Corrupt;
    return Error::Io;
}
}
Error NvsBackend::read(Key key,uint8_t *out,size_t capacity,size_t &length) {
    length=0;if(!name(key) || !out)return Error::Invalid;
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("m2s_channels",NVS_READONLY,&nvs);
    if(err!=ESP_OK)return convert(err);
    size_t size=capacity;err=nvs_get_blob(nvs,name(key),out,&size);nvs_close(nvs);
    if(err==ESP_OK)length=size;
    return convert(err);
}
Error NvsBackend::write(Key key,const uint8_t *bytes,size_t length) {
    // This adapter never writes, erases or migrates the legacy blob or other namespaces.
    if((key!=Key::Active && key!=Key::BackupEven && key!=Key::BackupOdd) || !bytes || length<36 || length>kEncodedMax)return Error::Invalid;
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("m2s_channels",NVS_READWRITE,&nvs);if(err!=ESP_OK)return convert(err);
    err=nvs_set_blob(nvs,name(key),bytes,length);
    if(err==ESP_OK)err=nvs_commit(nvs);
    nvs_close(nvs);return convert(err);
}
bool EspCrypto::sha256(const uint8_t *bytes,size_t length,uint8_t out[32]) {
    size_t written=0;
    return psa_hash_compute(PSA_ALG_SHA_256,bytes,length,out,32,&written)==PSA_SUCCESS && written==32;
}
bool EspCrypto::random(uint8_t *out,size_t length) {
    if(!out || !length)return false;
    esp_fill_random(out,length);
    return true;
}
Error v2_presence(bool &present) {
    present=false;
    nvs_handle_t nvs;esp_err_t err=nvs_open("m2s_channels",NVS_READONLY,&nvs);
    if(err==ESP_ERR_NVS_NOT_FOUND)return Error::Ok;
    if(err!=ESP_OK)return convert(err);
    size_t length=0;err=nvs_get_blob(nvs,"map_v2",nullptr,&length);nvs_close(nvs);
    if(err==ESP_ERR_NVS_NOT_FOUND)return Error::Ok;
    if(err!=ESP_OK)return convert(err);
    present=true;return Error::Ok;
}
}}
