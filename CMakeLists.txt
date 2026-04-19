cmake_minimum_required(VERSION 3.20)
project(LiveHaptics VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch hidapi – force static so no DLL dependency
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(
    hidapi
    GIT_REPOSITORY https://github.com/libusb/hidapi.git
    GIT_TAG        hidapi-0.14.0
)
FetchContent_MakeAvailable(hidapi)

set(SOURCES
    src/main.cpp
    src/hidpp/HidDevice.cpp
    src/hidpp/HidppDevice.cpp
    src/haptics/HapticController.cpp
    src/ui/TrayApp.cpp
    src/ui/PopupWindow.cpp
    src/config/Config.cpp
    resources/app.rc
)

add_executable(LiveHaptics WIN32 ${SOURCES})

target_include_directories(LiveHaptics PRIVATE
    src
    ${hidapi_SOURCE_DIR}/hidapi
)

target_link_libraries(LiveHaptics PRIVATE
    hidapi::hidapi
    gdiplus
    gdi32
    user32
    shell32
    comctl32
    ole32
    uuid
    dwmapi
    uxtheme
    oleacc
    uiautomationcore
)

target_compile_definitions(LiveHaptics PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(LiveHaptics PRIVATE /W4 /MD)
endif()
