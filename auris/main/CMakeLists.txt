idf_component_register(SRCS "player_ui.c" "player_status.c" "decode_mp3.c" "auris.c"
                    INCLUDE_DIRS "." "include")

fatfs_create_spiflash_image(storage spiflash FLASH_IN_PROJECT)
