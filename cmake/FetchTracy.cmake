include(FetchContent)

message(STATUS "Pulling and configuring tracy")

set(TRACY_STATIC ON CACHE BOOL "" FORCE)

FetchContent_Declare(
        tracy
        GIT_REPOSITORY "https://github.com/wolfpld/tracy.git"
        GIT_TAG "v0.13.1"
        GIT_PROGRESS TRUE
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
        EXCLUDE_FROM_ALL
)

FetchContent_MakeAvailable(tracy)