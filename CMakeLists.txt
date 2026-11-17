cmake_minimum_required(VERSION 3.16)
project(tb4117_verify C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(JPEG REQUIRED)
find_package(Freetype REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(AV REQUIRED libavformat libavcodec libavutil libswscale)

# LVGL 9 with our lv_conf.h (same dir as this file)
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/third_party)   # for "#include <lvgl/lvgl.h>"
include_directories(${DRM_INCLUDE_DIRS})   # libdrm ships drm.h in /usr/include/libdrm
include_directories(${AV_INCLUDE_DIRS})
include_directories(${FREETYPE_INCLUDE_DIRS})   # the lvgl freetype module includes ft2build.h
link_directories(${DRM_LIBRARY_DIRS})
link_directories(${AV_LIBRARY_DIRS})

# we only need the library: skip demos/examples/thorvg (hundreds of files)
set(LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(LV_BUILD_THORVG OFF CACHE BOOL "" FORCE)

set(LV_BUILD_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h")
add_subdirectory(third_party/lvgl)

add_executable(tb4117_verify src/main.c src/lv_font_cn_22.c)
target_include_directories(tb4117_verify PRIVATE
    ${FREETYPE_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${JPEG_INCLUDE_DIRS})
target_link_libraries(tb4117_verify PRIVATE
    lvgl
    ${FREETYPE_LIBRARIES}
    ${CURL_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${DRM_LIBRARIES}
    ${AV_LIBRARIES}
    pthread
    m)
