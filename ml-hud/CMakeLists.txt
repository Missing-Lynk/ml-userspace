# missinglynk HUD - the on-screen stack for the open kernel (menu, BTFL OSD, System OSD).
#
# Device-only, fully static (aarch64). LVGL (UI toolkit), lodepng (glyph-sheet PNG decode) and cJSON
# (settings) are fetched pinned. Build:
#   cmake -S ml-hud -B ml-hud/build -DCMAKE_TOOLCHAIN_FILE=ml-hud/cmake/aarch64-static.cmake
#   cmake --build ml-hud/build
cmake_minimum_required(VERSION 3.16)
project(missinglynk_hud C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-O2 -Wall -Wextra)

include(FetchContent)

# LVGL (MIT), the menu/UI toolkit: software-rendered widgets, no GPU. Configured by ml-hud/lv_conf.h.
# LV_CONF_INCLUDE_SIMPLE + the source root on the include path lets lvgl.h find lv_conf.h (also used
# by the app's ui/ display + menu). The root holds only lv_conf.h.
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lvgl)

# lodepng (zlib), PNG decode for the BTFL OSD glyph sheet. Upstream ships a single lodepng.cpp with
# no library target; its C++ wrapper is behind __cplusplus, so building the file as C yields the C
# API. gcc dispatches by extension, so copy the .cpp to a .c the compiler builds as C.
FetchContent_Declare(lodepng
    GIT_REPOSITORY https://github.com/lvandeve/lodepng.git
    GIT_TAG 865adac2b8149774401626101ec618e3de45e1d8)
FetchContent_GetProperties(lodepng)
if(NOT lodepng_POPULATED)
    FetchContent_Populate(lodepng)
endif()
configure_file(${lodepng_SOURCE_DIR}/lodepng.cpp ${CMAKE_CURRENT_BINARY_DIR}/lodepng.c COPYONLY)
add_library(lodepng STATIC ${CMAKE_CURRENT_BINARY_DIR}/lodepng.c)
target_include_directories(lodepng PUBLIC ${lodepng_SOURCE_DIR})

# cJSON (MIT), JSON read/write for the settings file. Build just the one source; skip its tests.
FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18)
FetchContent_GetProperties(cjson)
if(NOT cjson_POPULATED)
    FetchContent_Populate(cjson)
endif()
add_library(cjson STATIC ${cjson_SOURCE_DIR}/cJSON.c)
target_include_directories(cjson PUBLIC ${cjson_SOURCE_DIR})

add_executable(hud
    src/hud.c
    src/osd/msp_canvas.c
    src/osd/msp_font.c
    src/osd/btfl_osd.c
    src/channel/osd_channel.c
    src/fb/surface.c
    src/fb/drmoverlay.c
    src/hal/input.c
    src/hal/board_betafpv_p1_hd.c
    src/hal/backlight.c
    src/hal/buzzer.c
    src/hal/telemetry.c
    src/settings/settings.c
    src/services/i18n.c
    src/services/font_zh.c
    src/services/recordings.c
    src/services/linkstate.c
    src/services/pipecmd.c
    src/services/linkcmd.c
    src/ui/display.c
    src/ui/tone.c
    src/ui/menu.c
    src/ui/player.c
    src/ui/sysosd.c)
target_include_directories(hud PRIVATE
    src src/osd src/fb src/channel src/settings src/services src/hal src/ui)
target_link_libraries(hud PRIVATE lvgl lodepng cjson)

# Dev tooling: the OSD-channel replay (captured corpus playback onto the mlm seams, ml-linkd drop-in).
add_executable(osd-replay tools/osd-replay.c)
