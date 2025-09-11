cmake_policy(PUSH)

if(POLICY CMP0169)
    # Allow calling FetchContent_Populate directly.
    cmake_policy(SET CMP0169 OLD)
endif()

include(FetchContent)

message(STATUS "Pulling and configuring crashpad_cmake")

FetchContent_Declare(
        crashpad_cmake
        GIT_REPOSITORY "https://github.com/TheAssemblyArmada/crashpad-cmake.git"
        GIT_TAG "80573adcc845071401c73c99eaec7fd9847d45fb"
        GIT_PROGRESS TRUE
        GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(crashpad_cmake)

cmake_policy(POP)

if(LINUX OR MINGW)
    file(GENERATE
            OUTPUT "${CMAKE_BINARY_DIR}/pch.h"
            CONTENT "#pragma once\n#include <cstdint>\n"
    )
    target_precompile_headers(minichromium PUBLIC "${CMAKE_BINARY_DIR}/pch.h")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(crashpad_snapshot PRIVATE -Wno-template-id-cdtor)
    endif()
elseif(APPLE)
    target_compile_options(minichromium PUBLIC -Wno-deprecated-declarations -Wno-sign-conversion)
endif()

if(UNIX)
    target_compile_definitions(minichromium PUBLIC CRASHPAD_USE_BORINGSSL=1)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(minichromium PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

add_dependencies(${PROJECT_NAME} crashpad_handler)
