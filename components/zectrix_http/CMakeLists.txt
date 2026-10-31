idf_component_register(
    SRCS "zectrix_http.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client mbedtls
)
