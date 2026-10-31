idf_component_register(SRCS "app_main.cc"
                       REQUIRES
                       zectrix_board
                       zectrix_demo_ui
                       zectrix_epd
                       esp_timer
                       heap
                       json
                       zectrix_dashboard_wifi
                       zectrix_dashboard_weather
                       zectrix_dashboard_trains
                       zectrix_dashboard_events
                       zectrix_dashboard_commute
                       zectrix_dashboard_outlook
                       zectrix_dashboard_view)
