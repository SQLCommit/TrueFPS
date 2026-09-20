cmake_minimum_required(VERSION 3.22)
project(truefps LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(AshitaSDK REQUIRED)

add_library(truefps MODULE src/truefps.cpp src/truefps.def)
set_target_properties(truefps PROPERTIES
    PREFIX ""
    OUTPUT_NAME truefps
    SUFFIX ".dll"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

ashita_sdk_attach(truefps)
target_compile_options(truefps PRIVATE /EHa /W4 /WX /fp:precise)
target_link_libraries(truefps PRIVATE winmm psapi user32)
target_link_options(truefps PRIVATE /MAP /MAPINFO:EXPORTS)
