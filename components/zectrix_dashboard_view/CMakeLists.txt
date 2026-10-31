idf_component_register(
    SRCS "zectrix_dashboard_view.cc"
    INCLUDE_DIRS "include"
    REQUIRES zectrix_demo_ui zectrix_dashboard_weather zectrix_dashboard_trains
             zectrix_dashboard_events zectrix_dashboard_commute
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
