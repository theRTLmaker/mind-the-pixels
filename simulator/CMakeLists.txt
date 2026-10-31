# Mac simulator for the Mind the Pixels dashboard.
#
# Builds the real ZectrixCanvas, font and dashboard view sources for the host
# and renders sample scenarios to PNG. See "Mac simulator" in README.md.
cmake_minimum_required(VERSION 3.16)
project(mind_the_pixels_simulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

find_package(ZLIB REQUIRED)

add_executable(mtp_sim
    main.cc
    png_writer.cc
    ${REPO_ROOT}/components/zectrix_demo_ui/zectrix_canvas.cc
    ${REPO_ROOT}/components/zectrix_dashboard_view/zectrix_dashboard_view.cc
)

# host_include comes first so its ESP-IDF stand-ins win over the SDK.
target_include_directories(mtp_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/host_include
    ${REPO_ROOT}/components/zectrix_epd/include
    ${REPO_ROOT}/components/zectrix_demo_ui/include
    ${REPO_ROOT}/components/zectrix_demo_ui/font
    ${REPO_ROOT}/components/zectrix_dashboard_weather/include
    ${REPO_ROOT}/components/zectrix_dashboard_trains/include
    ${REPO_ROOT}/components/zectrix_http/include
    ${REPO_ROOT}/components/zectrix_dashboard_events/include
    ${REPO_ROOT}/components/zectrix_dashboard_commute/include
    ${REPO_ROOT}/components/zectrix_dashboard_outlook/include
    ${REPO_ROOT}/components/zectrix_dashboard_view/include
)
target_compile_options(mtp_sim PRIVATE -Wall -Wextra)
target_link_libraries(mtp_sim PRIVATE ZLIB::ZLIB)
