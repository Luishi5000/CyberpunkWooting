cmake_minimum_required(VERSION 3.20)
project(WootingAnalogBridge VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)

FetchContent_Declare(
    red4ext.sdk
    GIT_REPOSITORY https://github.com/WopsS/RED4ext.SDK.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(red4ext.sdk)

FetchContent_Declare(
    redlib
    GIT_REPOSITORY https://github.com/psiberx/cp2077-red-lib.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(redlib)

add_library(${PROJECT_NAME} SHARED 
    src/Main.cpp
)

target_include_directories(${PROJECT_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor
)

target_link_libraries(${PROJECT_NAME} PRIVATE 
    RED4ext::SDK 
    RedLib
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/wooting_analog_wrapper.lib
    Ws2_32
    Userenv
    Bcrypt
    ntdll
    Psapi
)