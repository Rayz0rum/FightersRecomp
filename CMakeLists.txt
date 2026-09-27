cmake_minimum_required(VERSION 3.25)
project(stf_xbla LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(STFXBLA_SOURCES
    src/main.cpp
    src/installer/button_guide.cpp
    src/installer/imgui_utils.cpp
    src/installer/installer.cpp
    src/installer/installer_app.cpp
    src/installer/installer_assets.cpp
    src/installer/installer_audio.cpp
    src/installer/installer_font.cpp
    src/installer/installer_input.cpp
    src/installer/installer_wizard.cpp
    src/installer/message_window.cpp
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
    target_link_libraries(stf_xbla PRIVATE d3d12 dxgi shell32 winmm cabinet bcrypt ole32 uuid)
endif()

# The installer's assets: built from the game's own files (sprites, fonts,
# button glyphs, sounds) by tools/installer_assets, and embedded as a resource.
# Without the game files the installer isn't available.
set(STF_GAME_MEDIA "${CMAKE_CURRENT_SOURCE_DIR}/_unpacked/media")
set(STF_INSTALLER_ASSETS_DIR "${CMAKE_CURRENT_BINARY_DIR}/installer_assets")
if(WIN32 AND EXISTS "${STF_GAME_MEDIA}/rom/fontmap.farc" AND TARGET mspack)
    add_executable(stf_installer_assets tools/installer_assets/installer_assets.cpp)
    target_compile_features(stf_installer_assets PRIVATE cxx_std_20)
    target_include_directories(stf_installer_assets PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/installer"
        "${REXSDK_DIR}/thirdparty/stb")
    target_link_libraries(stf_installer_assets PRIVATE mspack cabinet)
    file(GLOB STF_INSTALLER_CHARACTERS "${CMAKE_CURRENT_SOURCE_DIR}/res/installer/characters/*.png")
    add_custom_command(
        OUTPUT "${STF_INSTALLER_ASSETS_DIR}/installer_assets.bin"
               "${STF_INSTALLER_ASSETS_DIR}/installer_assets.rc"
        COMMAND stf_installer_assets
            --media "${STF_GAME_MEDIA}"
            --res "${CMAKE_CURRENT_SOURCE_DIR}/res/installer"
            --logo "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png"
            --out "${STF_INSTALLER_ASSETS_DIR}"
        DEPENDS stf_installer_assets
            "${STF_GAME_MEDIA}/rom/fontmap.farc"
            "${STF_GAME_MEDIA}/rom/sprite/n_fnt.farc"
            "${STF_GAME_MEDIA}/rom/sprite/n_stf.farc"
            "${STF_GAME_MEDIA}/rom/sprite/n_cmn.farc"
            "${STF_GAME_MEDIA}/rom/sound/stf_all.acb"
            "${STF_GAME_MEDIA}/rom/sound/stf_all.awb"
            "${CMAKE_CURRENT_SOURCE_DIR}/res/installer/strings.txt"
            "${CMAKE_CURRENT_SOURCE_DIR}/res/installer/kbm.png"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png"
            ${STF_INSTALLER_CHARACTERS}
        COMMENT "Building the installer's assets from the game files"
        VERBATIM)
    target_sources(stf_xbla PRIVATE "${STF_INSTALLER_ASSETS_DIR}/installer_assets.rc")
    set_property(SOURCE "${STF_INSTALLER_ASSETS_DIR}/installer_assets.rc" APPEND PROPERTY
        OBJECT_DEPENDS "${STF_INSTALLER_ASSETS_DIR}/installer_assets.bin")
else()
    message(WARNING "Game files not found in _unpacked/media - the installer won't be available")
endif()
