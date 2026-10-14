#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include "application_name.h"
#include "esp_log.h"
#include "esp_console.h"
#include "../cmd_system.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "pthread.h"
#include "platform_esp32.h"
#include "platform_config.h"
#include "nvs_utilities.h"
#include "esp_app_format.h"
#include "tools.h"
#include "messaging.h"
#include "adac.h"

extern esp_err_t process_recovery_ota(const char * bin_url, char * bin_buffer, uint32_t length);
static const char * TAG = "squeezelite_cmd";
#define SQUEEZELITE_THREAD_STACK_SIZE (8*1024)

const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {

    .magic_word = ESP_APP_DESC_MAGIC_WORD,
    .version = PROJECT_VER,
    .project_name = CONFIG_PROJECT_NAME,
    .idf_ver = IDF_VER,

#ifdef CONFIG_BOOTLOADER_APP_SECURE_VERSION
    .secure_version = CONFIG_BOOTLOADER_APP_SECURE_VERSION,
#else
    .secure_version = 0,
#endif

#ifdef CONFIG_APP_COMPILE_TIME_DATE
    .time = __TIME__,
    .date = __DATE__,
#else
    .time = "",
    .date = "",
#endif
};

extern void register_audio_config(void);
extern void register_rotary_config(void);
extern void register_ledvu_config(void);
extern void register_nvs();

void register_optional_cmd(void) {
    #if CONFIG_WITH_CONFIG_UI	
    register_rotary_config();
#endif
    register_audio_config();
	register_ledvu_config();
	register_nvs();
}    

extern int squeezelite_main(int argc, char **argv);

static int launchsqueezelite(int argc, char **argv);

/** Arguments used by 'squeezelite' function */
static struct {
    struct arg_str *parameters;
    struct arg_end *end;
} squeezelite_args;
static struct {
	int argc;
	char ** argv;
} thread_parms ;

#define ADDITIONAL_SQUEEZELITE_ARGS 5
static void squeezelite_thread(void *arg){  
	ESP_LOGV(TAG ,"Number of args received: %u",thread_parms.argc );
	ESP_LOGV(TAG ,"Values:");
    for(int i = 0;i<thread_parms.argc; i++){
    	ESP_LOGV(TAG ,"     %s",thread_parms.argv[i]);
    }
    ESP_LOGI(TAG ,"Calling squeezelite");
    int ret = squeezelite_main(thread_parms.argc, thread_parms.argv);
        
    cmd_send_messaging("cfg-audio-tmpl",ret > 1 ?  MESSAGING_ERROR : MESSAGING_WARNING,"squeezelite exited with error code %d\n", ret);

    if (ret <= 1) {
        int wait = 60;
        wait_for_commit();
        cmd_send_messaging("cfg-audio-tmpl",MESSAGING_ERROR,"Rebooting in %d sec\n", wait);
        vTaskDelay( pdMS_TO_TICKS(wait * 1000));
        ESP_LOGW(TAG, "Deliberate Squeezelite restart; resetting boot-loop counter");
        halSTORAGE_RebootCounterUpdate(0);
        esp_restart();
    } else {
		cmd_send_messaging("cfg-audio-tmpl",MESSAGING_ERROR,"Correct command line and reboot\n");
        vTaskSuspend(NULL);
    }

	ESP_LOGV(TAG, "Exited from squeezelite's main(). Freeing argv structure.");

	for(int i=0;i<thread_parms.argc;i++) free(thread_parms.argv[i]);
	free(thread_parms.argv);
}

static int launchsqueezelite(int argc, char **argv) {
	static DRAM_ATTR StaticTask_t xTaskBuffer __attribute__ ((aligned (4)));
	static EXT_RAM_ATTR StackType_t xStack[SQUEEZELITE_THREAD_STACK_SIZE] __attribute__ ((aligned (4)));
	static bool isRunning = false;

	if (isRunning) {
		ESP_LOGE(TAG,"Squeezelite already running. Exiting!");
		return -1;
	}
	
	ESP_LOGV(TAG ,"Begin");
	isRunning = true;

	ESP_LOGV(TAG, "Parameters:");
    for(int i = 0;i<argc; i++){
    	ESP_LOGV(TAG, "     %s",argv[i]);
    }
    ESP_LOGV(TAG,"Saving args in thread structure");

    thread_parms.argc=0;
    thread_parms.argv = malloc_init_external(sizeof(char**)*(argc+ADDITIONAL_SQUEEZELITE_ARGS));

	for(int i=0;i<argc;i++){
		ESP_LOGD(TAG ,"assigning parm %u : %s",i,argv[i]);
		thread_parms.argv[thread_parms.argc++]=strdup(argv[i]);
	}

	if(argc==1){
		// There isn't a default configuration that would actually work
		// if no parameter is passed.
		ESP_LOGV(TAG ,"Adding argv value at %u. Prev value: %s",thread_parms.argc,thread_parms.argv[thread_parms.argc-1]);
		thread_parms.argv[thread_parms.argc++]=strdup("-?");
	}

	ESP_LOGD(TAG,"Starting Squeezelite Thread");
	xTaskCreateStaticPinnedToCore(squeezelite_thread, "squeezelite", SQUEEZELITE_THREAD_STACK_SIZE, 
					  NULL, CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT, xStack, &xTaskBuffer, CONFIG_PTHREAD_TASK_CORE_DEFAULT);
	ESP_LOGD(TAG ,"Back to console thread!");

    return 0;
}

static struct {
    struct arg_int *sda;
    struct arg_int *scl;
    struct arg_int *addr;
    struct arg_end *end;
} dac_probe_args = { 0 };

static int parse_cfg_int(const char *cfg, const char *key, int dflt) {
    const char *p = cfg ? strcasestr(cfg, key) : NULL;
    if (p) {
        p = strchr(p, '=');
        if (p) return atoi(p + 1);
    }
    return dflt;
}

static int perform_dac_probe(int argc, char **argv) {
    int nerrors = arg_parse(argc, argv, (void **)&dac_probe_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, dac_probe_args.end, argv[0]);
        return 1;
    }
    char *cfg = config_alloc_get(NVS_TYPE_STR, "dac_config");
    int cfg_sda = parse_cfg_int(cfg, "sda", -1);
    int cfg_scl = parse_cfg_int(cfg, "scl", -1);
    int cfg_addr = parse_cfg_int(cfg, "i2c", 0x10);
    int sda = dac_probe_args.sda->count ? dac_probe_args.sda->ival[0] : cfg_sda;
    int scl = dac_probe_args.scl->count ? dac_probe_args.scl->ival[0] : cfg_scl;
    int addr = dac_probe_args.addr->count ? dac_probe_args.addr->ival[0] : cfg_addr;
    if (sda < 0 || scl < 0 || addr <= 0) {
        printf("dac_probe: need pins+addr (dac_config=%s)\n", cfg ? cfg : "(unset)");
        free(cfg);
        return 1;
    }
    bool reinstall = (sda != cfg_sda) || (scl != cfg_scl);
    char tmpcfg[64];
    if (reinstall) {
        // Temporarily move the I2C driver; restored below no matter what.
        snprintf(tmpcfg, sizeof(tmpcfg), "sda=%d,scl=%d,i2c=%d", sda, scl, addr);
        adac_deinit();
        adac_init(tmpcfg, 0);
    }
    // (reg, value written by the es8388 init blob in dac_external.c)
    static const uint8_t regs[] = { 0, 2, 4, 25, 39, 43 };
    static const uint8_t expect[] = { 5, 170, 60, 50, 184, 128 };
    int match = 0, ff = 0;
    uint8_t got[sizeof(regs)] = { 0 };
    printf("dac_probe: addr 0x%02x via sda=%d scl=%d\n", addr, sda, scl);
    for (size_t i = 0; i < sizeof(regs); i++) {
        uint8_t v = adac_read_byte(addr, regs[i]);
        got[i] = v;
        bool ok = (v == expect[i]);
        if (ok) match++;
        if (v == 0xFF) ff++;
        printf("  reg %3u: read 0x%02x, init wrote 0x%02x  %s\n",
            regs[i], v, expect[i], ok ? "MATCH" : "DIFF");
    }
    if (reinstall && cfg) {
        adac_deinit();
        adac_init(cfg, 0);
        printf("dac_probe: I2C driver restored to dac_config pins (sda=%d scl=%d)\n",
            cfg_sda, cfg_scl);
    }
    if (match == (int)sizeof(regs)) {
        printf("RESULT: codec ALIVE on sda=%d scl=%d addr=0x%02x (%d/%d regs match)\n",
            sda, scl, addr, match, (int)sizeof(regs));
    } else if (ff == (int)sizeof(regs)) {
        printf("RESULT: bus DEAD on sda=%d scl=%d addr=0x%02x (all 0xFF: nothing answering)\n",
            sda, scl, addr);
    } else {
        printf("RESULT: PARTIAL on sda=%d scl=%d addr=0x%02x (%d/%d match, %d 0xFF)\n",
            sda, scl, addr, match, (int)sizeof(regs), ff);
    }
    // Mirror the verdict to the LMS message feed so it is readable via
    // http://<device>/messages.json (UART console needs a serial open, and
    // every serial open reboots the board via the DTR reset circuit).
    {
        char verdict[256];
        int n = snprintf(verdict, sizeof(verdict),
            "dac_probe sda=%d scl=%d addr=0x%02x: %d/%d match, %d 0xFF -> %s |",
            sda, scl, addr, match, (int)sizeof(regs), ff,
            match == (int)sizeof(regs) ? "ALIVE" : (ff == (int)sizeof(regs) ? "DEAD" : "PARTIAL"));
        for (size_t i = 0; i < sizeof(regs) && n < (int)sizeof(verdict) - 16; i++) {
            n += snprintf(verdict + n, sizeof(verdict) - (size_t)n,
                " r%u=0x%02x(%s%u)", regs[i], got[i],
                got[i] == expect[i] ? "" : "!",
                (unsigned)expect[i]);
        }
        messaging_post_text(MESSAGING_INFO, MESSAGING_CLASS_SYSTEM, verdict);
    }
    free(cfg);
    return 0;
}

static const esp_console_cmd_t dac_probe_cmd = {
    .command = "dac_probe",
    .help = "Read back ES8388 init registers to prove the codec answers on the I2C pins.",
    .hint = NULL,
    .func = &perform_dac_probe,
    .argtable = &dac_probe_args
};

void register_squeezelite() {
	squeezelite_args.parameters = arg_str0(NULL, NULL, "<parms>", "command line for squeezelite. -h for help, --defaults to launch with default values.");
	squeezelite_args.end = arg_end(1);
	dac_probe_args.sda = arg_int0(NULL, "sda", "<gpio>", "SDA pin to probe (default: from dac_config)");
	dac_probe_args.scl = arg_int0(NULL, "scl", "<gpio>", "SCL pin to probe (default: from dac_config)");
	dac_probe_args.addr = arg_int0(NULL, "addr", "<7bit>", "I2C address (default: i2c= from dac_config)");
	dac_probe_args.end = arg_end(2);
	const esp_console_cmd_t launch_squeezelite = {
		.command = "squeezelite",
		.help = "Starts squeezelite",
		.hint = NULL,
		.func = &launchsqueezelite,
		.argtable = &squeezelite_args
	};
	ESP_ERROR_CHECK( esp_console_cmd_register(&launch_squeezelite) );
	ESP_ERROR_CHECK( esp_console_cmd_register(&dac_probe_cmd) );
}

// dac_probe: is the ES8388 really alive on the configured I2C pins?
// Rationale: adac writes are fire-and-forget (per-byte ACKs ignored), so a
// silent bus (floating lines with pull-ups, wrong pins, dead chip) still
// reports ESP_OK. Reading back registers written at init is the only proof.
// Usage: dac_probe [--sda N --scl N] [--addr A]
//   defaults come from NVS dac_config (sda=/scl=/i2c=). With --sda/--scl the
//   driver is temporarily reinstalled on those pins, probed, then restored
//   to the dac_config pins.

esp_err_t start_ota(const char * bin_url, char * bin_buffer, uint32_t length) {
	if(!bin_url){
		ESP_LOGE(TAG,"missing URL parameter. Unable to start OTA");
		return ESP_ERR_INVALID_ARG;
	}
	ESP_LOGW(TAG, "Called to update the firmware from url: %s",bin_url);
	if(config_set_value(NVS_TYPE_STR, "fwurl", bin_url) != ESP_OK){
		ESP_LOGE(TAG,"Failed to save the OTA url into nvs cache");
		return ESP_FAIL;
	}

	if(!wait_for_commit()){
		ESP_LOGW(TAG,"Unable to commit configuration. ");
	}

	ESP_LOGW(TAG, "Rebooting to recovery to complete the installation");
	return guided_factory();
	return ESP_OK;
}
