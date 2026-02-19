cmake_minimum_required(VERSION 3.22.1)
project(xrpresencetest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenXR SDK path (from prefab/AAR)
find_package(OpenXR REQUIRED CONFIG)

# Platform SDK paths (relative to project - self-contained)
set(OVR_PLATFORM_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ovr_platform_sdk")

# Source files
file(GLOB_RECURSE SRC_FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/*.cpp
)

# ImGui sources
file(GLOB IMGUI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/imgui/*.cpp
)

# Create shared library
add_library(${PROJECT_NAME} SHARED
    ${SRC_FILES}
    ${IMGUI_SOURCES}
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

# Include directories
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../Src/imgui
    ${ANDROID_NDK}/sources/android/native_app_glue
    ${OVR_PLATFORM_SDK}/Include
)

# Platform SDK loader library
add_library(ovrplatformloader SHARED IMPORTED)
set_target_properties(ovrplatformloader PROPERTIES
    IMPORTED_LOCATION ${OVR_PLATFORM_SDK}/Android/libs/arm64-v8a/libovrplatformloader.so
)

# Link libraries
target_link_libraries(${PROJECT_NAME}
    OpenXR::openxr_loader
    ovrplatformloader
    android
    log
    EGL
    GLESv3
)

# Required for NativeActivity
set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate"
)
