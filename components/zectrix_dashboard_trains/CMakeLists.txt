idf_component_register(
    SRCS "zectrix_dashboard_trains.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls json
)
