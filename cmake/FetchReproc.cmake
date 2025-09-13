message(STATUS "Pulling and configuring reproc")

set(REPROC++ ON)
set(REPROC_OBJECT_LIBRARIES ON)

FetchContent_Declare(reproc
        GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
        GIT_TAG v14.2.5
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(reproc)
