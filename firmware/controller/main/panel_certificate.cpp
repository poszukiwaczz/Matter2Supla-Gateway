#include "gateway_services.h"
#include "panel_security.h"
#include "quick_add.h"
#include <nvs.h>
#include <cstring>
namespace gateway {
esp_err_t certificate_save(httpd_req_t *r){
 if(mutation_busy())return reply(r,409,"Bramka zajeta.");
 struct Guard{~Guard(){mutation_done();}} guard;
 if(quick_add_pairing_busy())return reply(r,409,"Poczekaj na zakonczenie parowania.");
 auto *j=body_json(r,4096);if(!j)return reply(r,400,"Niepoprawny certyfikat.");
 const auto *v=cJSON_GetObjectItemCaseSensitive(j,"certificate");
 bool valid=cJSON_IsString(v)&&panel_security_certificate_matches(v->valuestring,panel_security_private_key());
 if(!valid){cJSON_Delete(j);return reply(r,400,"Wymagany certyfikat serwera z SAN, zgodny z istniejacym kluczem bramki.");}
 nvs_handle_t h;esp_err_t e=nvs_open("panel_sec",NVS_READWRITE,&h);
 if(e==ESP_OK){e=nvs_set_str(h,"tls_cert_v2",v->valuestring);if(e==ESP_OK)e=nvs_commit(h);nvs_close(h);}
 cJSON_Delete(j);if(e!=ESP_OK)return reply(r,500,"Nie zapisano certyfikatu. Biezacy certyfikat bez zmian.");
 auto result=reply(r,200,"Certyfikat zapisany. Restart panelu; haslo, token i klucz prywatny pozostaja.");restart_later();return result;
}
}
