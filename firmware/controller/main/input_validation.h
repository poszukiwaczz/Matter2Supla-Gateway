#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace m2s {
inline bool valid_name(const char *s) {
    if (!s || !*s || strlen(s)>39) return false;
    const auto *p=reinterpret_cast<const uint8_t*>(s);
    while (*p) {
        uint32_t cp=*p++;
        size_t extra=0;
        uint32_t minimum=0;
        if (cp<0x80) { if (cp<0x20 || cp==0x7f) return false;
        continue; }
        if ((cp&0xe0)==0xc0) {cp&=0x1f;
        extra=1;
        minimum=0x80;}
        else if ((cp&0xf0)==0xe0) {cp&=0x0f;
        extra=2;
        minimum=0x800;}
        else if ((cp&0xf8)==0xf0) {cp&=0x07;
        extra=3;
        minimum=0x10000;}
        else return false;
        while(extra--) {if ((*p&0xc0)!=0x80) return false;
        cp=(cp<<6)|(*p++&0x3f);}
        if (cp<minimum || cp>0x10ffff || (cp>=0xd800&&cp<=0xdfff)) return false;
    }
    return true;
}
inline bool valid_wifi(const char *ssid,const char *pass) {
    if (!ssid || !*ssid || strlen(ssid)>32 || !pass) return false;
    const size_t n=strlen(pass);
    if (!n || (n>=8&&n<=63)) return true;
    if (n!=64) return false;
    for(size_t i=0;i<n;i++) if (!((pass[i]>='0'&&pass[i]<='9')||(pass[i]>='a'&&pass[i]<='f')||(pass[i]>='A'&&pass[i]<='F'))) return false;
    return true;
}
}
