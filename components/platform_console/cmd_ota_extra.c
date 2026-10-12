/* Console commands for notify-only OTA check and deferred flash.
 *
 * Lives in its own object so the recovery image (which registers commands
 * through register_ota_extra, weak) links without these bodies.
 */
#include "cmd_ota.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "platform_console.h"
#include "messaging.h"
#include "ota_check.h"

static const char *TAG = "ota";

// Weak declarations: recovery links without the notify objects.
esp_err_t ota_check_notify(char **out_json) __attribute__((weak));
esp_err_t ota_flash_deferred(const char *url) __attribute__((weak));

static struct {
    struct arg_str *url;
    struct arg_end *end;
} ota_flash_args;

static int perform_ota_check(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!ota_check_notify) {
        printf("OTA check not available in recovery mode.\n");
        return 2;
    }
    char *js = NULL;
    esp_err_t err = ota_check_notify(&js);
    if (js) {
        printf("%s\n", js);
        ESP_LOGI(TAG, "ota_check: %s", js);
        free(js);
    }
    if (err == ESP_ERR_NOT_FOUND) return 0; // up to date
    return err == ESP_OK ? 0 : 1;
}

static int perform_ota_flash(int argc, char **argv)
{
    int nerrors = arg_parse_msg(argc, argv, (struct arg_hdr **)&ota_flash_args);
    if (nerrors != 0) return 1;
    if (!ota_flash_deferred) {
        printf("OTA flash not available in recovery mode.\n");
        return 2;
    }
    esp_err_t err = ota_flash_deferred(ota_flash_args.url->sval[0]);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("Flash deferred (notify-only mode). Set ota_allow_flash=1 to enable.\n");
        return 2;
    }
    return err == ESP_OK ? 0 : 1;
}

static const esp_console_cmd_t check_cmd = {
    .command = "ota_check",
    .help = "Notify-only: check GitHub releases for newer firmware of stored variant (no flash). MQTT+LMS notify.",
    .hint = NULL,
    .func = &perform_ota_check,
};

static const esp_console_cmd_t flash_cmd = {
    .command = "ota_flash",
    .help = "Deferred external flash hook (notify-only by default).",
    .hint = NULL,
    .func = &perform_ota_flash,
    .argtable = &ota_flash_args
};

void register_ota_extra(void)
{
    ota_flash_args.url = arg_str1(NULL, NULL, "<url>", "firmware binary URL (deferred: needs ota_allow_flash=1)");
    ota_flash_args.end = arg_end(2);
    ESP_ERROR_CHECK(esp_console_cmd_register(&check_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&flash_cmd));
    // Expose to the web UI command list (/commands.json -> Execute cards).
    cmd_to_json(&check_cmd);
    cmd_to_json(&flash_cmd);
}
