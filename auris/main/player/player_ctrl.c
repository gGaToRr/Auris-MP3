#include "player/player_ctrl.h"
#include "log/auris_log.h"

#include <string.h>
#include <dirent.h>

#include "freertos/queue.h"

#define CMD_QUEUE_DEPTH 4
#define MAX_TRACKS      16
#define PATH_MAX_LEN    272

static QueueHandle_t s_cmd_queue;

static char s_tracks[MAX_TRACKS][PATH_MAX_LEN];
static int  s_track_count;
static int  s_current;

void player_ctrl_init(void)
{
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(player_cmd_t));
    player_ctrl_scan_tracks();
}

void player_ctrl_send(player_cmd_t cmd)
{
    if (s_cmd_queue)
        xQueueSend(s_cmd_queue, &cmd, 0);
}

player_cmd_t player_ctrl_recv(TickType_t timeout)
{
    player_cmd_t cmd = PLAYER_CMD_NONE;
    if (s_cmd_queue)
        xQueueReceive(s_cmd_queue, &cmd, timeout);
    return cmd;
}

player_cmd_t player_ctrl_peek(void)
{
    player_cmd_t cmd = PLAYER_CMD_NONE;
    if (s_cmd_queue)
        xQueueReceive(s_cmd_queue, &cmd, 0);
    return cmd;
}

void player_ctrl_scan_tracks(void)
{
    s_track_count = 0;
    DIR *d = opendir("/sdcard");
    if (!d) {
        ALOG_W("AUD", "Cannot open /sdcard for track scan");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_track_count < MAX_TRACKS) {
        size_t len = strlen(ent->d_name);
        if (len > 4 && strcasecmp(ent->d_name + len - 4, ".mp3") == 0) {
            snprintf(s_tracks[s_track_count], PATH_MAX_LEN,
                     "/sdcard/%s", ent->d_name);
            ALOG_I("AUD", "Track [%d] %s", s_track_count, s_tracks[s_track_count]);
            s_track_count++;
        }
    }
    closedir(d);

    if (s_track_count == 0)
        ALOG_W("AUD", "No .mp3 files found on /sdcard");
    else
        ALOG_I("AUD", "%d track(s) found", s_track_count);

    s_current = 0;
}

const char *player_ctrl_current_path(void)
{
    if (s_track_count == 0) return NULL;
    return s_tracks[s_current];
}

void player_ctrl_advance(void)
{
    if (s_track_count > 0)
        s_current = (s_current + 1) % s_track_count;
}

int player_ctrl_track_count(void)
{
    return s_track_count;
}
