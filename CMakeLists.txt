cmake_minimum_required(VERSION 3.16)
project(TwinSteerMPQUIC LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SIRIUS_SDK_DIR "" CACHE PATH "Path to the Ettifos SIRIUS SDK root")
if(NOT SIRIUS_SDK_DIR)
    message(FATAL_ERROR "Set SIRIUS_SDK_DIR to the Ettifos SIRIUS SDK root (contains inc/ and lib/).")
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENCV REQUIRED IMPORTED_TARGET opencv4)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

find_library(GPS_LIBRARY gps)
find_library(MOSQUITTO_LIBRARY mosquitto)
if(NOT GPS_LIBRARY)
    message(FATAL_ERROR "libgps not found")
endif()
if(NOT MOSQUITTO_LIBRARY)
    message(FATAL_ERROR "libmosquitto not found")
endif()

# Keep the public artifact small: build picoquic core/log only and let picoquic
# fetch the picotls revision pinned by its upstream CMake configuration.
set(BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(picoquic_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_LOGLIB ON CACHE BOOL "" FORCE)
set(PICOQUIC_FETCH_PTLS ON CACHE BOOL "" FORCE)
add_subdirectory(picoquic)

set(SIRIUS_SDK_LIB_DIR "${SIRIUS_SDK_DIR}/lib")

function(configure_v2x_target target)
    target_include_directories(${target} PRIVATE
        "${SIRIUS_SDK_DIR}"
        "${SIRIUS_SDK_DIR}/inc"
        "${CMAKE_CURRENT_SOURCE_DIR}/picoquic/picoquic")
    target_link_directories(${target} PRIVATE
        "${SIRIUS_SDK_DIR}"
        "${SIRIUS_SDK_LIB_DIR}")
    target_link_libraries(${target} PRIVATE
        picoquic-log
        picoquic-core
        etf_seamless_sdk
        etfv2x_sdk
        efosv2xsdk_seamless_01
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads)
endfunction()

add_executable(mpquic_sender sender/sender.cpp)
configure_v2x_target(mpquic_sender)
target_link_libraries(mpquic_sender PRIVATE
    ${GPS_LIBRARY}
    PkgConfig::OPENCV
    PkgConfig::GSTREAMER)

add_executable(mpquic_receiver receiver/receiver.cpp)
configure_v2x_target(mpquic_receiver)
target_link_libraries(mpquic_receiver PRIVATE
    ${GPS_LIBRARY}
    ${MOSQUITTO_LIBRARY})

add_executable(pc5_beacon tools/pc5_beacon.cpp)
configure_v2x_target(pc5_beacon)
target_link_libraries(pc5_beacon PRIVATE
    ${GPS_LIBRARY}
    ${MOSQUITTO_LIBRARY})
