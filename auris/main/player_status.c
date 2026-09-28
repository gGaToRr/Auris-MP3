#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "player_status.h"

static const char *TAG = "PLAYER";

static StaticSemaphore_t s_mutex;
static SemaphoreHandle_t s_lock;
static player_status_t s_status;

// Guard against a caller publishing an unterminated string.
static void sanitise(player_status_t *s)
{
    s->path[sizeof(s->path) - 1] = '\0';
    s->message[sizeof(s->message) - 1] = '\0';
}

void player_status_init(void)
{
    s_lock = xSemaphoreCreateMutexStatic(&s_mutex);
    if (!s_lock) {
        ESP_LOGE(TAG, "Failed to create status mutex");
        return;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.phase = PLAYER_PHASE_IDLE;
}

void player_status_set(const player_status_t *status)
{
    if (!status) return;
    if (!s_lock) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = *status;
    sanitise(&s_status);
    xSemaphoreGive(s_lock);
}

void player_status_get(player_status_t *out)
{
    if (!out) return;

    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->phase = PLAYER_PHASE_IDLE;
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
