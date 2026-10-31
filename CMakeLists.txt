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

add_executable(stat
    src/main.c
    src/util.c
    src/server.c
    dirstat/scan.c
    dirstat/cache.c
    dirstat/tree.c
    dirstat/json.c
    sysinfo/cpu.c
    sysinfo/mem.c
    sysinfo/disk.c
    sysinfo/gpu.c
    sysinfo/snapshot.c
    ${WEB_C}
)
target_include_directories(stat PRIVATE src dirstat sysinfo)
target_link_libraries(stat PRIVATE Threads::Threads)
