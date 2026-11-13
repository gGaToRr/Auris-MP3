#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"

typedef enum {
    PLAYER_CMD_NONE = 0,
    PLAYER_CMD_PLAY,
    PLAYER_CMD_RESTART,
    PLAYER_CMD_NEXT,
} player_cmd_t;

void         player_ctrl_init(void);
void         player_ctrl_send(player_cmd_t cmd);
player_cmd_t player_ctrl_recv(TickType_t timeout);
player_cmd_t player_ctrl_peek(void);

const char  *player_ctrl_current_path(void);
void         player_ctrl_advance(void);
void         player_ctrl_scan_tracks(void);
int          player_ctrl_track_count(void);
