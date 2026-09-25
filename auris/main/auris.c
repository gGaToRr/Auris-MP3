#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_qemu_rgb.h"

#define LCD_H_RES 480
#define LCD_V_RES 320

void draw_test_pattern(esp_lcd_panel_handle_t panel_handle)
{
    int band_height = LCD_V_RES / 4;
    uint16_t *buffer = malloc(LCD_H_RES * band_height * sizeof(uint16_t));
    if (!buffer) return;

    // RGB565 test colors: Red, Green, Blue, Yellow
    uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFE0};

    for (int i = 0; i < 4; i++) {
        uint16_t color = colors[i];
        for (int p = 0; p < LCD_H_RES * band_height; p++) {
            buffer[p] = color;
        }

        int y_start = i * band_height;
        int y_end = y_start + band_height;

        esp_lcd_panel_draw_bitmap(panel_handle, 0, y_start, LCD_H_RES, y_end, buffer);
    }

    free(buffer);
}

void app_main(void)
{
    esp_lcd_panel_handle_t panel_handle = NULL;

    esp_lcd_rgb_qemu_config_t qemu_config = {
        .width = LCD_H_RES,
        .height = LCD_V_RES,
    };

    // 1. Create virtual panel
    ESP_ERROR_CHECK(esp_lcd_new_rgb_qemu(&qemu_config, &panel_handle));

    // 2. Initialize memory buffer (Do NOT call reset or disp_on_off)
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // 3. Render color bars
    draw_test_pattern(panel_handle);
}