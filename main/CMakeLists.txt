idf_component_register(
    SRCS
        "main.c"
        "tf.c"
        "bus_i2c.c"
        "board.c"
        "axp2101.c"
        "es8311.c"
        "audio_path.c"
        "wifi_lab.c"
        "ili9342c.c"
        "cam_ov2640.c"
        "cam_jpeg.c"
        "cam_web.c"
        "cam_mqtt.c"
        "sec_crypto.c"
        "mqtt_cmd.c"
        "mqtt_console.c"
        "batt_log.c"
        "ddns.c"
        "health.c"
        "app_cmds.c"
        "test_core.c"
        "test_i2c.c"
        "test_axp2101.c"
        "test_es8311.c"
        "test_audio.c"
        "test_wifi.c"
        "test_lcd.c"
        "test_camera.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_gpio
        esp_driver_spi
        esp_driver_ledc
        esp_timer
        spi_flash
        esp_psram
        esp_driver_cam
        # 收帧改用 components/esp32-camera（同款板量产工程实测能出帧的那份）。
        # 自研的 esp_cam_ctlr_dvp 路径在本板拿不到 EOF 回调，本工程只保留它
        # 做 bring-up 诊断（cam probe / cam fmt），正式出图走这里。
        esp32-camera
        esp_http_server
        # DDNS 上报走 HTTPS，需要 http 客户端 + TLS 证书包
        esp_http_client
        esp-tls
        mbedtls
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
)

# printf 旁路：mqtt_console.c 里的 __wrap_printf/__wrap_puts/__wrap_putchar
# 会接管全工程的输出调用，从而在不改动任何调用点的前提下，把命令输出复制
# 一份给 MQTT 回传。puts / putchar 也必须 wrap —— GCC 会把
# printf("字面量\n") 优化成 puts()、printf("%c", c) 优化成 putchar()。
# INTERFACE 让这些选项传到最终链接，否则只作用于 main 这个静态库、不生效。
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=printf"
    "-Wl,--wrap=puts"
    "-Wl,--wrap=putchar"
)

