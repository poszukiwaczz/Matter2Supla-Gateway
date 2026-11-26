#include "panel_security.h"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <initializer_list>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <mbedtls/pk.h>
#include <psa/crypto.h>
#include <mbedtls/x509_crt.h>


namespace {
struct SecurityRecord {
    uint32_t version;
    char token[65];
    char ap_password[17];
    char certificate[2048];
    char private_key[768];
};
SecurityRecord record{};
bool ready = false;
struct AccessRecord {
    uint32_t version;
    unsigned char salt[16];
    unsigned char digest[32];
};
struct AccessRecordV2 {
    uint32_t version;
    uint32_t iterations;
    unsigned char salt[16];
    unsigned char digest[32];
};
constexpr uint32_t kPbkdf2Iterations = 10000;
AccessRecord access_record{};
AccessRecordV2 access_record_v2{};
bool password_configured=false;
bool password_v2=false;
bool password_digest_v1(const char *password,const AccessRecord &access,unsigned char out[32]) {
    unsigned char input[80]{};
    size_t length=strlen(password),written=0;
    if(length>64)return false;
    memcpy(input,access.salt,16);
    memcpy(input+16,password,length);
    psa_status_t result=psa_hash_compute(PSA_ALG_SHA_256,input,16+length,out,32,&written);
    memset(input,0,sizeof(input));
    return result==PSA_SUCCESS && written==32;
}
bool hmac_sha256(const unsigned char key_block[64],const unsigned char *msg,size_t msg_len,unsigned char out[32]) {
    if(msg_len>36)return false;
    unsigned char inner[64+36]{},outer[64+32]{},inner_hash[32]{};
    for(size_t i=0;i<64;i++){inner[i]=key_block[i]^0x36;outer[i]=key_block[i]^0x5c;}
    memcpy(inner+64,msg,msg_len);
    size_t w1=0,w2=0;
    bool ok=psa_hash_compute(PSA_ALG_SHA_256,inner,64+msg_len,inner_hash,32,&w1)==PSA_SUCCESS && w1==32;
    if(ok){
        memcpy(outer+64,inner_hash,32);
        ok=psa_hash_compute(PSA_ALG_SHA_256,outer,96,out,32,&w2)==PSA_SUCCESS && w2==32;
    }
    memset(inner,0,sizeof(inner));memset(outer,0,sizeof(outer));memset(inner_hash,0,sizeof(inner_hash));
    return ok;
}
bool password_digest_pbkdf2(const char *password,const unsigned char salt[16],uint32_t iterations,unsigned char out[32]) {
    size_t length=strlen(password);
    if(length>64 || iterations<1000 || iterations>100000)return false;
    unsigned char key_block[64]{};
    memcpy(key_block,password,length);
    unsigned char salt_block[20]{};
    memcpy(salt_block,salt,16);
    salt_block[19]=1;
    unsigned char u[32]{},t[32]{};
    bool ok=hmac_sha256(key_block,salt_block,sizeof(salt_block),u);
    if(ok){
        memcpy(t,u,32);
        for(uint32_t i=1;i<iterations && ok;++i){
            ok=hmac_sha256(key_block,u,32,u);
            for(size_t j=0;j<32;++j)t[j]^=u[j];
        }
    }
    if(ok)memcpy(out,t,32);
    memset(key_block,0,sizeof(key_block));memset(u,0,sizeof(u));memset(t,0,sizeof(t));
    return ok;
}
void random_hex(char *out, size_t bytes) {
    static const char hex[] = "0123456789abcdef";
    uint8_t data[32];
    esp_fill_random(data, bytes);
    for (size_t i=0;i<bytes;i++) { out[2*i]=hex[data[i]>>4];
    out[2*i+1]=hex[data[i]&15]; }
    out[2*bytes]=0;
}
bool terminated(const char *p, size_t n) { return memchr(p,0,n)!=nullptr; }
esp_err_t generate_certificate() {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert cert;
    mbedtls_x509write_crt_init(&cert);
    psa_key_attributes_t attributes=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attributes,PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attributes,256);
    psa_set_key_usage_flags(&attributes,PSA_KEY_USAGE_EXPORT|PSA_KEY_USAGE_SIGN_HASH);
    psa_set_key_algorithm(&attributes,PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key_id=0;
    int rc=psa_generate_key(&attributes,&key_id);
    psa_reset_key_attributes(&attributes);
    if (!rc) rc=mbedtls_pk_copy_from_psa(key_id,&key);
    if (key_id) psa_destroy_key(key_id);
    if (!rc) rc=mbedtls_pk_write_key_pem(&key,(unsigned char*)record.private_key,sizeof(record.private_key));
    uint8_t serial[16];
    esp_fill_random(serial,sizeof(serial));
    serial[0]&=0x7f;
    serial[0]|=1;
    if (!rc) rc=mbedtls_x509write_crt_set_serial_raw(&cert,serial,sizeof(serial));
    mbedtls_x509write_crt_set_version(&cert,MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&cert,MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&cert,&key);
    mbedtls_x509write_crt_set_issuer_key(&cert,&key);
    const char *subject="CN=matter2supla.local,O=Matter2Supla";
    if (!rc) rc=mbedtls_x509write_crt_set_subject_name(&cert,subject);
    if (!rc) rc=mbedtls_x509write_crt_set_issuer_name(&cert,subject);
    if (!rc) rc=mbedtls_x509write_crt_set_validity(&cert,"20200101000000","20991231235959");
    if (!rc) rc=mbedtls_x509write_crt_set_basic_constraints(&cert,0,-1);
    if (!rc) rc=mbedtls_x509write_crt_set_key_usage(&cert,MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (!rc) rc=mbedtls_x509write_crt_pem(&cert,(unsigned char*)record.certificate,sizeof(record.certificate));
    mbedtls_x509write_crt_free(&cert);
    mbedtls_pk_free(&key);
    return rc==0 ? ESP_OK : ESP_FAIL;
}
bool header(httpd_req_t *req,const char *name,char *out,size_t n) {
    size_t len=httpd_req_get_hdr_value_len(req,name);
    return len>0 && len<n && httpd_req_get_hdr_value_str(req,name,out,n)==ESP_OK;
}
bool allowed_host(const char *host) {
    char hostname[80];
    snprintf(hostname,sizeof(hostname),"%s",host);
    char *colon=strchr(hostname,':');
    if (colon) { if (strcmp(colon,":443")!=0) return false;
    *colon=0; }
    if (strcasecmp(hostname,"matter2supla.local")==0) return true;
    for (const char *key : {"WIFI_STA_DEF","WIFI_AP_DEF"}) {
        esp_netif_t *netif=esp_netif_get_handle_from_ifkey(key);
        esp_netif_ip_info_t ip{};
        if (netif && esp_netif_get_ip_info(netif,&ip)==ESP_OK && ip.ip.addr) {
            char text[16];
            snprintf(text,sizeof(text),IPSTR,IP2STR(&ip.ip));
            if (strcmp(hostname,text)==0) return true;
        }
    }
    return false;
}
bool deny(httpd_req_t *req,const char *status,const char *message) {
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Connection","close");
    httpd_resp_sendstr(req,message);
    return false;
}
}
esp_err_t panel_security_init() {
    if (ready) return ESP_OK;
    if(psa_crypto_init()!=PSA_SUCCESS)return ESP_FAIL;
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("panel_sec",NVS_READWRITE,&nvs);
    if (err!=ESP_OK) return err;
    size_t size=sizeof(record);
    err=nvs_get_blob(nvs,"identity",&record,&size);
    if (err==ESP_ERR_NVS_NOT_FOUND) {
        record={};
        record.version=1;
        random_hex(record.token,32);
        random_hex(record.ap_password,8);
        err=generate_certificate();
        if (err==ESP_OK) err=nvs_set_blob(nvs,"identity",&record,sizeof(record));
        if (err==ESP_OK) err=nvs_commit(nvs);
    } else if (err==ESP_OK && (size!=sizeof(record) || record.version!=1 ||
        !terminated(record.token,sizeof(record.token)) || strlen(record.token)!=64 ||
        !terminated(record.ap_password,sizeof(record.ap_password)) || strlen(record.ap_password)!=16 ||
        !terminated(record.certificate,sizeof(record.certificate)) || !record.certificate[0] ||
        !terminated(record.private_key,sizeof(record.private_key)) || !record.private_key[0])) err=ESP_ERR_INVALID_STATE;
    nvs_close(nvs);
    if (err!=ESP_OK) return err;
    // Optional signed leaf certificate; token, password, key and base identity stay intact.
    err=nvs_open("panel_sec",NVS_READONLY,&nvs);
    if(err!=ESP_OK)return err;
    char signed_certificate[sizeof(record.certificate)]{};size_t signed_length=sizeof(signed_certificate);
    err=nvs_get_str(nvs,"tls_cert_v2",signed_certificate,&signed_length);nvs_close(nvs);
    if(err==ESP_OK){
        if(!panel_security_certificate_matches(signed_certificate,record.private_key))return ESP_ERR_INVALID_STATE;
        memcpy(record.certificate,signed_certificate,sizeof(record.certificate));
    }else if(err!=ESP_ERR_NVS_NOT_FOUND)return err;
    // Separate optional record; never overwrite the TLS/AP identity or legacy token.
    err=nvs_open("panel_sec",NVS_READONLY,&nvs);
    if(err!=ESP_OK)return err;
    size=sizeof(access_record_v2);
    err=nvs_get_blob(nvs,"access_v2",&access_record_v2,&size);
    if(err==ESP_OK){
        nvs_close(nvs);
        if(size!=sizeof(access_record_v2) || access_record_v2.version!=2 ||
           access_record_v2.iterations<1000 || access_record_v2.iterations>100000)return ESP_ERR_INVALID_STATE;
        password_configured=true;
        password_v2=true;
    } else if(err==ESP_ERR_NVS_NOT_FOUND){
        size=sizeof(access_record);
        err=nvs_get_blob(nvs,"access_v1",&access_record,&size);
        nvs_close(nvs);
        if(err==ESP_ERR_NVS_NOT_FOUND){password_configured=false;password_v2=false;}
        else if(err!=ESP_OK)return err;
        else {
            if(size!=sizeof(access_record) || access_record.version!=1)return ESP_ERR_INVALID_STATE;
            password_configured=true;
            password_v2=false;
        }
    } else {
        nvs_close(nvs);
        return err;
    }
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    if (mbedtls_x509_crt_parse(&crt,(const unsigned char*)record.certificate,strlen(record.certificate)+1)!=0) {
        mbedtls_x509_crt_free(&crt);
        return ESP_FAIL;
    }
    unsigned char digest[32];
    size_t digest_size=0;
    psa_status_t hash=psa_hash_compute(PSA_ALG_SHA_256,crt.raw.p,crt.raw.len,digest,sizeof(digest),&digest_size);
    mbedtls_x509_crt_free(&crt);
    if(hash!=PSA_SUCCESS||digest_size!=sizeof(digest))return ESP_FAIL;
    char fingerprint[96];
    for(size_t i=0;i<32;i++) snprintf(fingerprint+3*i,sizeof(fingerprint)-3*i,i==31?"%02X":"%02X:",digest[i]);
    // Physical console only: printf bypasses the HTTP ESP-log capture.
    printf("\nMatter2Supla HTTPS certificate SHA256: %s\nPanel access token: %s\nConfiguration AP: open, maximum 15 minutes\n",fingerprint,record.token);
    ready=true;
    return ESP_OK;
}
const char *panel_security_ap_password() { return ready ? record.ap_password : ""; }
const char *panel_security_certificate() { return ready ? record.certificate : ""; }
const char *panel_security_private_key() { return ready ? record.private_key : ""; }
bool panel_security_authorize(httpd_req_t *req,bool mutation,bool plain_body) {
    char host[80];
    if (!ready || !header(req,"Host",host,sizeof(host)) || !allowed_host(host))
        return deny(req,"403 Forbidden","{\"ok\":false,\"message\":\"Niedozwolony adres panelu.\"}");
    char origin[128];
    size_t origin_len=httpd_req_get_hdr_value_len(req,"Origin");
    if (origin_len) {
        char expected[96];
        snprintf(expected,sizeof(expected),"https://%s",host);
        if (!header(req,"Origin",origin,sizeof(origin)) || strcasecmp(origin,expected)!=0)
            return deny(req,"403 Forbidden","{\"ok\":false,\"message\":\"Niedozwolone pochodzenie zadania.\"}");
    }
    char authorization[96]{};
    if(!header(req,"Authorization",authorization,sizeof(authorization)) ||
       strncmp(authorization,"Bearer ",7)!=0)
        return deny(req,"401 Unauthorized","{\"ok\":false,\"message\":\"Wymagane haslo lub token panelu.\"}");
    struct IpLockout {
        bool in_use = false;
        uint8_t addr[16]{};
        uint32_t failed = 0;
        uint32_t level = 0;
        int64_t until_us = 0;
        int64_t last_fail_us = 0;
        int64_t last_seen_us = 0;
    };
    static DRAM_ATTR IpLockout s_ip_lockouts[8]{};
    static int64_t s_global_window_us = 0;
    static uint32_t s_global_failed = 0;
    static bool s_boot_key_ready = false;
    static unsigned char s_boot_key[16]{};
    static bool s_verified_cache_valid = false;
    static unsigned char s_verified_cache[32]{};
    static uint32_t s_verified_cache_gen = 0;
    static uint32_t s_password_gen = 1;
    const int64_t now_us = esp_timer_get_time();

    auto extract_peer_ip = [](httpd_req_t *r, uint8_t out[16]) {
        memset(out, 0, 16);
        int fd = httpd_req_to_sockfd(r);
        sockaddr_storage ss{};
        socklen_t len = sizeof(ss);
        if (fd >= 0 && getpeername(fd, reinterpret_cast<sockaddr *>(&ss), &len) == 0) {
            if (ss.ss_family == AF_INET) {
                const auto *in4 = reinterpret_cast<const sockaddr_in *>(&ss);
                out[10] = 0xff;
                out[11] = 0xff;
                memcpy(out + 12, &in4->sin_addr, 4);
            } else if (ss.ss_family == AF_INET6) {
                const auto *in6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
                memcpy(out, &in6->sin6_addr, 16);
            }
        }
    };
    auto find_peer_slot = [&](const uint8_t key[16], bool create) -> IpLockout * {
        IpLockout *free_slot = nullptr;
        IpLockout *oldest = &s_ip_lockouts[0];
        for (auto &slot : s_ip_lockouts) {
            if (slot.in_use && memcmp(slot.addr, key, 16) == 0) {
                slot.last_seen_us = now_us;
                return &slot;
            }
            if (!slot.in_use && !free_slot) free_slot = &slot;
            if (slot.last_seen_us < oldest->last_seen_us) oldest = &slot;
        }
        if (!create) return nullptr;
        IpLockout *dst = free_slot ? free_slot : oldest;
        *dst = {};
        dst->in_use = true;
        memcpy(dst->addr, key, 16);
        dst->last_seen_us = now_us;
        return dst;
    };

    const char *credential=authorization+7;
    // Console recovery token is checked BEFORE any lockout and never touches IP counters.
    bool via_token = false;
    if(strlen(credential)==64) {
        unsigned char diff=0;
        for(size_t i=0;i<64;i++)diff|=(unsigned char)(credential[i]^record.token[i]);
        via_token = (diff==0);
    }
    bool accepted = via_token;
    uint8_t peer_key[16]{};
    IpLockout *peer = nullptr;
    if (!via_token) {
        extract_peer_ip(req, peer_key);
        peer = find_peer_slot(peer_key, false);
        if (peer && peer->last_fail_us && (now_us - peer->last_fail_us) >= 15LL * 60 * 1000000) {
            peer->level = 0;
            peer->failed = 0;
        }
        if (peer && peer->until_us > now_us) {
            memset(authorization, 0, sizeof(authorization));
            return deny(req, "429 Too Many Requests", "{\"ok\":false,\"message\":\"Zbyt wiele blednych prob logowania z tego adresu. Odczekaj przed kolejna proba.\"}");
        }
    }
    if(!accepted && password_configured) {
        if(!s_boot_key_ready){esp_fill_random(s_boot_key,sizeof(s_boot_key));s_boot_key_ready=true;}
        unsigned char cache_digest[32]{};
        AccessRecord cache_ctx{};
        memcpy(cache_ctx.salt,s_boot_key,16);
        const bool have_cache_digest=password_digest_v1(credential,cache_ctx,cache_digest);
        if(have_cache_digest && s_verified_cache_valid && s_verified_cache_gen==s_password_gen) {
            unsigned char diff=0;
            for(size_t i=0;i<32;i++)diff|=cache_digest[i]^s_verified_cache[i];
            accepted=diff==0;
        }
        if(!accepted) {
            unsigned char digest[32]{},diff=0;
            if(password_v2) {
                if(password_digest_pbkdf2(credential,access_record_v2.salt,access_record_v2.iterations,digest)) {
                    for(size_t i=0;i<32;i++)diff|=digest[i]^access_record_v2.digest[i];
                    accepted=diff==0;
                }
            } else {
                if(password_digest_v1(credential,access_record,digest)) {
                    for(size_t i=0;i<32;i++)diff|=digest[i]^access_record.digest[i];
                    accepted=diff==0;
                    if(accepted) {
                        // Transparently migrate legacy SHA-256 password to PBKDF2-HMAC-SHA256.
                        AccessRecordV2 migrated{};
                        migrated.version=2;
                        migrated.iterations=kPbkdf2Iterations;
                        esp_fill_random(migrated.salt,sizeof(migrated.salt));
                        if(password_digest_pbkdf2(credential,migrated.salt,migrated.iterations,migrated.digest)) {
                            nvs_handle_t nvs;
                            if(nvs_open("panel_sec",NVS_READWRITE,&nvs)==ESP_OK) {
                                // access_v1 is kept for rollback to factory beta4 / 0.3.x — never erase.
                                if(nvs_set_blob(nvs,"access_v2",&migrated,sizeof(migrated))==ESP_OK && nvs_commit(nvs)==ESP_OK) {
                                    access_record_v2=migrated;
                                    password_v2=true;
                                }
                                nvs_close(nvs);
                            }
                        }
                    }
                }
            }
            memset(digest,0,sizeof(digest));
            if(accepted && have_cache_digest) {
                memcpy(s_verified_cache,cache_digest,32);
                s_verified_cache_gen=s_password_gen;
                s_verified_cache_valid=true;
            }
        }
        memset(cache_digest,0,sizeof(cache_digest));
    }
    // admin is a restricted first-setup credential, never general API access.
    if(!accepted && !password_configured && strcmp(credential,"admin")==0) {
        if(strcmp(req->uri,"/api/status")==0 || strcmp(req->uri,"/api/panel/password")==0)
            accepted=true;
        else return deny(req,"403 Forbidden","{\"ok\":false,\"message\":\"Najpierw ustaw wlasne haslo panelu.\"}");
    }
    memset(authorization,0,sizeof(authorization));
    if(!accepted) {
        if (!peer) peer = find_peer_slot(peer_key, true);
        if (peer) {
            if (peer->last_fail_us && (now_us - peer->last_fail_us) >= 15LL * 60 * 1000000) {
                peer->level = 0;
                peer->failed = 0;
            }
            peer->last_fail_us = now_us;
            if (++peer->failed >= 5) {
                static constexpr int64_t kLockoutUs[] = {5000000LL, 15000000LL, 45000000LL, 120000000LL, 300000000LL};
                const size_t idx = peer->level < 5 ? peer->level : 4;
                peer->until_us = now_us + kLockoutUs[idx];
                if (peer->level < 4) ++peer->level;
                peer->failed = 0;
            }
        }
        if (!s_global_window_us || (now_us - s_global_window_us) >= 60000000LL) {
            s_global_window_us = now_us;
            s_global_failed = 0;
        }
        if (++s_global_failed >= 30) {
            return deny(req, "429 Too Many Requests", "{\"ok\":false,\"message\":\"Zbyt wiele blednych prob logowania w sieci. Odczekaj chwile lub uzyj tokenu z konsoli.\"}");
        }
        return deny(req,"401 Unauthorized","{\"ok\":false,\"message\":\"Nieprawidlowe haslo lub token panelu.\"}");
    }
    if (!via_token && peer) {
        peer->failed = 0;
        peer->level = 0;
        peer->until_us = 0;
        peer->last_fail_us = 0;
    }
    if(strcmp(req->uri,"/api/panel/password")==0)++s_password_gen;
    if (mutation) {
        char type[80];
        // The binary MIME exception belongs only to authenticated OTA routes (S3 & C6 RCP).
        const bool ota_binary=strcmp(req->uri,"/api/gateway/update")==0 || strcmp(req->uri,"/api/gateway/update-rcp")==0;
        const char *expected=ota_binary?"application/octet-stream":(plain_body?"text/plain":"application/json");
        if (!header(req,"Content-Type",type,sizeof(type))) return deny(req,"415 Unsupported Media Type","{\"ok\":false,\"message\":\"Nieprawidlowy typ danych.\"}");
        char *semicolon=strchr(type,';');
        if (semicolon) *semicolon=0;
        if (strcasecmp(type,expected)!=0) return deny(req,"415 Unsupported Media Type","{\"ok\":false,\"message\":\"Nieprawidlowy typ danych.\"}");
    }
    return true;
}

bool panel_security_setup_required(httpd_req_t *req) {
    char authorization[96]{};
    bool setup=!password_configured && header(req,"Authorization",authorization,sizeof(authorization)) &&
        strcmp(authorization,"Bearer admin")==0;
    memset(authorization,0,sizeof(authorization));
    return setup;
}
// Called only on the HTTP server task, like authorize. Commit before publishing.
esp_err_t panel_security_change_password(const char *password) {
    if(!ready || !password)return ESP_ERR_INVALID_STATE;
    size_t length=strlen(password);
    if(length<8 || length>64)return ESP_ERR_INVALID_ARG;
    for(size_t i=0;i<length;i++)
        if((unsigned char)password[i]<33 || (unsigned char)password[i]>126)return ESP_ERR_INVALID_ARG;
    // Keep the console recovery token distinct from the configurable password.
    if(strcmp(password,record.token)==0)return ESP_ERR_INVALID_ARG;
    AccessRecordV2 next{};
    next.version=2;
    next.iterations=kPbkdf2Iterations;
    esp_fill_random(next.salt,sizeof(next.salt));
    if(!password_digest_pbkdf2(password,next.salt,next.iterations,next.digest))return ESP_FAIL;
    // access_v1 is kept for rollback to factory beta4 / 0.3.x — never erase.
    AccessRecord next_v1{};
    next_v1.version=1;
    esp_fill_random(next_v1.salt,sizeof(next_v1.salt));
    if(!password_digest_v1(password,next_v1,next_v1.digest))return ESP_FAIL;
    nvs_handle_t nvs;
    esp_err_t err=nvs_open("panel_sec",NVS_READWRITE,&nvs);
    if(err!=ESP_OK)return err;
    err=nvs_set_blob(nvs,"access_v2",&next,sizeof(next));
    if(err==ESP_OK)err=nvs_set_blob(nvs,"access_v1",&next_v1,sizeof(next_v1));
    if(err==ESP_OK)err=nvs_commit(nvs);
    nvs_close(nvs);
    if(err==ESP_OK){access_record_v2=next;access_record=next_v1;password_configured=true;password_v2=true;}
    return err;
}

bool panel_security_certificate_matches(const char *certificate,const char *private_key){
    if(!certificate||!private_key||strlen(certificate)>=2048)return false;
    mbedtls_x509_crt crt;mbedtls_x509_crt_init(&crt);mbedtls_pk_context key;mbedtls_pk_init(&key);
    bool ok=mbedtls_x509_crt_parse(&crt,(const unsigned char*)certificate,strlen(certificate)+1)==0 &&
        mbedtls_x509_crt_get_ca_istrue(&crt)==0 && mbedtls_x509_crt_has_ext_type(&crt,MBEDTLS_X509_EXT_SUBJECT_ALT_NAME) &&
        mbedtls_pk_parse_key(&key,(const unsigned char*)private_key,strlen(private_key)+1,nullptr,0)==0 &&
        mbedtls_pk_check_pair(&crt.pk,&key)==0;
    mbedtls_pk_free(&key);mbedtls_x509_crt_free(&crt);return ok;
}
