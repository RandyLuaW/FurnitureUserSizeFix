cmake_minimum_required(VERSION 3.21)
project(FurnitureUserSizeFix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT, matching what SKSE plugins are normally built against.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(CommonLibSSE CONFIG REQUIRED)

# The in-game menu is optional. The integration header is GPL-3.0 and is not
# part of this repository (see include/README.md); without it the plugin still
# builds and works, driven entirely by the configuration file.
set(HAVE_MENU_FRAMEWORK OFF)
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/SKSEMenuFramework.h")
    set(HAVE_MENU_FRAMEWORK ON)
    message(STATUS "SKSE Menu Framework header found: building WITH the in-game menu")
else ()
    message(STATUS "SKSE Menu Framework header not found: building WITHOUT the in-game menu")
endif ()

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

if (MSVC)
    # /utf-8: the source carries Chinese comments; MSVC would otherwise decode it
    # as GBK and fail with unrelated-looking syntax errors.
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (HAVE_MENU_FRAMEWORK)
    target_compile_definitions(${PROJECT_NAME} PRIVATE FUSF_WITH_MENU=1)
    # The framework header relies on <filesystem> / <format> / <atomic>.
    find_package(Threads REQUIRED)
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)

if (MSVC)
    # Keep the PDB path out of the binary: releases are distributed as-is.
    target_link_options(${PROJECT_NAME} PRIVATE /PDBALTPATH:%_PDB%)
endif ()

set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "FurnitureUserSizeFix"
)
