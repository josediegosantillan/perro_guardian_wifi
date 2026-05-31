idf_component_register(
    SRCS
        "main.c"
        "app_state.c"
        "button_control.c"
        "display_oled.c"
        "wifi_credentials.c"
        "wifi_config_portal.c"
        "wifi_manager.c"
        "internet_check.c"
        "relay_control.c"
        "router_watchdog.c"
        "status_leds.c"
        "telegram_bot.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client esp_timer nvs_flash driver esp-tls
)
