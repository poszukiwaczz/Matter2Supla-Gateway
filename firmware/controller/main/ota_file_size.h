#pragma once
#include <cstdint>
#include <limits>
namespace matter_ota {
inline bool decimal_file_size(const char *text,uint32_t &out) {
    if(!text || !*text)return false;
    uint32_t result=0;
    for(const char *p=text;*p;++p) {
        if(*p<'0' || *p>'9')return false;
        uint32_t digit=static_cast<uint32_t>(*p-'0');
        if(result>(std::numeric_limits<uint32_t>::max()-digit)/10)return false;
        result=result*10+digit;
    }
    out=result;return true;
}
}
