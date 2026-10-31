idf_component_register(
    SRCS "zectrix_canvas.cc" "zectrix_panel.cc"
    INCLUDE_DIRS "include" "font"
    REQUIRES zectrix_epd esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
