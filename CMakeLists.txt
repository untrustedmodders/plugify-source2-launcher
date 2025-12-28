# plugify-source2-launcher
# Copyright (C) 2023-2025 untrustedmodders
# Licensed under the MIT license. See LICENSE file in the project root for details.

cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW) # Don't add -W3 warning level by default.
endif()


file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" VERSION_FILE_CONTENTS)
string(STRIP "${VERSION_FILE_CONTENTS}" VERSION_FILE_CONTENTS)
set(S2_VERSION "${VERSION_FILE_CONTENTS}" CACHE STRING "Set version name")
set(S2_PACKAGE "s2launcher" CACHE STRING "Set package name")
string(REPLACE "v" "" S2_VERSION "${S2_VERSION}")
string(REGEX REPLACE "[.+-]" ";" S2_VERSION_LIST ${S2_VERSION})
list(GET S2_VERSION_LIST 0 S2_VERSION_MAJOR)
list(GET S2_VERSION_LIST 1 S2_VERSION_MINOR)
list(GET S2_VERSION_LIST 2 S2_VERSION_PATCH)

project(plugify-source2-launcher
        VERSION "${S2_VERSION_MAJOR}.${S2_VERSION_MINOR}.${S2_VERSION_PATCH}"
        DESCRIPTION "Plugify S2 Launcher"
        HOMEPAGE_URL "https://github.com/untrustedmodders/plugify-source2-launcher"
        LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
#set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(NOT CMAKE_BUILD_TYPE MATCHES "Debug|Devel|MinSizeRel|RelWithDebInfo|Release")
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
    set(CMAKE_BUILD_TYPE Release)
endif()

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

set(BUILD_SHARED_LIBS ON CACHE INTERNAL "")

#
# Plugify
#
set(PLUGIFY_BUILD_SHARED_LIB ON CACHE INTERNAL "")
set(PLUGIFY_BUILD_TESTS OFF CACHE INTERNAL "")
if(LINUX)
    set(PLUGIFY_USE_STATIC_STDLIB ON CACHE INTERNAL "")
    set(PLUGIFY_USE_ABI0 OFF CACHE INTERNAL "")
endif()
add_subdirectory(external/plugify)

#
# Source SDK
#
set(SOURCESDK_LINK_STRIP_CPP_EXPORTS ON CACHE INTERNAL "")
set(SOURCESDK_LINK_STEAMWORKS OFF CACHE INTERNAL "")
set(SOURCESDK_MALLOC_OVERRIDE OFF CACHE INTERNAL "")
set(SOURCESDK_COMPILE_PROTOBUF OFF CACHE INTERNAL "")
set(SOURCESDK_CONFIGURE_EXPORT_MAP OFF CACHE INTERNAL "")
set(SOURCESDK_USE_ABI0 OFF CACHE INTERNAL "")
add_subdirectory(external/sourcesdk)

#
# Dynlibutils
#
set(DYNLIBUTILS_USE_ABI0 OFF CACHE INTERNAL "")
add_subdirectory(external/dynlibutils)

#
# Sentry
#
add_subdirectory(external/sentry)
if(LINUX)
    file(GENERATE
            OUTPUT "${CMAKE_BINARY_DIR}/pch.h"
            CONTENT "#pragma once\n#include <ucontext.h>\n#include <stdlib.h>\n#include <locale.h>\n#include <dlfcn.h>\n#include <pthread.h>\n"
    )
    target_precompile_headers(sentry PRIVATE "${CMAKE_BINARY_DIR}/pch.h")
    target_compile_definitions(sentry PRIVATE _GNU_SOURCE)

    target_include_directories(sentry SYSTEM PRIVATE external/sentry/external/crashpad/compat/linux)
endif()

#
# Main
#
file(GLOB_RECURSE SOURCE_FILES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*.cpp")

add_executable(${PROJECT_NAME} ${SOURCE_FILES})

#
# Dependencies
#
include(FetchCLI)
include(FetchReproc)
find_package(Threads REQUIRED)

set(S2_LINK_LIBRARIES plugify::plugify glaze::glaze Threads::Threads reproc++ cpp-memory_utils CLI11 sentry::sentry sourcesdk::sourcesdk)

if(NOT COMPILER_SUPPORTS_FORMAT)
    #set(S2_LINK_LIBRARIES ${S2_LINK_LIBRARIES} fmt::fmt-header-only)
endif()

if(WIN32)
    set(S2_LINK_LIBRARIES ${S2_LINK_LIBRARIES} Dbghelp.lib)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${S2_LINK_LIBRARIES} ${CMAKE_DL_LIBS})

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NOMINMAX=1)
endif()

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wextra -Wshadow -Wconversion) #  -Werror -Wpedantic
endif()

# Produce PDB file for debug
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:-g1>)
endif()

if(LINUX)
    set_property(TARGET ${PROJECT_NAME} PROPERTY LINK_FLAGS "-Wl,-rpath,\\\$ORIGIN/")
endif()

if(APPLE)
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/sym/exported_symbols.lds")
elseif(UNIX)
    target_link_options(${PROJECT_NAME} PRIVATE "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

set(S2_GAME_NAME "csgo" CACHE INTERNAL "Set game name")
set(S2_GAME_START "matchmaking" CACHE INTERNAL "Set game library name to start after")
set(S2_COMPILE_DEFINITIONS
        S2_PLATFORM_WINDOWS=$<BOOL:${WIN32}>
        S2_PLATFORM_APPLE=$<BOOL:${APPLE}>
        S2_PLATFORM_LINUX=$<BOOL:${LINUX}>
        S2_LIBRARY_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}"
        S2_LIBRARY_PREFIX="${CMAKE_SHARED_LIBRARY_PREFIX}"
        S2_EXECUTABLE_SUFFIX="${CMAKE_EXECUTABLE_SUFFIX}"
        S2_EXECUTABLE_PREFIX="${CMAKE_EXECUTABLE_PREFIX}"

        S2_GAME_NAME="${S2_GAME_NAME}"
        S2_GAME_START="${S2_GAME_START}"
        S2_PROJECT_YEAR="${PROJECT_BUILD_DATE_YEAR}"
        S2_PROJECT_NAME="${PROJECT_NAME}"
        S2_PROJECT_DESCRIPTION="${CMAKE_PROJECT_DESCRIPTION}"
        S2_PROJECT_HOMEPAGE_URL="${CMAKE_PROJECT_HOMEPAGE_URL}"
        S2_PROJECT_VERSION="${S2_VERSION}"
)

set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME ${S2_PACKAGE})

if(WIN32)
    set(S2_MICROMAMBA "micromamba-win-64.exe")
    set(S2_COMPILE_DEFINITIONS
            ${S2_COMPILE_DEFINITIONS}
            S2_PLATFORM="windows"
            S2_BINARY="win64"
    )
elseif(LINUX)
    set(S2_MICROMAMBA "micromamba-linux-64")
    set(S2_COMPILE_DEFINITIONS
            ${S2_COMPILE_DEFINITIONS}
            S2_PLATFORM="linux"
            S2_BINARY="linuxsteamrt64"
    )
elseif(APPLE)
    set(S2_MICROMAMBA "micromamba-osx-64")
    set(S2_COMPILE_DEFINITIONS
            ${S2_COMPILE_DEFINITIONS}
            S2_PLATFORM="macos"
            S2_BINARY="osx64"
    )
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE ${S2_COMPILE_DEFINITIONS})
#target_compile_options(${PROJECT_NAME} PRIVATE ${S2_COMPILE_OPTIONS})

configure_file(
        ${CMAKE_SOURCE_DIR}/micromamba/${S2_MICROMAMBA}
        ${CMAKE_BINARY_DIR}/${CMAKE_EXECUTABLE_PREFIX}micromamba${CMAKE_EXECUTABLE_SUFFIX}
        COPYONLY
)

configure_file(
        ${CMAKE_SOURCE_DIR}/sentry.jsonc.in
        ${CMAKE_BINARY_DIR}/sentry.jsonc
)
