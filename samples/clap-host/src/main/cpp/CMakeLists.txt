cmake_minimum_required(VERSION 3.22)
project(sample-clap-host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(aap-clap-hosting-helper REQUIRED CONFIG)
find_package(oboe REQUIRED CONFIG)

add_library(sample-clap-host SHARED
    sample_host.cpp
)

# clap is a submodule in the aap-clap-hosting-helper repo; it is not packaged in the Prefab AAR,
# so we reference its headers directly from the repo tree.
get_filename_component(REPO_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../" ABSOLUTE)

target_include_directories(sample-clap-host PRIVATE
    "${REPO_ROOT}/external/clap/include"
    "${REPO_ROOT}/external/clap-helpers/include"
)

target_link_libraries(sample-clap-host
    PRIVATE aap-clap-hosting-helper::aap-clap-hosting-helper
    PRIVATE oboe::oboe
    PRIVATE android
    PRIVATE log
)
