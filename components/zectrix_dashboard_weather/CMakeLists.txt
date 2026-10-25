idf_component_register(
    SRCS "zectrix_dashboard_weather.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client json mbedtls
)
