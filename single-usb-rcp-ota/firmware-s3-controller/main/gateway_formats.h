#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace gateway {
constexpr uint32_t kLayout=3,kNvsBytes=0x6000,kAppLimit=4*1024*1024;
constexpr uint32_t kRcpLayout=1,kRcpChipC6=13,kRcpAppLimit=580*1024;
#pragma pack(push,1)
struct BackupHeader {char magic[8];uint32_t version,bytes;uint8_t guid[16],mac[6],reserved[2],sha[32];};
struct UpdateHeader {char magic[8];uint32_t layout,chip,image_bytes,build;char version[32];uint8_t sha[32];uint16_t signature_bytes;uint8_t signature[80];};
#pragma pack(pop)
inline bool backup_header(const BackupHeader &h,size_t length){return !memcmp(h.magic,"M2SBK03",8)&&h.version==3&&h.bytes==kNvsBytes&&length==sizeof(h)+h.bytes;}
inline bool update_header(const UpdateHeader &h,uint32_t current,size_t length){return !memcmp(h.magic,"M2SOTA3",8)&&h.layout==kLayout&&h.chip==9&&h.image_bytes>0&&h.image_bytes<=kAppLimit&&h.build>current&&h.version[31]==0&&h.signature_bytes>=8&&h.signature_bytes<=80&&length==sizeof(h)+h.image_bytes;}
inline bool rcp_update_header(const UpdateHeader &h,size_t length){return !memcmp(h.magic,"M2SRCP1",8)&&h.layout==kRcpLayout&&h.chip==kRcpChipC6&&h.image_bytes>0&&h.image_bytes<=kRcpAppLimit&&h.build>0&&h.version[31]==0&&h.signature_bytes>=8&&h.signature_bytes<=80&&length==sizeof(h)+h.image_bytes;}
}
