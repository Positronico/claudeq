idf_component_register(SRCS "main.cpp" "net.cpp" "ui.cpp" "audio.cpp" "provision.cpp"
                    REQUIRES i2c_bsp lcd_bl_pwm_bsp espressif__esp_io_expander_tca9554
                             esp_wifi esp_event nvs_flash esp_netif json
                             esp_http_server lwip mdns
                             espressif__esp_websocket_client
                             espressif__esp_codec_dev esp_driver_i2s esp_driver_i2c
                             microlink
                    INCLUDE_DIRS ".")
