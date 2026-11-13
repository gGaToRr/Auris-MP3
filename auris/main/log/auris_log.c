#include "log/auris_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "miniz.h"

#define LOG_TAG     "ALOG"
#define LOG_DIR     "/sdcard/logs"
#define LOG_CURRENT LOG_DIR "/auris.log"

#define LOG_QUEUE_DEPTH 32
#define LOG_TASK_STACK  (4 * 1024)

typedef struct {
    int64_t  timestamp_us;
    char     module[8];
    char     level;
    char     msg[120];
} log_entry_t;

static QueueHandle_t s_queue;
static FILE         *s_file;
static uint32_t      s_boot_id;
static uint32_t      s_file_size;

// ── Boot counter (NVS) ──

static uint32_t next_boot_id(void)
{
    nvs_handle_t h;
    uint32_t id = 0;
    if (nvs_open("auris_log", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "boot", &id);
        id++;
        nvs_set_u32(h, "boot", id);
        nvs_commit(h);
        nvs_close(h);
    }
    return id;
}

static uint32_t next_rotation_id(void)
{
    nvs_handle_t h;
    uint32_t id = 0;
    if (nvs_open("auris_log", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "rot", &id);
        id++;
        nvs_set_u32(h, "rot", id);
        nvs_commit(h);
        nvs_close(h);
    }
    return id;
}

// ── Gzip compression ──

static uint32_t gzip_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static bool compress_to_gzip(const char *src_path, const char *dst_path)
{
    FILE *f = fopen(src_path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long src_len = ftell(f);
    rewind(f);

    if (src_len <= 0 || src_len > AURIS_LOG_MAX_SIZE + 1024) {
        fclose(f);
        return false;
    }

    uint8_t *src = malloc((size_t)src_len);
    if (!src) { fclose(f); return false; }
    fread(src, 1, (size_t)src_len, f);
    fclose(f);

    size_t dst_max = (size_t)src_len + 512;
    uint8_t *dst = malloc(dst_max);
    if (!dst) { free(src); return false; }

    size_t compressed = tdefl_compress_mem_to_mem(
        dst, dst_max, src, (size_t)src_len,
        TDEFL_DEFAULT_MAX_PROBES | TDEFL_GREEDY_PARSING_FLAG);

    if (compressed == 0) {
        free(src);
        free(dst);
        return false;
    }

    FILE *out = fopen(dst_path, "wb");
    if (!out) { free(src); free(dst); return false; }

    // Gzip header (RFC 1952)
    const uint8_t gz_hdr[] = {0x1f, 0x8b, 0x08, 0x00, 0,0,0,0, 0x04, 0xff};
    fwrite(gz_hdr, 1, sizeof(gz_hdr), out);
    fwrite(dst, 1, compressed, out);

    uint32_t crc = gzip_crc32(src, (size_t)src_len);
    uint32_t orig_size = (uint32_t)src_len;
    fwrite(&crc, 1, 4, out);
    fwrite(&orig_size, 1, 4, out);

    fclose(out);
    free(src);
    free(dst);
    return true;
}

// ── Rotation & cleanup ──

static void purge_oldest_archives(void)
{
    typedef struct { char name[32]; } fname_t;
    fname_t archives[AURIS_LOG_MAX_ARCHIVES + 4];
    int count = 0;

    DIR *d = opendir(LOG_DIR);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < (int)(sizeof(archives)/sizeof(archives[0]))) {
        if (strncmp(ent->d_name, "auris_", 6) == 0 &&
            strstr(ent->d_name, ".log.gz") != NULL) {
            strncpy(archives[count].name, ent->d_name, sizeof(archives[0].name) - 1);
            archives[count].name[sizeof(archives[0].name) - 1] = '\0';
            count++;
        }
    }
    closedir(d);

    // Sort by name (rotation id is zero-padded → alphabetical = chronological)
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(archives[i].name, archives[j].name) > 0) {
                fname_t tmp = archives[i];
                archives[i] = archives[j];
                archives[j] = tmp;
            }

    while (count > AURIS_LOG_MAX_ARCHIVES) {
        char path[64];
        snprintf(path, sizeof(path), LOG_DIR "/%s", archives[0].name);
        remove(path);
        ESP_LOGI(LOG_TAG, "Purged old archive: %s", archives[0].name);
        memmove(&archives[0], &archives[1], (size_t)(count - 1) * sizeof(fname_t));
        count--;
    }
}

static void rotate_log(void)
{
    if (!s_file) return;
    fclose(s_file);
    s_file = NULL;

    uint32_t rot_id = next_rotation_id();
    char gz_path[64];
    snprintf(gz_path, sizeof(gz_path), LOG_DIR "/auris_%04lu.log.gz", (unsigned long)rot_id);

    if (compress_to_gzip(LOG_CURRENT, gz_path)) {
        ESP_LOGI(LOG_TAG, "Rotated → %s", gz_path);
    } else {
        // Fallback: keep uncompressed
        char plain[64];
        snprintf(plain, sizeof(plain), LOG_DIR "/auris_%04lu.log", (unsigned long)rot_id);
        rename(LOG_CURRENT, plain);
        ESP_LOGW(LOG_TAG, "Compression failed, kept as %s", plain);
    }

    remove(LOG_CURRENT);
    purge_oldest_archives();

    s_file = fopen(LOG_CURRENT, "w");
    s_file_size = 0;
}

// ── Writer task ──

static void format_entry(const log_entry_t *e, char *buf, size_t buf_len)
{
    uint32_t sec = (uint32_t)(e->timestamp_us / 1000000);
    uint32_t ms  = (uint32_t)((e->timestamp_us / 1000) % 1000);
    snprintf(buf, buf_len, "[%05lu.%03lu][%-4s][%c] %s\n",
             (unsigned long)sec, (unsigned long)ms,
             e->module, e->level, e->msg);
}

static void writer_task(void *arg)
{
    (void)arg;
    log_entry_t entry;
    char line[180];

    while (1) {
        if (xQueueReceive(s_queue, &entry, pdMS_TO_TICKS(5000))) {
            if (!s_file) continue;

            format_entry(&entry, line, sizeof(line));
            size_t len = strlen(line);
            fwrite(line, 1, len, s_file);
            s_file_size += (uint32_t)len;

            // Drain batch
            while (xQueueReceive(s_queue, &entry, 0)) {
                format_entry(&entry, line, sizeof(line));
                len = strlen(line);
                fwrite(line, 1, len, s_file);
                s_file_size += (uint32_t)len;
            }

            fflush(s_file);

            if (s_file_size >= AURIS_LOG_MAX_SIZE)
                rotate_log();
        }
    }
}

// ── Public API ──

void auris_log_init(void)
{
    mkdir(LOG_DIR, 0755);

    s_boot_id = next_boot_id();
    s_queue = xQueueCreate(LOG_QUEUE_DEPTH, sizeof(log_entry_t));

    // Check existing log size (append across reboots)
    struct stat st;
    if (stat(LOG_CURRENT, &st) == 0) {
        s_file_size = (uint32_t)st.st_size;
        if (s_file_size >= AURIS_LOG_MAX_SIZE) {
            // Pre-rotate before opening
            s_file = fopen(LOG_CURRENT, "a");
            rotate_log();
        } else {
            s_file = fopen(LOG_CURRENT, "a");
        }
    } else {
        s_file = fopen(LOG_CURRENT, "w");
        s_file_size = 0;
    }

    xTaskCreate(writer_task, "alog", LOG_TASK_STACK, NULL, 1, NULL);

    ESP_LOGI(LOG_TAG, "Log daemon started (boot #%lu)", (unsigned long)s_boot_id);
    ALOG_I("SYS", "======== BOOT #%lu ========", (unsigned long)s_boot_id);
}

void auris_log_write(const char *module, char level, const char *fmt, ...)
{
    if (!s_queue) return;

    log_entry_t entry;
    entry.timestamp_us = esp_timer_get_time();
    entry.level = level;
    strncpy(entry.module, module, sizeof(entry.module) - 1);
    entry.module[sizeof(entry.module) - 1] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.msg, sizeof(entry.msg), fmt, args);
    va_end(args);

    // Mirror to serial console
    char line[180];
    format_entry(&entry, line, sizeof(line));
    printf("%s", line);

    // Non-blocking enqueue (drop if queue full — never block a caller)
    xQueueSend(s_queue, &entry, 0);
}
