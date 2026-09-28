#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"
#include "driver/i2s_std.h"
#include "esp_vfs_fat.h"

#include "decoder_mp3.h"
#include "player_status.h"
#include "player_ui.h"

static const char *TAG = "AURIS_MAIN";

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

// Display Configuration
#define LCD_H_RES 480
#define LCD_V_RES 480

// I2S Hardware Output Pins for ESP32-S3
//
// MCLK/BCLK/WS/DOUT is the standard 16-bit I2S pin group. MCLK is not optional
// here: the PCM5102A DAC takes its master clock on SCKI and will not produce
// audio without it, and i2s_channel_init_std_mode() silently skips a pin left at
// I2S_GPIO_UNUSED, so leaving it out gives a clean boot log and no sound.
#define I2S_MCLK_PIN   GPIO_NUM_8
#define I2S_BCLK_PIN   GPIO_NUM_16
#define I2S_WS_PIN     GPIO_NUM_17
#define I2S_DOUT_PIN   GPIO_NUM_18

// Audio Task Stack Size
//
// minimp3's mp3dec_decode_frame() declares a ~14.5 KB mp3dec_scratch_t as a local
// variable, i.e. on the calling task's stack (measured with -fstack-usage:
// 17200 bytes for that frame alone). The deepest Layer III path
// (mp3dec_decode_frame -> L3_decode -> L3_decode_scalefactors -> mp3d_DCT_II ->
// L3_imdct36) needs ~17.6 KB in total, and uxTaskGetStackHighWaterMark() measures
// a 18.1 KB peak in practice (decoding track.mp3 with this minimp3 build).
//
// A 16 KB stack therefore overflows into the heap on every decoded frame: the
// return addresses of the decoder live outside the task stack, get overwritten by
// unrelated heap traffic, and the next `ret` jumps to garbage
// ("Guru Meditation Error: Core 1 panic'ed (InstrFetchProhibited)").
//
// 32 KB keeps a comfortable margin (45% headroom) over the measured peak.
// Re-measure if minimp3.h is updated: the high-water mark printed at the end of
// audio_task() must stay well below 0.
#define AUDIO_TASK_STACK_SIZE (32 * 1024)

static i2s_chan_handle_t tx_handle = NULL;

void mount_virtual_storage(void)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/sdcard", "storage", &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE("STORAGE", "Failed to mount FATFS (0x%x)", err);
        return;
    }
    ESP_LOGI("STORAGE", "Virtual storage mounted at /sdcard");
}

// ---------------------------------------------------------------------------
// 1. DISPLAY DRIVER TASK (Core 0)
// ---------------------------------------------------------------------------
void display_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Initializing QEMU RGB Display...");

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_rgb_qemu_config_t qemu_config = {
        .width = LCD_H_RES,
        .height = LCD_V_RES,
        .bpp = RGB_QEMU_BPP_16,   // frame buffer holds RGB565 pixels
    };

    ESP_ERROR_CHECK(esp_lcd_new_rgb_qemu(&qemu_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(player_ui_init(panel_handle, LCD_H_RES, LCD_V_RES));

    ESP_LOGI(TAG, "Display initialized (%dx%d RGB565).", LCD_H_RES, LCD_V_RES);

    // The UI is only repainted when the published status actually changes, which
    // the audio task does about once per second of playback.
    player_status_t shown;
    player_status_t latest;
    bool have_shown = false;

    while (1) {
        player_status_get(&latest);
        if (!have_shown || memcmp(&latest, &shown, sizeof(latest)) != 0) {
            player_ui_render(panel_handle, &latest);
            shown = latest;
            have_shown = true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------------------------------------------------------------------------
// 2. AUDIO & I2S ENGINE TASK (Core 1)
// ---------------------------------------------------------------------------
static void init_i2s_tx(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // I2S_STD_CLK_DEFAULT_CONFIG generates MCLK at 256 x Fs (11.29 MHz at
    // 44.1 kHz) and BCLK at 64 x Fs. The PCM5102A's MODE pins strap the chip to
    // expect SCKI at 128/192/256/384 x fS; 256 is the common default, and a
    // wrong ratio shows up as wrong pitch or distortion rather than silence.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_PIN,
            .bclk = I2S_BCLK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}

// ---------------------------------------------------------------------------
// Output level
// ---------------------------------------------------------------------------

// Digital gain as a right shift: 0 = full scale, 1 = half, 2 = quarter.
// Earbuds are a 16-32 ohm load right off a line-out DAC, so playing the decoded
// samples untouched means full-scale drive (this track peaks at 32767) with no
// way to turn it down short of pulling the plug.
#define AUDIO_GAIN_SHIFT 1

// 20 ms of audio at 44.1 kHz. The PCM stream starts and stops at whatever
// sample value the encoder happened to leave, and that step is an audible click
// in the analogue domain, so both ends of playback are ramped to silence.
#define AUDIO_FADE_SAMPLES 882

// Applies AUDIO_GAIN_SHIFT plus a linear fade to silence, in place.
//
// `fade_in` and `fade_out` ramp the first and last AUDIO_FADE_SAMPLES frames of
// the block. Where the two overlap (a block shorter than twice the ramp) the
// smaller of the two gains wins, so a single short block comes out as a
// triangle rather than with a step at the point where one ramp should have
// started.
static void shape_output(int16_t *pcm, size_t samples, size_t channels,
                         bool fade_in, bool fade_out)
{
    if (channels == 0 || samples == 0) return;

    size_t frames = samples / channels;
    size_t ramp = AUDIO_FADE_SAMPLES;
    if (ramp > frames) ramp = frames;     // shorter block: ramp across all of it

    for (size_t i = 0; i < frames; i++) {
        // Linear gain in 16.16 fixed point, 65536 == 1.0.
        uint32_t gain = 65536u;

        // Both ramps are scaled so the boundary sample is exactly zero, not
        // 1/ramp of full scale: silencing the discontinuity is the whole point.
        if (fade_in && i < ramp) {
            uint32_t g = (uint32_t)(((uint64_t)i * 65536u) / ramp);
            if (g < gain) gain = g;
        }
        if (fade_out && frames - i <= ramp) {
            uint32_t g = (uint32_t)(((uint64_t)(frames - 1 - i) * 65536u) / ramp);
            if (g < gain) gain = g;
        }

        for (size_t ch = 0; ch < channels; ch++) {
            int16_t *s = &pcm[i * channels + ch];
            // 64-bit intermediate: a full-scale sample times 65536 overflows a
            // signed 32-bit product by exactly one count.
            int32_t scaled = (int32_t)(((int64_t)*s * gain) >> (16 + AUDIO_GAIN_SHIFT));
            *s = (int16_t)scaled;
        }
    }
}

// Shapes one decoded block and hands it to the I2S DMA.
//
// The write uses a 1 s timeout rather than portMAX_DELAY. On real hardware the
// DMA keeps up easily, and if it ever does not, dropping a block is better than
// stalling playback and freezing the display. In QEMU the I2S peripheral is not
// modelled, so every write times out - the drop counter is what makes that
// visible instead of mysterious.
static void output_block(int16_t *pcm, size_t samples, size_t channels,
                         bool fade_in, bool fade_out, uint32_t *dropped_frames)
{
    shape_output(pcm, samples, channels, fade_in, fade_out);

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle, pcm,
                                      samples * sizeof(int16_t),
                                      &bytes_written,
                                      pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        (*dropped_frames)++;
        if (*dropped_frames == 1 || *dropped_frames % 100 == 0) {
            ESP_LOGW(TAG, "I2S write failed (0x%x), %lu frame(s) dropped",
                     err, (unsigned long)*dropped_frames);
        }
    }
}

void audio_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio task started on Core 1.");

    // Snapshot shared with the display task; the audio task owns the local copy
    // and publishes it whenever something visible changes.
    player_status_t st = {
        .phase = PLAYER_PHASE_LOADING,
    };

    const char *test_file = "/sdcard/track.mp3";
    snprintf(st.path, sizeof(st.path), "%s", test_file);
    snprintf(st.message, sizeof(st.message), "OPENING FILE");

    FILE *f = fopen(test_file, "rb");

    if (!f) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "FILE NOT FOUND");
        player_status_set(&st);
        ESP_LOGE(TAG, "Failed to open '%s'. Check if FATFS flash image created the file.", test_file);
        vTaskDelete(NULL);
        return;
    }

    // File size drives the progress bar and the duration estimate.
    if (fseek(f, 0, SEEK_END) == 0) {
        long file_size = ftell(f);
        if (file_size > 0) {
            st.file_size = (uint32_t)file_size;
        }
        rewind(f);
    }

    snprintf(st.message, sizeof(st.message), "DECODING");
    player_status_set(&st);

    // Allocate decoder context on the HEAP to protect task stack
    mp3_decoder_t *dec = calloc(1, sizeof(mp3_decoder_t));
    if (!dec) {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "OUT OF MEMORY");
        player_status_set(&st);
        ESP_LOGE(TAG, "Failed to allocate memory for MP3 decoder structure");
        fclose(f);
        vTaskDelete(NULL);
        return;
    }

    if (mp3_decoder_init(dec, f)) {
        size_t pcm_buf_samples = MP3_MAX_SAMPLES_PER_FRAME;
        // Two blocks: one is decoded into, the previous one is held back so the
        // end of the file is known before the last block is sent. Without that
        // one-block lookahead the final block has already left at full level by
        // the time read_pcm() reports EOF, so it cannot be faded out.
        int16_t *decode_buf = malloc(pcm_buf_samples * sizeof(int16_t));
        int16_t *out_buf   = malloc(pcm_buf_samples * sizeof(int16_t));

        if (!decode_buf || !out_buf) {
            st.phase = PLAYER_PHASE_ERROR;
            snprintf(st.message, sizeof(st.message), "OUT OF MEMORY");
            player_status_set(&st);
            ESP_LOGE(TAG, "Failed to allocate PCM buffer");
            free(decode_buf);
            free(out_buf);
            mp3_decoder_deinit(dec);
            free(dec);
            fclose(f);
            vTaskDelete(NULL);
            return;
        }

        bool i2s_initialized = false;
        bool info_published = false;
        uint32_t published_second = UINT32_MAX;
        uint32_t dropped_frames = 0;
        int64_t start_us = esp_timer_get_time();

        size_t pending = 0;               // Interleaved samples waiting in out_buf
        uint32_t pending_channels = 0;
        bool have_pending = false;
        bool wrote_any = false;

        while (1) {
            int decoded = mp3_decoder_read_pcm(dec, decode_buf, pcm_buf_samples);
            if (decoded <= 0) break; // Playback finished or error

            if (!i2s_initialized) {
                init_i2s_tx(dec->info.sample_rate);
                i2s_initialized = true;
            }

            if (!info_published) {
                st.sample_rate     = dec->info.sample_rate;
                st.bitrate_kbps    = dec->info.bitrate_kbps;
                st.channels        = dec->info.channels;
                st.bits_per_sample = dec->info.bits_per_sample;
                st.layer           = dec->info.layer;
                st.phase           = PLAYER_PHASE_PLAYING;
                snprintf(st.message, sizeof(st.message), "PLAYING");
                info_published     = true;
            }

            uint32_t channels = dec->info.channels ? dec->info.channels : 1;
            st.samples_played += (uint32_t)decoded / channels;

            // Publish once per displayed second instead of once per frame to
            // keep the status copy (and the repaint) cheap.
            uint32_t second = st.sample_rate ? st.samples_played / st.sample_rate : 0;
            if (second != published_second) {
                st.file_pos       = mp3_decoder_position(dec);
                st.bitrate_kbps   = dec->info.bitrate_kbps;     // running average
                st.total_seconds  = mp3_decoder_total_seconds(dec);
                st.total_exact    = mp3_decoder_total_is_exact(dec);
                player_status_set(&st);
                published_second  = second;
            }

            // Getting here means the held-back block was not the last one.
            if (have_pending) {
                output_block(out_buf, pending, pending_channels,
                             !wrote_any, false, &dropped_frames);
                wrote_any = true;
            }

            int16_t *swap = decode_buf;
            decode_buf = out_buf;
            out_buf    = swap;
            pending = (size_t)decoded;
            pending_channels = channels;
            have_pending = true;
        }

        if (have_pending) {
            // End of file, so this really is the last block: fade it out.
            output_block(out_buf, pending, pending_channels,
                         !wrote_any, true, &dropped_frames);
        }

        int64_t elapsed_us = esp_timer_get_time() - start_us;
        ESP_LOGI(TAG, "Decoded %lu samples in %lld ms (%lu frame(s) dropped)",
                 (unsigned long)st.samples_played,
                 (long long)(elapsed_us / 1000),
                 (unsigned long)dropped_frames);
        ESP_LOGI(TAG, "Track: %s, %u kbps%s, %u s (gain 1/%d)",
                 dec->info.vbr ? "VBR" : "CBR",
                 (unsigned)dec->info.bitrate_kbps,
                 mp3_decoder_total_is_exact(dec) ? "" : " (estimated duration)",
                 (unsigned)mp3_decoder_total_seconds(dec),
                 1 << AUDIO_GAIN_SHIFT);

        st.phase = PLAYER_PHASE_FINISHED;
        st.file_pos = st.file_size ? st.file_size : st.file_pos;
        st.total_seconds = mp3_decoder_total_seconds(dec);
        st.total_exact = mp3_decoder_total_is_exact(dec);
        snprintf(st.message, sizeof(st.message), "PLAYBACK COMPLETE");
        player_status_set(&st);

        free(decode_buf);
        free(out_buf);
        mp3_decoder_deinit(dec);
    } else {
        st.phase = PLAYER_PHASE_ERROR;
        snprintf(st.message, sizeof(st.message), "DECODER INIT FAILED");
        player_status_set(&st);
        ESP_LOGE(TAG, "Failed to initialize MP3 decoder context");
    }

    free(dec);
    fclose(f);

    // Report the deepest stack usage reached during decoding: a low value here
    // means AUDIO_TASK_STACK_SIZE is too small for the current minimp3 build.
    ESP_LOGI(TAG, "Audio task stack: %d bytes still free (high-water mark)",
             (int)uxTaskGetStackHighWaterMark(NULL));

    ESP_LOGI(TAG, "Audio playback task completed.");
    vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// 3. APPLICATION ENTRY POINT
// ---------------------------------------------------------------------------
void app_main(void)
{
    ESP_LOGI(TAG, "Starting Auris MP3 Project...");

    // Mount Virtual Storage (FATFS on SPI Flash) for testing
    mount_virtual_storage();

    // Shared playback state must exist before either task publishes to it
    player_status_init();

    // Spawn Display Task on Core 0
    if (xTaskCreatePinnedToCore(
            display_task,
            "gui_task",
            4096,
            NULL,
            2,
            NULL,
            0
        ) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create display task");
    }

    // Spawn Audio Processing Task on Core 1
    // The stack size is critical here: see AUDIO_TASK_STACK_SIZE above.
    if (xTaskCreatePinnedToCore(
            audio_task,
            "audio_task",
            AUDIO_TASK_STACK_SIZE,
            NULL,
            5,
            NULL,
            1
        ) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create audio task (stack size %d bytes)",
                 (int)AUDIO_TASK_STACK_SIZE);
    }
}