cmake_minimum_required(VERSION 3.22)
project(aap-clap-hosting-helper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(external/clap)
add_subdirectory(external/clap-helpers)

find_package(androidaudioplugin REQUIRED CONFIG)
set(AAP_LINK_TARGET androidaudioplugin::androidaudioplugin)

add_library(aap-clap-hosting-helper SHARED
    src/aap_clap_adapter.cpp
    src/aap_clap_pkg_entry.cpp
    src/aap_clap_factory.cpp
    src/aap_clap_plugin.cpp
    src/aap_clap_preset_discovery.cpp
    src/midi2_event_bridge.cpp
    src/buffer_bridge.cpp
)

target_include_directories(aap-clap-hosting-helper
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(aap-clap-hosting-helper
    PUBLIC  clap
    PRIVATE clap-helpers
    PRIVATE ${AAP_LINK_TARGET}
)
