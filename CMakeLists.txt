cmake_minimum_required(VERSION 3.20)

# Linux build of the launcher.
#
# The Windows build is premake + MSVC + WebView2 (see premake5.lua). There is no
# WebView2 on Linux, so this build hosts the same ui/ page in WebKitGTK instead: one
# page, one JSON protocol, no Qt on either side.

project(shield_launcher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkit2gtk-4.1)

find_package(ZLIB REQUIRED)

# the page is built into the binary, like Windows embeds it in the exe's resources
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(ui_resources_c ${CMAKE_CURRENT_BINARY_DIR}/ui_resources.c)

add_custom_command(
    OUTPUT ${ui_resources_c}
    COMMAND ${GLIB_COMPILE_RESOURCES}
        --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/source/launcher
        --generate-source
        --target=${ui_resources_c}
        ${CMAKE_CURRENT_SOURCE_DIR}/source/launcher/ui.gresource.xml
    DEPENDS
        source/launcher/ui.gresource.xml
        source/launcher/ui/index.html
        source/launcher/ui/style.css
        source/launcher/ui/app.js
        source/launcher/icon.ico
    VERBATIM
)

set(launcher_sources
    source/launcher/std_include.hpp
    source/launcher/launcher_funcs/dll_loading.cpp
    source/launcher/launcher_funcs/auto_update.cpp
    source/launcher/launcher_funcs/download.cpp
    source/launcher/launcher_funcs/json_utils.cpp
    source/launcher/launcher_funcs/linux_run.cpp
    source/launcher/launcher_funcs/mods.cpp
    source/launcher/launcher_funcs/zip_archive.cpp
    source/launcher/launcher_main/launcher_actions.cpp
    source/launcher/launcher_main/page_actions.cpp
    source/launcher/launcher_main/launcher_main_linux.cpp
    source/utilities/utils.cpp
    ${ui_resources_c}
)

add_executable(Shield_Launcher ${launcher_sources})

target_include_directories(Shield_Launcher PRIVATE
    source/launcher
    source/utilities
    deps/rapidjson/include
)

target_link_libraries(Shield_Launcher PRIVATE
    PkgConfig::GTK
    PkgConfig::WEBKIT
    ZLIB::ZLIB
)

target_compile_definitions(Shield_Launcher PRIVATE LINUX_PORT)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(Shield_Launcher PRIVATE -O2)
endif()
