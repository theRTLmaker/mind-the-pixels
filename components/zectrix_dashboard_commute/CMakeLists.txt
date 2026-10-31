idf_component_register(
    SRCS "zectrix_dashboard_commute.cpp" "board_stream.cc"
    INCLUDE_DIRS "include"
    REQUIRES zectrix_dashboard_trains zectrix_http
)
