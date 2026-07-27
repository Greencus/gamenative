cmake_minimum_required(VERSION 3.22.1)

project(gamenative_xr)

if (NOT ANDROID_ABI STREQUAL "arm64-v8a")
    add_library(gamenative_xr SHARED xr_stub.cpp)
    target_link_libraries(gamenative_xr log)
    return()
endif()

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION
    "${CMAKE_CURRENT_LIST_DIR}/../../jniLibs/${ANDROID_ABI}/libopenxr_loader.so"
)

add_library(gamenative_xr SHARED
    quest_xr.cpp
    xr_transport.cpp
    xr_test_producer.cpp
)

target_include_directories(gamenative_xr PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/openxr"
)

target_compile_options(gamenative_xr PRIVATE
    -Wall
    -Wextra
    -Wno-missing-field-initializers
    -std=c++17
)

target_link_libraries(gamenative_xr
    openxr_loader
    android
    EGL
    GLESv3
    log
)
