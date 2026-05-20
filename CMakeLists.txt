cmake_minimum_required(VERSION 3.20)
project(WinAnanicy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Configure static linking for MSVC runtime library (MultiThreaded / MultiThreadedDebug)
# This removes external DLL dependencies for the compiled executable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Fetch nlohmann/json for config parsing
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

# Sources
set(SOURCES
    src/main.cpp
    src/logger.hpp
    src/logger.cpp
    src/config.hpp
    src/config.cpp
    src/process_utils.hpp
    src/process_utils.cpp
)

add_executable(win-ananicy ${SOURCES})
target_link_libraries(win-ananicy PRIVATE nlohmann_json::nlohmann_json)

# Additional compiler/linker flags for low-level features
if(MSVC)
    target_compile_options(win-ananicy PRIVATE /W4 /WX /permissive- /utf-8)
    # Link required libraries
    target_link_libraries(win-ananicy PRIVATE Advapi32.lib User32.lib)
endif()
