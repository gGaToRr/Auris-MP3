#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

// BLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "ble/ble_scan.h"
#include "ble/ble_connect.h"
#include "ble/ble_gatt_svc.h"
#include "log/auris_log.h"
#include "player/player_ctrl.h"

// Display
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ili9488.h"

// Audio
#include "driver/i2s_std.h"
#include "esp_vfs_fat.h"

#include "decoder_mp3.h"
#include "player_status.h"
#include "player_ui.h"

static const char *TAG = "AURIS_MAIN";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// ===========================================================================
// BLE
// ===========================================================================

static const ble_uuid128_t adv_svc_uuid =
    BLE_UUID128_INIT(0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x01, 0x00, 0xd4, 0xc3, 0xb2, 0xa1);

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    ble_gap_adv_set_fields(&fields);

    // Scan response carries the service UUID so Android can filter on it
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&adv_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT, NULL,
                               BLE_HS_FOREVER, &adv_params,
                               ble_connect_gap_event_cb, NULL);
    if (rc != 0)
        printf("[adv] start failed: %d\n", rc);
    else
        printf("[adv] advertising as \"%s\"\n", name);
}

static void configure_security(void)
{
    ble_hs_cfg.sm_io_cap     = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding    = 1;
    ble_hs_cfg.sm_mitm       = 0;  // NoIO → Just Works, MITM impossible
    ble_hs_cfg.sm_sc         = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }

    configure_security();
    ble_connect_set_on_disconnect(start_advertising);
    start_advertising();

    printf("\n=== AURIS — BLE Ready ===\n");
    printf("Advertising as \"AURIS\"\n");
    printf("Commands: scan | conn <idx> | disc | list | notify <msg> | status\n");
    printf("=========================\n\n");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void read_line(char *buf, int max)
{
    int i = 0;
    while (i < max - 1) {
        int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\n' || c == '\r') {
            putchar('\n');
            break;
        }
        putchar(c);
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
}

static void cmd_loop(void *param)
{
    (void)param;
    char line[64];

    vTaskDelay(pdMS_TO_TICKS(500));

    for (;;) {
        printf("auris> ");
        fflush(stdout);
        read_line(line, sizeof(line));

        if (strncmp(line, "scan", 4) == 0) {
            uint16_t sec = 10;
            if (line[4] == ' ')
                sec = (uint16_t)atoi(line + 5);
            printf("[scan] scanning %d seconds...\n", sec);
            ble_scan_clear();
            ble_scan_start(sec);

            vTaskDelay(pdMS_TO_TICKS(sec * 1000 + 500));
            ble_scan_print_results();

        } else if (strncmp(line, "conn", 4) == 0 && line[4] == ' ') {
            int idx = atoi(line + 5);
            const ble_scan_results_t *res = ble_scan_get_results();
            if (idx < 0 || idx >= res->count) {
                printf("invalid index\n");
                continue;
            }
            const ble_scan_device_t *d = &res->devices[idx];
            if (!d->connectable) {
                printf("device not connectable\n");
                continue;
            }
            printf("[conn] connecting to %s...\n",
                   d->name[0] ? d->name : "(unknown)");
            ble_connect_to(d->addr, d->addr_type);

        } else if (strcmp(line, "disc") == 0) {
            ble_disconnect();

        } else if (strcmp(line, "list") == 0) {
            ble_scan_print_results();

        } else if (strncmp(line, "notify", 6) == 0) {
            const char *msg = line + 6;
            while (*msg == ' ') msg++;
            if (*msg == '\0') {
                printf("usage: notify <message>\n");
            } else {
                int rc = ble_gatt_svc_notify((const uint8_t *)msg,
                                             (uint16_t)strlen(msg));
                if (rc == 0)
                    printf("[notify] sent: %s\n", msg);
            }

        } else if (strcmp(line, "status") == 0) {
            printf("\n--- BLE Status ---\n");
            printf("  connected : %s\n", ble_connect_is_connected() ? "yes" : "no");
            printf("  subscribed: %s\n", ble_gatt_svc_is_subscribed() ? "yes" : "no");
            if (ble_connect_is_connected())
                printf("  conn handle: %d\n", ble_connect_get_handle());
            printf("------------------\n");

        } else if (line[0] != '\0') {
            printf("unknown command: %s\n", line);
        }
    }
}

// ===========================================================================
// DISPLAY
// ===========================================================================

// Display Configuration
#define LCD_H_RES 480
#define LCD_V_RES 320

#define LCD_SPI_HOST SPI2_HOST
#define LCD_SCLK_PIN GPIO_NUM_12
#define LCD_MOSI_PIN GPIO_NUM_11
#define LCD_CS_PIN GPIO_NUM_10
#define LCD_DC_PIN GPIO_NUM_9
#define LCD_RST_PIN GPIO_NUM_6
#define LCD_BL_PIN GPIO_NUM_7

// I2S Hardware Output Pins for ESP32-S3
#define I2S_MCLK_PIN GPIO_NUM_8
#define I2S_BCLK_PIN GPIO_NUM_16
#define I2S_WS_PIN GPIO_NUM_17
#define I2S_DOUT_PIN GPIO_NUM_18

// minimp3's mp3dec_decode_frame() uses ~14.5 KB of stack for mp3dec_scratch_t.
// 32 KB keeps ~45% headroom over the measured 18.1 KB peak.
#define AUDIO_TASK_STACK_SIZE (36 * 1024)

static i2s_chan_handle_t tx_handle = NULL;

void mount_virtual_storage(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE};

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "storage", &mount_config, &s_wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE("STORAGE", "Failed to mount FATFS (0x%x)", err);
        return;
    }
    ESP_LOGI("STORAGE", "Virtual storage mounted at /sdcard");
}

void display_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Initializing ILI9488 SPI display...");

    gpio_config_t bl_config = {
        .pin_bit_mask = 1ULL << LCD_BL_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_config));
    gpio_set_level(LCD_BL_PIN, 0);

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_SCLK_PIN,
        .mosi_io_num = LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 40 * 3,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_DC_PIN,
        .cs_gpio_num = LCD_CS_PIN,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 18,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9488(io_handle, &panel_config,
                                              LCD_H_RES * 40, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    vTaskDelay(pdMS_TO_TICKS(120));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    esp_err_t ui_err = player_ui_init(panel_handle, LCD_H_RES, LCD_V_RES);
    if (ui_err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to allocate display framebuffer: %s", esp_err_to_name(ui_err));
        esp_lcd_panel_del(panel_handle);
        esp_lcd_panel_io_del(io_handle);
        spi_bus_free(LCD_SPI_HOST);
        vTaskDelete(NULL);
        return;
    }
    gpio_set_level(LCD_BL_PIN, 1);

    ESP_LOGI(TAG, "Display initialized (%dx%d RGB565).", LCD_H_RES, LCD_V_RES);

    player_status_t shown;
    player_status_t latest;
    bool have_shown = false;
    int loop_count = 0;

    while (1)
    {
        player_status_get(&latest);
        if (!have_shown || memcmp(&latest, &shown, sizeof(latest)) != 0)
        {
            player_ui_render(panel_handle, &latest);
            shown = latest;
            have_shown = true;
        }
        if (++loop_count % 10 == 0)
        {
            ESP_LOGI(TAG, "Status [%s]: %lu/%lu s",
                     latest.message,
                     (unsigned long)(latest.sample_rate ? latest.samples_played / latest.sample_rate : 0),
                     (unsigned long)latest.total_seconds);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ===========================================================================
// AUDIO & I2S ENGINE
// ===========================================================================

static void init_i2s_tx(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

#define AUDIO_FADE_SAMPLES 882

static void shape_output(int32_t *pcm, size_t samples, size_t channels,
                         bool fade_in, bool fade_out)
{
    if (channels == 0 || samples == 0)
        return;

    size_t frames = samples / channels;
    size_t ramp = AUDIO_FADE_SAMPLES;
    if (ramp > frames)
        ramp = frames;

    uint32_t vol = player_ctrl_get_volume();

    for (size_t i = 0; i < frames; i++)
    {
        uint32_t gain = (vol * 65536u) / 100u;

        if (fade_in && i < ramp)
        {
            uint32_t g = (uint32_t)(((uint64_t)i * gain) / ramp);
            if (g < gain)
                gain = g;
        }
        if (fade_out && frames - i <= ramp)
        {
            uint32_t g = (uint32_t)(((uint64_t)(frames - 1 - i) * gain) / ramp);
            if (g < gain)
                gain = g;
        }

        for (size_t ch = 0; ch < channels; ch++)
        {
            int32_t *s = &pcm[i * channels + ch];
            *s = (int32_t)(((int64_t)*s * gain) >> 16);
        }
    }
}

static void convert_pcm_for_i2s(const int16_t *pcm, int32_t *i2s_pcm,
                                size_t samples)
{
    for (size_t i = 0; i < samples; i++)
    {
        i2s_pcm[i] = (int32_t)pcm[i] << 16;
    }
}

static void output_block(int32_t *pcm, size_t samples, size_t channels,
                         bool fade_in, bool fade_out, uint32_t *dropped_frames)
{
    shape_output(pcm, samples, channels, fade_in, fade_out);

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(
        tx_handle,
        pcm,
        samples * sizeof(int32_t),
        &bytes_written,
        pdMS_TO_TICKS(1000));
    if (err != ESP_OK)
    {
        (*dropped_frames)++;
        if (*dropped_frames == 1 || *dropped_frames % 100 == 0)
        {
            ESP_LOGW(TAG, "I2S write failed (0x%x), %lu frame(s) dropped",
                     err, (unsigned long)*dropped_frames);
        }
    }
}

// Returns the command that interrupted playback, or PLAYER_CMD_NONE on natural end.
static player_cmd_t play_file(const char *path, bool *i2s_ready)
{
    player_cmd_t interrupted = PLAYER_CMD_NONE;
    player_status_t st = { .phase = PLAYER_PHASE_LOADING };

    snprintf(st.path, sizeof(st.path), "%s", path);
    snprintf(st.message, sizeof(st.message), "OPENING FILE");
    player_status_set(&st);
    ALOG_I("AUD", "Opening %s", path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "FILE NOT FOUND");
        player_status_set(&st);
        ALOG_E("AUD", "Cannot open %s", path);
        return PLAYER_CMD_NONE;
    }

    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz > 0) st.file_size = (uint32_t)sz;
        rewind(f);
    }

    snprintf(st.message, sizeof(st.message), "DECODING");
    player_status_set(&st);

    mp3_decoder_t *dec = calloc(1, sizeof(mp3_decoder_t));
    if (!dec) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "OUT OF MEMORY");
        player_status_set(&st);
        fclose(f);
        return PLAYER_CMD_NONE;
    }

    if (!mp3_decoder_init(dec, f)) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "DECODER INIT FAILED");
        player_status_set(&st);
        ALOG_E("AUD", "Decoder init failed for %s", path);
        free(dec);
        fclose(f);
        return PLAYER_CMD_NONE;
    }

    size_t pcm_buf_samples = MP3_MAX_SAMPLES_PER_FRAME;
    int16_t *decode_buf = malloc(pcm_buf_samples * sizeof(int16_t));
    int32_t *out_buf    = malloc(pcm_buf_samples * sizeof(int32_t));
    int32_t *pending_buf = malloc(pcm_buf_samples * sizeof(int32_t));

    if (!decode_buf || !out_buf || !pending_buf) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "OUT OF MEMORY");
        player_status_set(&st);
        free(decode_buf); free(out_buf); free(pending_buf);
        mp3_decoder_deinit(dec); free(dec); fclose(f);
        return PLAYER_CMD_NONE;
    }

    bool info_published = false;
    uint32_t published_second = UINT32_MAX;
    uint32_t dropped_frames = 0;
    int64_t start_us = esp_timer_get_time();

    size_t pending = 0;
    uint32_t pending_channels = 0;
    bool have_pending = false;
    bool wrote_any = false;

    bool paused = false;

    while (1) {
        // Check for BLE commands (non-blocking)
        player_cmd_t cmd = player_ctrl_peek();
        if (cmd == PLAYER_CMD_RESTART || cmd == PLAYER_CMD_NEXT
            || cmd == PLAYER_CMD_PREV) {
            ALOG_I("AUD", "Command: %d", cmd);
            interrupted = cmd;
            break;
        }
        if (cmd == PLAYER_CMD_PAUSE) {
            paused = !paused;
            ALOG_I("AUD", paused ? "Paused" : "Resumed");
            snprintf(st.message, sizeof(st.message), paused ? "PAUSED" : "PLAYING");
            st.phase = paused ? PLAYER_PHASE_IDLE : PLAYER_PHASE_PLAYING;
            player_status_set(&st);
        }
        if (cmd == PLAYER_CMD_VOL_UP) {
            uint8_t v = player_ctrl_get_volume();
            player_ctrl_set_volume(v + 10 > 100 ? 100 : v + 10);
            ALOG_I("AUD", "Volume: %d", player_ctrl_get_volume());
        }
        if (cmd == PLAYER_CMD_VOL_DOWN) {
            uint8_t v = player_ctrl_get_volume();
            player_ctrl_set_volume(v < 10 ? 0 : v - 10);
            ALOG_I("AUD", "Volume: %d", player_ctrl_get_volume());
        }
        if (paused) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int decoded = mp3_decoder_read_pcm(dec, decode_buf, pcm_buf_samples);
        if (decoded <= 0)
            break;

        if (!*i2s_ready) {
            init_i2s_tx(dec->info.sample_rate);
            *i2s_ready = true;
        }

        if (!info_published) {
            st.sample_rate    = dec->info.sample_rate;
            st.bitrate_kbps   = dec->info.bitrate_kbps;
            st.channels       = dec->info.channels;
            st.bits_per_sample = dec->info.bits_per_sample;
            st.layer          = dec->info.layer;
            st.phase          = PLAYER_PHASE_PLAYING;
            snprintf(st.message, sizeof(st.message), "PLAYING");
            info_published = true;
        }

        uint32_t channels = dec->info.channels ? dec->info.channels : 1;
        st.samples_played += (uint32_t)decoded / channels;

        uint32_t second = st.sample_rate ? st.samples_played / st.sample_rate : 0;
        if (second != published_second) {
            st.file_pos      = mp3_decoder_position(dec);
            st.bitrate_kbps  = dec->info.bitrate_kbps;
            st.total_seconds = mp3_decoder_total_seconds(dec);
            st.total_exact   = mp3_decoder_total_is_exact(dec);
            player_status_set(&st);
            published_second = second;
        }

        if (have_pending) {
            output_block(pending_buf, pending, pending_channels,
                         !wrote_any, false, &dropped_frames);
            wrote_any = true;
        }

        convert_pcm_for_i2s(decode_buf, out_buf, (size_t)decoded);
        int32_t *swap = pending_buf;
        pending_buf = out_buf;
        out_buf = swap;
        pending = (size_t)decoded;
        pending_channels = channels;
        have_pending = true;
    }

    if (have_pending && interrupted == PLAYER_CMD_NONE)
        output_block(pending_buf, pending, pending_channels,
                     !wrote_any, true, &dropped_frames);

    int64_t elapsed_us = esp_timer_get_time() - start_us;
    ALOG_I("AUD", "Played %lu samples in %lld ms (%lu dropped)",
           (unsigned long)st.samples_played,
           (long long)(elapsed_us / 1000),
           (unsigned long)dropped_frames);

    if (interrupted == PLAYER_CMD_NONE) {
        st.phase = PLAYER_PHASE_FINISHED;
        st.file_pos = st.file_size ? st.file_size : st.file_pos;
        st.total_seconds = mp3_decoder_total_seconds(dec);
        st.total_exact   = mp3_decoder_total_is_exact(dec);
        snprintf(st.message, sizeof(st.message), "PLAYBACK COMPLETE");
        player_status_set(&st);
    }

    free(decode_buf);
    free(out_buf);
    free(pending_buf);
    mp3_decoder_deinit(dec);
    free(dec);
    fclose(f);

    return interrupted;
}

void audio_task(void *pvParameters)
{
    (void)pvParameters;
    ALOG_I("AUD", "Audio task started on Core 1");

    bool i2s_ready = false;

    // Auto-play first track on boot
    while (1) {
        const char *path = player_ctrl_current_path();
        if (!path) {
            ALOG_W("AUD", "No tracks available, waiting for command...");
            player_ctrl_recv(portMAX_DELAY);
            player_ctrl_scan_tracks();
            continue;
        }

        player_cmd_t cmd = play_file(path, &i2s_ready);

        switch (cmd) {
        case PLAYER_CMD_RESTART:
            ALOG_I("AUD", "Restarting current track");
            break;
        case PLAYER_CMD_NEXT:
            player_ctrl_advance();
            ALOG_I("AUD", "Next → %s",
                   player_ctrl_current_path() ? player_ctrl_current_path() : "none");
            break;
        case PLAYER_CMD_PREV:
            player_ctrl_rewind();
            ALOG_I("AUD", "Prev → %s",
                   player_ctrl_current_path() ? player_ctrl_current_path() : "none");
            break;
        default:
            // Natural end — advance to next, or wait
            player_ctrl_advance();
            ALOG_I("AUD", "Track ended → %s",
                   player_ctrl_current_path() ? player_ctrl_current_path() : "none");
            break;
        }
    }
}

// ===========================================================================
// APPLICATION ENTRY POINT
// ===========================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "=== AURIS Boot ===");

    // 1. NVS (needed by log daemon + BLE bonding)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // 2. Storage (needed by log daemon)
    mount_virtual_storage();

    // 3. Log daemon (earliest possible — everything after this is logged)
    auris_log_init();

    // 4. BLE daemon
    ALOG_I("BLE", "Starting BLE daemon...");

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ALOG_E("BLE", "nimble init failed: %d", ret);
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_device_name_set("AURIS");
    ble_gatt_svc_init();
    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(cmd_loop, "ble_cmd", 4096, NULL, 5, NULL);

    ALOG_I("BLE", "AURIS advertising started");

    // 5. Player (display + audio)
    player_ctrl_init();
    player_status_init();

    if (xTaskCreatePinnedToCore(display_task, "gui_task", 8192,
                                NULL, 2, NULL, 0) != pdPASS)
        ALOG_E("SYS", "Failed to create display task");

    if (xTaskCreatePinnedToCore(audio_task, "audio_task", AUDIO_TASK_STACK_SIZE,
                                NULL, 5, NULL, 1) != pdPASS)
        ALOG_E("SYS", "Failed to create audio task");

    ALOG_I("SYS", "=== AURIS Ready ===");
}
