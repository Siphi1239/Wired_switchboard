idf_component_register(
    SRCS "app_main.cpp"
         "matter_app.cpp"
         "device_model.c"
         "eth_t1s.c"
         "inputs.c"
         "outputs.c"
         "indicators.c"
         "sensors.c"
         "local_control.c"
         "app_settings.c"
         "app_console.c"
         "board.c"
         "mqtt_app.c"
    PRIV_INCLUDE_DIRS "." "${ESP_MATTER_PATH}/examples/common/utils"
    # esp_matter_app_ethernet_init() is only weakly referenced by connectedhomeip:
    # keep every object so the strong definition is always linked.
    WHOLE_ARCHIVE)
# No REQUIRES: like the esp-matter examples, main depends on all components
# (esp_matter, esp_matter_console, lan865x, mqtt, led_strip, drivers, json).

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
