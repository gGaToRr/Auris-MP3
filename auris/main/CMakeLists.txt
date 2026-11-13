idf_component_register(SRCS "player_ui.c" "player_status.c" "decode_mp3.c" "auris.c"
                            "ble/ble_scan.c"
                            "ble/ble_connect.c"
                            "ble/ble_gatt_svc.c"
                            "log/auris_log.c"
                            "player/player_ctrl.c"
                    INCLUDE_DIRS "." "include"
                    REQUIRES bt nvs_flash
                    PRIV_REQUIRES esp_driver_i2s esp_driver_spi esp_driver_gpio
                                  esp_lcd esp_timer fatfs esp_rom)

fatfs_create_spiflash_image(storage spiflash FLASH_IN_PROJECT)
