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

# Fully static Linux binary (build with CC=musl-gcc): runs on any distribution,
# including old ones such as Ubuntu 18.04, because it needs no system libraries.
option(STATE_STATIC "Link the Linux binary statically" OFF)
if(STATE_STATIC AND NOT WIN32)
    target_link_options(state PRIVATE -static)
endif()

if(WIN32)
    # Windows 10 APIs (GPU counters); static so state.exe needs no MinGW DLLs.
    target_compile_definitions(state PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00 UNICODE _UNICODE)
    target_link_libraries(state PRIVATE ws2_32 pdh psapi dxgi dxguid shell32 advapi32)
    target_link_options(state PRIVATE -static)
endif()

# ---------- install + Debian package (cpack -G DEB) ----------
include(GNUInstallDirs)
install(TARGETS state RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

if(UNIX)
    install(FILES packaging/state.service DESTINATION lib/systemd/system)
    install(FILES packaging/debian/copyright DESTINATION ${CMAKE_INSTALL_DOCDIR})
    if(STATE_STATIC)
        install(FILES packaging/debian/lintian-overrides
                DESTINATION ${CMAKE_INSTALL_DATADIR}/lintian/overrides RENAME state)
    endif()

    # Debian versions: v0.0.1-alpha.1 -> 0.0.1~alpha.1 ('~' sorts before the final 0.0.1)
    string(REGEX REPLACE "^v" "" deb_version "${version}")
    if(NOT deb_version MATCHES "^[0-9]")
        set(deb_version "0.0.0~${deb_version}")
    endif()
    string(REPLACE "-" "~" deb_version "${deb_version}")

    # Debian expects a gzipped changelog and man page.
    set(pkg_dir ${CMAKE_BINARY_DIR}/packaging)
    file(MAKE_DIRECTORY ${pkg_dir})
    string(TIMESTAMP changelog_date "%a, %d %b %Y %H:%M:%S +0000" UTC)
    file(WRITE ${pkg_dir}/changelog
        "state (${deb_version}) unstable; urgency=low\n\n"
        "  * Release ${version}, see https://github.com/wildwind123/state/releases\n\n"
        " -- wildwind123 <wildwind123@users.noreply.github.com>  ${changelog_date}\n")
    # gzip -9n: maximum compression, no timestamp (reproducible)
    execute_process(COMMAND gzip -9n -c ${pkg_dir}/changelog OUTPUT_FILE ${pkg_dir}/changelog.gz)
    execute_process(COMMAND gzip -9n -c ${CMAKE_SOURCE_DIR}/packaging/state.1 OUTPUT_FILE ${pkg_dir}/state.1.gz)
    install(FILES ${pkg_dir}/changelog.gz DESTINATION ${CMAKE_INSTALL_DOCDIR})
    install(FILES ${pkg_dir}/state.1.gz DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGE_NAME state)
    set(CPACK_PACKAGE_VERSION ${deb_version})
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "System monitor: CPU, memory, disks, GPU and disk-usage treemap")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Shows CPU, memory, disk free space and GPU usage in the terminal and in a
web UI, and finds which folders and files take up disk space (like
WinDirStat), with cached scans and a JSON API.")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "wildwind123 <wildwind123@users.noreply.github.com>")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/wildwind123/state")
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    if(STATE_STATIC)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF) # a static binary depends on nothing
    else()
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    endif()
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        ${CMAKE_SOURCE_DIR}/packaging/debian/postinst
        ${CMAKE_SOURCE_DIR}/packaging/debian/prerm
        ${CMAKE_SOURCE_DIR}/packaging/debian/postrm)
    set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
    set(CPACK_DEBIAN_FILE_NAME "state-${version}-amd64.deb")
    set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
    set(CPACK_STRIP_FILES ON)
    include(CPack)
endif()
