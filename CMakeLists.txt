cmake_minimum_required(VERSION 3.16)
project(stat C)

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

add_executable(stat
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
target_include_directories(stat PRIVATE src dirstat sysinfo)
target_link_libraries(stat PRIVATE Threads::Threads)

if(WIN32)
    # Windows 10 APIs (GPU counters); static so stat.exe needs no MinGW DLLs.
    target_compile_definitions(stat PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
    target_link_libraries(stat PRIVATE ws2_32 pdh psapi dxgi dxguid shell32 advapi32)
    target_link_options(stat PRIVATE -static)
endif()
