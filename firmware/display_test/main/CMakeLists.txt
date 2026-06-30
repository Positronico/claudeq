idf_component_register(SRCS "main.cpp"
                    REQUIRES i2c_bsp lcd_bl_pwm_bsp espressif__esp_io_expander_tca9554
                    INCLUDE_DIRS ".")
