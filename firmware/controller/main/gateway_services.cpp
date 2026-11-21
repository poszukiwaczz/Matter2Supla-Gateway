#include "wifi_manager.h"
#include "gateway_services.h"
#include "version.h"
#include "quick_add.h"
#include "supla_bridge.h"
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_ipc.h>
#include <esp_flash.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include "app_image_size.h"
#include <cmath>
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/platform/radio.h>
#include <esp_log.h>
#include "radio_health.h"
#include <openthread/link.h>
#include <openthread/thread.h>
#include <openthread/dataset.h>
#include <openthread/dataset_updater.h>
#include <nvs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <memory>
#include <cstring>
extern "C" cJSON *supla_bridge_cloud_map();
namespace gateway {
namespace {
std::atomic_flag operation=ATOMIC_FLAG_INIT;portMUX_TYPE guard=portMUX_INITIALIZER_UNLOCKED;
struct Radio {
 int64_t tried=0,good=0;
 uint32_t failed=0,consecutive_failed=0,cca=0,cca_delta=0,consecutive_cca_blocked=0,tx=0,tx_delta=0,rx_errors=0;
 bool blocked=false,watchdog_alert=false;
 int result=-1;
 char version[256]{};
} radio;

// Option 2: Persistent RTC crash/panic tracker across warm reboots.
struct RtcCrashBreadcrumb {
 uint32_t magic;
 uint32_t boot_count;
 uint32_t panic_count;
 int32_t last_abnormal_reason;
 uint32_t last_uptime_before_reset_s;
 uint32_t current_uptime_s;
};
RTC_NOINIT_ATTR RtcCrashBreadcrumb s_rtc_crash;
constexpr uint32_t kCrashMagic = 0x4D325347U; // "M2SG"

void init_crash_breadcrumb() {
 const int reason = static_cast<int>(esp_reset_reason());
 const bool is_abnormal = (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT);
 if (s_rtc_crash.magic != kCrashMagic) {
  s_rtc_crash = {kCrashMagic, 1, 0, 0, 0, 0};
 } else {
  ++s_rtc_crash.boot_count;
  if (is_abnormal) {
   s_rtc_crash.last_uptime_before_reset_s = s_rtc_crash.current_uptime_s;
  }
 }
 if (is_abnormal) {
  ++s_rtc_crash.panic_count;
  s_rtc_crash.last_abnormal_reason = reason;
 }
 s_rtc_crash.current_uptime_s = 0;
}
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && !CONFIG_FREERTOS_SMP && CONFIG_ESP_IPC_ENABLE
constexpr int cpu_cores = CONFIG_FREERTOS_NUMBER_OF_CORES;
constexpr int64_t cpu_interval_ms = 2000;
struct CpuSample {
    int64_t measured_at_us = 0;
    double load_pct[cpu_cores]{};
};
CpuSample cpu_sample;

struct CpuCounter {
    int core = 0;
    int64_t time_us = 0;
    configRUN_TIME_COUNTER_TYPE idle_us = 0;
};

void read_cpu_counter(void *arg)
{
    auto &counter = *static_cast<CpuCounter *>(arg);
    // IPC switches out the idle task on this core, committing its current slice.
    counter.idle_us = ulTaskGetIdleRunTimeCounterForCore(counter.core);
    counter.time_us = esp_timer_get_time();
}

void sample_cpu(void *)
{
    CpuCounter previous[cpu_cores]{};
    for (;;) {
        CpuSample sample;
        bool ready = true;
        for (int core = 0; core < cpu_cores; ++core) {
            CpuCounter current;
            current.core = core;
            if (esp_ipc_call_blocking(core, read_cpu_counter, &current) != ESP_OK) {
                previous[core] = {};
                ready = false;
                continue;
            }

            const auto &last = previous[core];
            const auto elapsed = current.time_us - last.time_us;
            // FreeRTOS can skip the idle slice that crosses the 32-bit clock wrap.
            const bool clock_wrapped = (current.time_us >> 32) != (last.time_us >> 32);
            if (last.time_us && elapsed > 0 && !clock_wrapped) {
                const configRUN_TIME_COUNTER_TYPE idle = current.idle_us - last.idle_us;
                sample.load_pct[core] = std::fmax(0.0, 100.0 - 100.0 * idle / elapsed);
            } else {
                ready = false;
            }
            previous[core] = current;
        }
        if (ready) {
            sample.measured_at_us = esp_timer_get_time();
            portENTER_CRITICAL(&guard);
            cpu_sample = sample;
            portEXIT_CRITICAL(&guard);
        }
        vTaskDelay(pdMS_TO_TICKS(cpu_interval_ms));
    }
}

void add_cpu_diagnostics(cJSON *j, int64_t now)
{
    CpuSample sample;
    portENTER_CRITICAL(&guard);
    sample = cpu_sample;
    portEXIT_CRITICAL(&guard);

    const auto age = now - sample.measured_at_us;
    if (!sample.measured_at_us || age < 0 || age > 3 * cpu_interval_ms * 1000) return;

    auto *cpu = cJSON_AddObjectToObject(j, "cpu");
    auto *cores = cJSON_AddArrayToObject(cpu, "core_load_pct");
    double sum = 0;
    for (double load : sample.load_pct) {
        cJSON_AddItemToArray(cores, cJSON_CreateNumber(load));
        sum += load;
    }
    cJSON_AddNumberToObject(cpu, "load_pct", sum / cpu_cores);
}
#endif

std::atomic<TaskHandle_t> gateway_worker{nullptr};
void work(void*){gateway_worker.store(xTaskGetCurrentTaskHandle());int64_t next=0;for(;;){const auto now=esp_timer_get_time();
 s_rtc_crash.current_uptime_s = static_cast<uint32_t>(now / 1000000);
 if(now>=next){Radio r;portENTER_CRITICAL(&guard);r=radio;portEXIT_CRITICAL(&guard);r.tried=now;
  if(esp_openthread_get_instance()&&esp_openthread_lock_acquire(pdMS_TO_TICKS(20))){int8_t power=0;r.result=otPlatRadioGetTransmitPower(esp_openthread_get_instance(),&power);
   if(r.result==ESP_OK){r.good=esp_timer_get_time();r.consecutive_failed=0;const char *v=otPlatRadioGetVersionString(esp_openthread_get_instance());snprintf(r.version,sizeof(r.version),"%s",v?v:"");}
   else {++r.failed;++r.consecutive_failed;}
   auto *c=otLinkGetCounters(esp_openthread_get_instance());if(c){r.tx_delta=c->mTxTotal-r.tx;r.tx=c->mTxTotal;r.cca_delta=c->mTxErrCca-r.cca;r.cca=c->mTxErrCca;r.blocked=radio_blocked(r.tx_delta,r.cca_delta);if(r.blocked)++r.consecutive_cca_blocked;else r.consecutive_cca_blocked=0;r.rx_errors=c->mRxErrFcs+c->mRxErrSec;}
   esp_openthread_lock_release();}else r.result=ESP_ERR_TIMEOUT;
  r.watchdog_alert = (r.consecutive_cca_blocked >= 3 || r.consecutive_failed >= 3);
  static int64_t warning=0;if((r.blocked || r.watchdog_alert)&&(!warning||now-warning>=60000000)){ESP_LOGW("gateway_health","RF/C6 watchdog: TX window=%lu CCA window=%lu (consec_cca=%lu consec_fail=%lu); RCP query=%d.",(unsigned long)r.tx_delta,(unsigned long)r.cca_delta,(unsigned long)r.consecutive_cca_blocked,(unsigned long)r.consecutive_failed,r.result);warning=now;}
  portENTER_CRITICAL(&guard);radio=r;portEXIT_CRITICAL(&guard);next=now+15000000;}
 Radio r;portENTER_CRITICAL(&guard);r=radio;portEXIT_CRITICAL(&guard);ota_health(r.good&&now-r.good<45000000&&heap_caps_get_free_size(MALLOC_CAP_INTERNAL)>8192);
 mqtt_tick();ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2000));}}
}
void wake_gateway_worker()
{
    if (auto task = gateway_worker.load()) xTaskNotifyGive(task);
}
bool mutation_busy(){return operation.test_and_set();}void mutation_done(){operation.clear();}
esp_err_t reply(httpd_req_t *req,int status,const char *message){char line[48];snprintf(line,sizeof(line),"%d %s",status,status<400?"OK":"Error");httpd_resp_set_status(req,line);httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");auto *j=cJSON_CreateObject();if(!j){return ESP_ERR_NO_MEM;}
cJSON_AddBoolToObject(j,"ok",status<400);cJSON_AddStringToObject(j,"message",message);char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text){return ESP_ERR_NO_MEM;}
auto err=httpd_resp_sendstr(req,text);cJSON_free(text);return err;}
cJSON* body_json(httpd_req_t *req,size_t limit){if(req->content_len==0||req->content_len>limit){return nullptr;}
auto data=std::unique_ptr<char[]>(new(std::nothrow)char[req->content_len+1]);if(!data){return nullptr;}
size_t done=0;unsigned timeouts=0;while(done<req->content_len){int n=httpd_req_recv(req,data.get()+done,req->content_len-done);if(n==HTTPD_SOCK_ERR_TIMEOUT&&timeouts++<3)continue;if(n<=0){return nullptr;}
done+=n;}data[done]=0;return cJSON_ParseWithOpts(data.get(),nullptr,true);}
void restart_later(){
 if(xTaskCreate([](void*){vTaskDelay(pdMS_TO_TICKS(1800));esp_restart();},"gw_restart",2048,nullptr,2,nullptr)!=pdPASS){
  vTaskDelay(pdMS_TO_TICKS(600));
  esp_restart();
 }
}
void start()
{
    init_crash_breadcrumb();
    mqtt_init();
    xTaskCreate(work, "gateway_services", 12288, nullptr, 3, nullptr);
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && !CONFIG_FREERTOS_SMP && CONFIG_ESP_IPC_ENABLE
    if (xTaskCreate(sample_cpu, "gateway_cpu", 2048, nullptr, 2, nullptr) != pdPASS) {
        ESP_LOGW("gateway_health", "Could not start the CPU sampler");
    }
#endif
}
esp_err_t cloud_map(httpd_req_t *req){auto *j=supla_bridge_cloud_map();if(!j){return reply(req,409,"Brak aktywnej mapy kanalow.");}
char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text){return ESP_ERR_NO_MEM;}
httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");auto e=httpd_resp_sendstr(req,text);cJSON_free(text);return e;}
esp_err_t relay_control(httpd_req_t *req){
 auto *j=body_json(req,128);if(!j)return reply(req,400,"Niepoprawny JSON.");
 auto *node=cJSON_GetObjectItemCaseSensitive(j,"node"),*ep=cJSON_GetObjectItemCaseSensitive(j,"endpoint"),*on=cJSON_GetObjectItemCaseSensitive(j,"on");
 const bool valid=cJSON_IsNumber(node)&&std::isfinite(node->valuedouble)&&node->valuedouble>=1&&node->valuedouble<=9007199254740991.0&&std::floor(node->valuedouble)==node->valuedouble&&cJSON_IsNumber(ep)&&ep->valuedouble>=1&&ep->valuedouble<65535&&std::floor(ep->valuedouble)==ep->valuedouble&&cJSON_IsBool(on);
 if(!valid){cJSON_Delete(j);return reply(req,400,"Wymagane: poprawny node, endpoint i on typu boolean.");}
 const uint64_t id=(uint64_t)node->valuedouble;const uint16_t endpoint=(uint16_t)ep->valuedouble;const bool enabled=cJSON_IsTrue(on);cJSON_Delete(j);
 if(!quick_add_device_exists(id))return reply(req,404,"Nie ma takiego urzadzenia.");
 const auto e=relay_enqueue(id,endpoint,enabled);
 return reply(req,e==ESP_OK?202:e==ESP_ERR_NOT_SUPPORTED?409:e==ESP_ERR_INVALID_STATE?409:e==ESP_ERR_INVALID_ARG?400:503,e==ESP_OK?"Polecenie w kolejce. Zaczekaj na stan potwierdzony przez urzadzenie.":e==ESP_ERR_NOT_SUPPORTED?"Endpoint nie ma potwierdzonego OnOff.":e==ESP_ERR_INVALID_STATE?"Trwa parowanie. Sprobuj po jego zakonczeniu.":"Nie przyjeto polecenia. Sprobuj ponownie.");
}
namespace {
constexpr char name_namespace[] = "m2s_gateway";
constexpr char name_key[] = "label";
constexpr uint32_t channel_delay_ms = 300000;
std::atomic<int> channel_result{OT_ERROR_NONE};

struct ThreadLock {
    bool owned = esp_openthread_lock_acquire(pdMS_TO_TICKS(30));
    ~ThreadLock() { if (owned) esp_openthread_lock_release(); }
};

struct MutationLease {
    bool owned = !mutation_busy();
    ~MutationLease() { if (owned) mutation_done(); }
};

bool can_update_thread(otDeviceRole role)
{
    return role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
}

uint32_t available_channels(otInstance *instance, const otOperationalDataset &active)
{
    const auto radio_mask = otLinkGetSupportedChannelMask(instance);
    return active.mComponents.mIsChannelMaskPresent ? radio_mask & active.mChannelMask : radio_mask;
}

bool pending_dataset_exists(otInstance *instance, otOperationalDataset &pending)
{
    return otDatasetGetPending(instance, &pending) == OT_ERROR_NONE &&
           pending.mComponents.mIsPendingTimestampPresent;
}

void channel_updated(otError result, void *)
{
    channel_result.store(result);
}
}

void read_gateway_name(char (&name)[gateway_name_capacity])
{
    strcpy(name, "Matter Gateway");
    nvs_handle_t handle;
    if (nvs_open(name_namespace, NVS_READONLY, &handle) == ESP_OK) {
        size_t size = sizeof(name);
        if (nvs_get_str(handle, name_key, name, &size) != ESP_OK) {
            strcpy(name, "Matter Gateway");
        }
        nvs_close(handle);
    }
}

void append_name(cJSON *system)
{
    char name[gateway_name_capacity];
    read_gateway_name(name);
    cJSON_AddStringToObject(system, "gateway_name", name);
}

esp_err_t settings_get(httpd_req_t *req)
{
    auto *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    append_name(root);
    int channel = 0, pending_channel = 0;
    int64_t remaining_ms = -1;
    bool can_change = false, updating = false;
    uint32_t mask = 0;
    if (auto *instance = esp_openthread_get_instance()) {
        ThreadLock lock;
        if (lock.owned) {
            otOperationalDataset active{}, pending{};
            if (otDatasetGetActive(instance, &active) == OT_ERROR_NONE && active.mComponents.mIsChannelPresent) {
                channel = active.mChannel;
                mask = available_channels(instance, active);
                can_change = can_update_thread(otThreadGetDeviceRole(instance));
            }
            updating = otDatasetUpdaterIsUpdateOngoing(instance);
            if (pending_dataset_exists(instance, pending)) {
                updating = true;
                if (pending.mComponents.mIsChannelPresent && pending.mChannel != channel) {
                    pending_channel = pending.mChannel;
                    if (pending.mComponents.mIsDelayPresent) remaining_ms = pending.mDelay;
                }
            }
        }
    }
    if (auto *thread = cJSON_AddObjectToObject(root, "thread")) {
        if (channel) cJSON_AddNumberToObject(thread, "channel", channel);
        if (pending_channel) cJSON_AddNumberToObject(thread, "pending_channel", pending_channel);
        if (remaining_ms >= 0) cJSON_AddNumberToObject(thread, "remaining_s", (remaining_ms + 999) / 1000);
        cJSON_AddBoolToObject(thread, "can_change", can_change);
        cJSON_AddBoolToObject(thread, "updating", updating);
        cJSON_AddNumberToObject(thread, "last_error", channel_result.load());
        if (auto *options = cJSON_AddArrayToObject(thread, "channels")) {
            for (int ch = 11; ch <= 26; ++ch) {
                if (mask & (uint32_t{1} << ch)) cJSON_AddItemToArray(options, cJSON_CreateNumber(ch));
            }
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const auto status = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return status;
}

esp_err_t name_save(httpd_req_t *req)
{
    auto *body = body_json(req, 384);
    const auto *name = cJSON_GetObjectItemCaseSensitive(body, "name");
    const char *value = cJSON_IsString(name) ? name->valuestring : "";
    const auto size = strlen(value);
    bool valid = size > 0 && size < gateway_name_capacity && value[0] != ' ' && value[size - 1] != ' ';
    for (size_t i = 0; i < size && valid; ++i) {
        const auto ch = static_cast<unsigned char>(value[i]);
        valid = ch >= 0x20 && ch != 0x7f;
    }
    if (!valid) {
        cJSON_Delete(body);
        return reply(req, 400, "Nazwa bramki: od 1 do 48 bajtów UTF-8, bez znaków sterujących.");
    }
    MutationLease lease;
    if (!lease.owned) {
        cJSON_Delete(body);
        return reply(req, 409, "Bramka jest zajęta.");
    }
    nvs_handle_t handle;
    auto result = nvs_open(name_namespace, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, name_key, value);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    cJSON_Delete(body);
    if (result != ESP_OK) return reply(req, 503, "Nie zapisano nazwy w pamięci bramki.");
    supla_bridge_request_name_reload();
    return reply(req, 200, "Zapisano nazwę. Bramka przekaże ją przy ponownym połączeniu z SUPLĄ.");
}

esp_err_t thread_channel_save(httpd_req_t *req)
{
    auto *body = body_json(req, 192);
    const auto *number = cJSON_GetObjectItemCaseSensitive(body, "channel");
    const auto *confirmed = cJSON_GetObjectItemCaseSensitive(body, "confirm");
    const double value = cJSON_IsNumber(number) ? number->valuedouble : 0;
    const bool valid = value >= 11 && value <= 26 && value == std::floor(value) && cJSON_IsTrue(confirmed);
    cJSON_Delete(body);
    if (!valid) return reply(req, 400, "Wybierz i potwierdź kanał Thread od 11 do 26.");

    MutationLease lease;
    if (!lease.owned || quick_add_pairing_busy()) return reply(req, 409, "Bramka jest zajęta.");
    auto *instance = esp_openthread_get_instance();
    if (!instance) return reply(req, 503, "Radio Thread jest niedostępne.");

    int status = 409;
    const char *message = "Sieć Thread nie jest gotowa do zmiany kanału.";
    {
        ThreadLock lock;
        otOperationalDataset active{}, pending{};
        if (lock.owned && can_update_thread(otThreadGetDeviceRole(instance)) &&
            otDatasetGetActive(instance, &active) == OT_ERROR_NONE && active.mComponents.mIsChannelPresent) {
            const auto channel = static_cast<uint16_t>(value);
            if (!(available_channels(instance, active) & (uint32_t{1} << channel))) {
                status = 400;
                message = "Kanał jest poza maską sieci lub możliwościami radia.";
            } else if (otDatasetUpdaterIsUpdateOngoing(instance) || pending_dataset_exists(instance, pending)) {
                message = "Sieć Thread ma już zaplanowaną zmianę parametrów.";
            } else if (channel == active.mChannel) {
                status = 200;
                message = "Ten kanał jest już aktywny.";
            } else {
                otOperationalDataset change{};
                change.mComponents.mIsChannelPresent = true;
                change.mComponents.mIsDelayPresent = true;
                change.mChannel = channel;
                change.mDelay = channel_delay_ms;
                const auto result = otDatasetUpdaterRequestUpdate(instance, &change, channel_updated, nullptr);
                channel_result.store(result);
                if (result == OT_ERROR_NONE) {
                    status = 202;
                    message = "Zlecono zmianę kanału sieci Thread z opóźnieniem 5 minut.";
                } else {
                    message = "OpenThread nie przyjął zmiany kanału.";
                }
            }
        }
    }
    return reply(req, status, message);
}

esp_err_t diagnostics(httpd_req_t *req){ const size_t ram_total=heap_caps_get_total_size(MALLOC_CAP_INTERNAL),ram_free=heap_caps_get_free_size(MALLOC_CAP_INTERNAL),psram_total=heap_caps_get_total_size(MALLOC_CAP_SPIRAM),psram_free=heap_caps_get_free_size(MALLOC_CAP_SPIRAM);auto *j=cJSON_CreateObject();if(!j){return ESP_ERR_NO_MEM;}
Radio r;portENTER_CRITICAL(&guard);r=radio;portEXIT_CRITICAL(&guard);const auto now=esp_timer_get_time();
 cJSON_AddStringToObject(j,"firmware",FW_VERSION);cJSON_AddNumberToObject(j,"uptime_s",now/1000000);cJSON_AddNumberToObject(j,"reset_reason",esp_reset_reason());
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && !CONFIG_FREERTOS_SMP && CONFIG_ESP_IPC_ENABLE
 add_cpu_diagnostics(j,esp_timer_get_time());
#endif
 auto *mem=cJSON_AddObjectToObject(j,"memory");cJSON_AddNumberToObject(mem,"internal_free",ram_free);cJSON_AddNumberToObject(mem,"internal_minimum",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));cJSON_AddNumberToObject(mem,"internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));cJSON_AddNumberToObject(mem,"psram_free",psram_free);

 cJSON_AddNumberToObject(mem,"internal_total",ram_total);cJSON_AddNumberToObject(mem,"internal_used",ram_total>=ram_free?ram_total-ram_free:0);cJSON_AddNumberToObject(mem,"psram_total",psram_total);cJSON_AddNumberToObject(mem,"psram_used",psram_total>=psram_free?psram_total-psram_free:0);
 struct Flash {uint32_t physical=0,assigned=0,unassigned=0,app_size=0,app_image=0;const char *label="";bool image_known=false;};
 static const Flash flash=[](){Flash f;if(esp_flash_get_size(nullptr,&f.physical)!=ESP_OK)return f;uint32_t first=f.physical;auto it=esp_partition_find(ESP_PARTITION_TYPE_ANY,ESP_PARTITION_SUBTYPE_ANY,nullptr);while(it){const auto *p=esp_partition_get(it);f.assigned+=p->size;if(p->address<first)first=p->address;it=esp_partition_next(it);}f.assigned+=first;f.unassigned=f.assigned<=f.physical?f.physical-f.assigned:0;const auto *app=esp_ota_get_running_partition();if(app){f.app_size=app->size;f.label=app->label;f.image_known=app_image_size(app->size,[app](uint32_t offset,void *out,size_t length){return esp_partition_read(app,offset,out,length)==ESP_OK;},f.app_image);}return f;}();
 auto *storage=cJSON_AddObjectToObject(j,"flash");if(flash.physical)cJSON_AddNumberToObject(storage,"physical_bytes",flash.physical);else cJSON_AddNullToObject(storage,"physical_bytes");if(flash.physical)cJSON_AddNumberToObject(storage,"unassigned_bytes",flash.unassigned);else cJSON_AddNullToObject(storage,"unassigned_bytes");cJSON_AddStringToObject(storage,"running_partition",flash.label);cJSON_AddNumberToObject(storage,"app_partition_bytes",flash.app_size);if(flash.image_known){cJSON_AddNumberToObject(storage,"app_image_bytes",flash.app_image);cJSON_AddNumberToObject(storage,"app_partition_unused_bytes",flash.app_size-flash.app_image);}else{cJSON_AddNullToObject(storage,"app_image_bytes");cJSON_AddNullToObject(storage,"app_partition_unused_bytes");}

 auto *rcp=cJSON_AddObjectToObject(j,"s3_c6");cJSON_AddStringToObject(rcp,"transport","UART1 2000000 8N1 over USB connector pins");cJSON_AddStringToObject(rcp,"probe","Spinel GET PHY_TX_POWER response");cJSON_AddBoolToObject(rcp,"connected",r.result==ESP_OK&&r.good&&now-r.good<45000000);if(r.good)cJSON_AddNumberToObject(rcp,"last_success_age_s",(now-r.good)/1000000);else cJSON_AddNullToObject(rcp,"last_success_age_s");cJSON_AddStringToObject(rcp,"rcp_version",r.version);cJSON_AddNumberToObject(rcp,"probe_failures",r.failed);cJSON_AddNumberToObject(rcp,"consecutive_probe_failures",r.consecutive_failed);cJSON_AddNumberToObject(rcp,"last_result",r.result);cJSON_AddBoolToObject(rcp,"radio_access_blocked",r.blocked);cJSON_AddNumberToObject(rcp,"consecutive_cca_blocked_windows",r.consecutive_cca_blocked);cJSON_AddBoolToObject(rcp,"watchdog_alert",r.watchdog_alert);cJSON_AddNumberToObject(rcp,"radio_tx_total",r.tx);cJSON_AddNumberToObject(rcp,"radio_tx_since_last_probe",r.tx_delta);cJSON_AddNumberToObject(rcp,"radio_cca_total",r.cca);cJSON_AddNumberToObject(rcp,"radio_cca_since_last_probe",r.cca_delta);cJSON_AddNumberToObject(rcp,"radio_rx_errors_total",r.rx_errors);
 if(auto *cg=cJSON_AddObjectToObject(j,"crash_guard")){
  cJSON_AddNumberToObject(cg,"boot_count",s_rtc_crash.boot_count);
  cJSON_AddNumberToObject(cg,"panic_count",s_rtc_crash.panic_count);
  cJSON_AddNumberToObject(cg,"last_abnormal_reason",s_rtc_crash.last_abnormal_reason);
  cJSON_AddNumberToObject(cg,"last_uptime_before_reset_s",s_rtc_crash.last_uptime_before_reset_s);
 }
 wifi_health_t wifi{};wifi_manager_health(&wifi);
 if(auto *network=cJSON_AddObjectToObject(j,"wifi_health")){
  cJSON_AddNumberToObject(network,"disconnects",wifi.disconnects);
  cJSON_AddNumberToObject(network,"ip_losses",wifi.ip_losses);
  cJSON_AddNumberToObject(network,"last_disconnect_reason",wifi.last_disconnect_reason);
  cJSON_AddNumberToObject(network,"reconnects",wifi.reconnects);
  cJSON_AddNumberToObject(network,"recoveries",wifi.recoveries);
  cJSON_AddNumberToObject(network,"gateway_misses",wifi.gateway_misses);
  cJSON_AddBoolToObject(network,"gateway_probe_active",wifi.gateway_probe_active);
  cJSON_AddBoolToObject(network,"gateway_seen_reply",wifi.gateway_seen_reply);
 }
 cJSON_AddStringToObject(j,"radio_note","Cumulative RF counters are separate from UART probe failures; no sensor polling is performed by this endpoint.");char *text=cJSON_PrintUnformatted(j);cJSON_Delete(j);if(!text){return ESP_ERR_NO_MEM;}
httpd_resp_set_type(req,"application/json");httpd_resp_set_hdr(req,"Cache-Control","no-store");auto e=httpd_resp_sendstr(req,text);cJSON_free(text);return e;}
}
