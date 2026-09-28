// Framebuffer based player UI: title bar, track info, progress and a clock.
#ifndef PLAYER_UI_H
#define PLAYER_UI_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#include "player_status.h"

// Caches the panel frame buffer. Must be called once after esp_lcd_panel_init().
// `width`/`height` are the configured panel dimensions (RGB565 frame buffer).
esp_err_t player_ui_init(esp_lcd_panel_handle_t panel, int width, int height);

// Paints the whole screen from `st` and pushes it to the panel.
void player_ui_render(esp_lcd_panel_handle_t panel, const player_status_t *st);

#endif // PLAYER_UI_H
