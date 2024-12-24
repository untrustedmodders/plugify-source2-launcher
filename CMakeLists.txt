cmake_minimum_required(VERSION 3.14 FATAL_ERROR)

if(POLICY CMP0092)
    cmake_policy(SET CMP0092 NEW) # Don't add -W3 warning level by default.
endif()


project(s2-plugify
        VERSION 1.0.0.0 
        DESCRIPTION "Plugify S2 Core" 
        HOMEPAGE_URL "https://github.com/untrustedmodders/s2-plugify" 
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

#
# Plugify
#
set(PLUGIFY_BUILD_SHARED_LIB ON CACHE INTERNAL "")
set(PLUGIFY_BUILD_ASSEMBLY ON CACHE INTERNAL "")
set(PLUGIFY_BUILD_TESTS OFF CACHE INTERNAL "")
if(LINUX)
    set(PLUGIFY_USE_STATIC_STDLIB ON CACHE INTERNAL "")
endif()
add_subdirectory(external/plugify)

#
# Source SDK
#
set(SOURCESDK_LINK_STRIP_CPP_EXPORTS ON CACHE INTERNAL "")
set(SOURCESDK_LINK_STEAMWORKS OFF CACHE INTERNAL "")
set(SOURCESDK_COMPILE_PROTOBUF OFF CACHE INTERNAL "")
set(SOURCESDK_CONFIGURE_EXPORT_MAP OFF CACHE INTERNAL "")

add_subdirectory(external/sourcesdk)

#
# CPP Language Module for Plugify
#
file(GLOB_RECURSE S2PLUGIFY_SOURCES RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} "src/*.cpp")

add_executable(${PROJECT_NAME} ${S2PLUGIFY_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE external/sourcesdk/public)

set(S2PLUGIFY_LINK_LIBRARIES plugify::plugify plugify::plugify-assembly sourcesdk::sourcesdk)

if(NOT COMPILER_SUPPORTS_FORMAT)
    set(S2PLUGIFY_LINK_LIBRARIES ${S2PLUGIFY_LINK_LIBRARIES} fmt::fmt-header-only)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE ${S2PLUGIFY_LINK_LIBRARIES})

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /W4 /WX)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wextra -Wshadow -Wconversion -Wpedantic -Werror)
endif()

if(MSVC)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS} /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
        S2PLUGIFY_PLATFORM_WINDOWS=$<BOOL:${WIN32}>
        S2PLUGIFY_PLATFORM_APPLE=$<BOOL:${APPLE}>
        S2PLUGIFY_PLATFORM_LINUX=$<BOOL:${LINUX}>
        S2PLUGIFY_LIBRARY_SUFFIX="${CMAKE_SHARED_LIBRARY_SUFFIX}"
        S2PLUGIFY_LIBRARY_PREFIX="${CMAKE_SHARED_LIBRARY_PREFIX}"
        S2PLUGIFY_GAME_NAME="csgo"
)