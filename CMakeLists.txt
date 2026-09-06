cmake_minimum_required(VERSION 3.22)
project(modbootstrap LANGUAGES CXX ASM)

option(MOD_ROOT_EXTERNAL "Use app-specific external storage (Android/data/<pkg>/files) as mod root" ON)

add_library(modbootstrap SHARED bootstrap.cpp return_buffer_bridge.S)
target_compile_features(modbootstrap PRIVATE cxx_std_17)
target_compile_options(modbootstrap PRIVATE -Wall -Wextra -Werror)
target_link_options(modbootstrap PRIVATE -Wl,--export-dynamic $<$<CONFIG:Release>:-Wl,--strip-all>)
target_link_libraries(modbootstrap PRIVATE android dl log)

if(MOD_ROOT_EXTERNAL)
    target_compile_definitions(modbootstrap PRIVATE MOD_ROOT_EXTERNAL=1)
endif()
