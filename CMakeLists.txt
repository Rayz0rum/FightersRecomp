cmake_minimum_required(VERSION 3.25)
project(stf_xbla LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(STFXBLA_SOURCES
    src/main.cpp
)

if(WIN32)
    list(APPEND STFXBLA_SOURCES src/windows_resources.rc)
    # Rebuild the Windows resource when the icon changes.
    set_property(SOURCE src/windows_resources.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/game.ico")
    add_executable(stf_xbla WIN32 ${STFXBLA_SOURCES})
else()
    add_executable(stf_xbla ${STFXBLA_SOURCES})
endif()

rexglue_setup_target(stf_xbla)

target_sources(rexgpu-xenos PRIVATE src/gpu_live_aa.cpp)

if(REXSDK_DIR)
    target_include_directories(stf_xbla PRIVATE "${REXSDK_DIR}/thirdparty/imgui")
endif()

if(WIN32)
    target_link_libraries(stf_xbla PRIVATE d3d12 dxgi shell32 winmm)
endif()
