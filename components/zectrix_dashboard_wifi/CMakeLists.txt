idf_component_register(
    SRCS "zectrix_dashboard_wifi.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_event esp_netif esp_wifi nvs_flash
)
