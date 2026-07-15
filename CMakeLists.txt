cmake_minimum_required(VERSION 3.20)
project(arachnel_plugin_freetp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ARACHNEL_SDK_DIR)
    if(DEFINED ENV{ARACHNEL_SDK_DIR})
        set(ARACHNEL_SDK_DIR "$ENV{ARACHNEL_SDK_DIR}")
    else()
        set(ARACHNEL_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Arachnel")
    endif()
endif()
cmake_path(ABSOLUTE_PATH ARACHNEL_SDK_DIR NORMALIZE)

find_package(Qt6 REQUIRED COMPONENTS Core Network)

include(${ARACHNEL_SDK_DIR}/cmake/ArachnelPluginSdk.cmake)
arachnel_plugin_sdk_init(${ARACHNEL_SDK_DIR})

# Catalog lives in freetp-hydra-link; plugin.json catalogUrl is the runtime source of truth.
# Optional: download a snapshot at configure time to bundle offline copies inside .arach.
set(FREETP_CATALOG "${CMAKE_CURRENT_SOURCE_DIR}/games-arachnel.json")
set(FREETP_CATALOG_URL
    "https://gitlab.com/BadKiko/freetp-hydra-link/-/raw/main/games-arachnel.json?ref_type=heads")

if(NOT "$ENV{ARACHNEL_SKIP_FREETP_CATALOG_FETCH}" STREQUAL "1")
    message(STATUS "freetp: fetching games-arachnel.json from freetp-hydra-link ...")
    file(DOWNLOAD "${FREETP_CATALOG_URL}" "${FREETP_CATALOG}" STATUS _dl_status TIMEOUT 180)
    list(GET _dl_status 0 _dl_code)
    if(NOT _dl_code EQUAL 0)
        message(WARNING
            "freetp: could not download games-arachnel.json (plugin still works via catalogUrl in plugin.json)")
    endif()
elseif(EXISTS "${FREETP_CATALOG}")
    message(STATUS "freetp: using existing games-arachnel.json (fetch skipped)")
else()
    message(STATUS "freetp: no local catalog; runtime feed from plugin.json catalogUrl")
endif()

add_library(freetp_plugin SHARED
    src/freetp_plugin.cpp
    src/archive_installer.cpp
    src/installer_runner.cpp
    src/linux_fix_launch.cpp
    src/plugin_entry.cpp
)

target_include_directories(freetp_plugin PRIVATE src)
target_link_libraries(freetp_plugin PRIVATE arachnel_plugin_sdk Qt6::Core Qt6::Network)

set_target_properties(freetp_plugin PROPERTIES
    OUTPUT_NAME "freetp_plugin"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-bundle"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin-bundle"
)
if(MINGW)
    set_target_properties(freetp_plugin PROPERTIES PREFIX "")
elseif(MSVC)
    # Visual Studio appends Release/Debug under RUNTIME_OUTPUT_DIRECTORY otherwise.
    foreach(_cfg IN ITEMS Release RelWithDebInfo Debug MinSizeRel)
        set_target_properties(freetp_plugin PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/plugin-bundle"
            LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/plugin-bundle"
        )
    endforeach()
endif()

set(_PLUGIN_BUNDLE_DIR "${CMAKE_BINARY_DIR}/plugin-bundle")
set(_FREETP_ARACH "${CMAKE_BINARY_DIR}/dist/freetp.arach")
set(_FREETP_ARACH_STAGING "${CMAKE_BINARY_DIR}/_staging_freetp_arach/freetp")

arachnel_stage_plugin_bundle(freetp_plugin freetp "${_PLUGIN_BUNDLE_DIR}")

add_custom_command(TARGET freetp_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/_staging_freetp_arach"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_FREETP_ARACH_STAGING}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${_PLUGIN_BUNDLE_DIR}" "${_FREETP_ARACH_STAGING}"
    COMMENT "Stage freetp .arach payload"
)

arachnel_package_plugin_arach(freetp_plugin freetp "${_FREETP_ARACH_STAGING}" "${_FREETP_ARACH}")

add_custom_target(freetp_arach ALL DEPENDS freetp_plugin)
