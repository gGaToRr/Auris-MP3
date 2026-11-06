idf_component_register(SRCS "auris.c"
                            "ble/ble_scan.c"
                            "ble/ble_connect.c"
                    INCLUDE_DIRS "."
                    REQUIRES bt nvs_flash)
