message(STATUS "Pulling and configuring CLI11")

FetchContent_Declare(
        cli
        GIT_REPOSITORY "https://github.com/CLIUtils/CLI11.git"
        GIT_TAG "v2.5.0"
        GIT_PROGRESS TRUE
        GIT_SHALLOW TRUE
        OVERRIDE_FIND_PACKAGE
)

FetchContent_MakeAvailable(cli)

