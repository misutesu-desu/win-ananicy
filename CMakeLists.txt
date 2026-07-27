cmake_minimum_required(VERSION 3.20)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" WINANANICY_VERSION_FILE)
string(STRIP "${WINANANICY_VERSION_FILE}" WINANANICY_VERSION_FILE)
project(WinAnanicy VERSION "${WINANANICY_VERSION_FILE}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    UNICODE
    _UNICODE
    WINANANICY_VERSION="${PROJECT_VERSION}"
)

include(FetchContent)
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

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
target_link_libraries(
    win-ananicy
    PRIVATE
        nlohmann_json::nlohmann_json
        Advapi32
        User32
        PowrProf
        Psapi
        Shell32
        Ole32
)

if(MSVC)
    set_property(TARGET win-ananicy PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(win-ananicy PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(
        win-ananicy
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
    )
    if(MINGW)
        target_link_options(
            win-ananicy
            PRIVATE
                -municode
                -static
                -static-libgcc
                -static-libstdc++
        )
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(
        win-ananicy-config-tests
        tests/config_tests.cpp
        src/config.cpp
        src/logger.cpp
        src/process_utils.cpp
    )
    target_include_directories(win-ananicy-config-tests PRIVATE src)
    target_link_libraries(
        win-ananicy-config-tests
        PRIVATE
            nlohmann_json::nlohmann_json
            Advapi32
            User32
            PowrProf
            Psapi
    )
    if(MSVC)
        target_compile_options(win-ananicy-config-tests PRIVATE /W4 /WX /permissive- /utf-8)
    else()
        target_compile_options(
            win-ananicy-config-tests
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
                -Wshadow
        )
        if(MINGW)
            target_link_options(
                win-ananicy-config-tests
                PRIVATE
                    -static
                    -static-libgcc
                    -static-libstdc++
            )
        endif()
    endif()
    add_test(NAME config-validation COMMAND win-ananicy-config-tests)
endif()
