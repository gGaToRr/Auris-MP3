idf_component_register(SRCS "player_ui.c" "player_status.c" "decode_mp3.c" "auris.c"
                            "ble/ble_scan.c"
                            "ble/ble_connect.c"
                    INCLUDE_DIRS "." "include"
                    REQUIRES bt nvs_flash)

fatfs_create_spiflash_image(storage spiflash FLASH_IN_PROJECT)
