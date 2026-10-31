cmake_minimum_required(VERSION 3.16)
project(state C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wextra -Wno-format-truncation)
find_package(Threads REQUIRED)

# Embed the web pages into the binary so the server is a single file.
set(WEB_C "")
foreach(page index dirstat)
    set(src ${CMAKE_SOURCE_DIR}/web/${page}.html)
    set(out ${CMAKE_BINARY_DIR}/web_${page}.c)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -DIN=${src} -DOUT=${out} -DNAME=web_${page}_html
                -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS ${src} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    )
    list(APPEND WEB_C ${out})
endforeach()

if(WIN32)
    set(PLATFORM_SRC
        dirstat/scan_win32.c
        sysinfo/win32.c
        sysinfo/gpu_win32.c
    )
else()
    set(PLATFORM_SRC
        dirstat/scan.c
        sysinfo/host.c
        sysinfo/cpu.c
        sysinfo/mem.c
        sysinfo/disk.c
        sysinfo/gpu.c
    )
endif()

add_executable(state
    src/main.c
    src/util.c
    src/compat.c
    src/server.c
    dirstat/node.c
    dirstat/cache.c
    dirstat/tree.c
    dirstat/json.c
    sysinfo/common.c
    sysinfo/snapshot.c
    ${PLATFORM_SRC}
    ${WEB_C}
)
target_include_directories(state PRIVATE src dirstat sysinfo)

# Version shown by --version: -DSTATE_VERSION=v1.2.3 (CI passes the tag), else git describe.
set(STATE_VERSION "" CACHE STRING "Version string, e.g. v0.0.1")
if(STATE_VERSION)
    set(version ${STATE_VERSION})
else()
    execute_process(COMMAND git describe --tags --always --dirty
                    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                    OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT version)
        set(version dev)
    endif()
endif()
target_compile_definitions(state PRIVATE STATE_VERSION="${version}")
target_link_libraries(state PRIVATE Threads::Threads)

if(WIN32)
    # Windows 10 APIs (GPU counters); static so state.exe needs no MinGW DLLs.
    target_compile_definitions(state PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
    target_link_libraries(state PRIVATE ws2_32 pdh psapi dxgi dxguid shell32 advapi32)
    target_link_options(state PRIVATE -static)
endif()
