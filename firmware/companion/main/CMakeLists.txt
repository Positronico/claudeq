idf_component_register(SRCS "main.cpp" "net.cpp" "ui.cpp" "ota.cpp" "audio.cpp" "provision.cpp" "battery.cpp" "trust.cpp" "pairing.cpp" "font_mic_18.c" "font_deck_16.c" "font_deck_12.c" "font_back_32.c"
                    REQUIRES i2c_bsp lcd_bl_pwm_bsp espressif__esp_io_expander_tca9554
                             esp_wifi esp_event nvs_flash esp_netif json esp_adc
                             esp_http_server lwip mdns mbedtls
                             espressif__esp_websocket_client
                             espressif__esp_codec_dev esp_driver_i2s esp_driver_i2c
                             microlink
                             app_update esp_https_ota esp_http_client esp-tls
                    INCLUDE_DIRS ".")
