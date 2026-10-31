FROM mcr.azure.cn/devcontainers/base:trixie

ENV DEBIAN_FRONTEND=noninteractive

# Keep downloaded .deb files so the BuildKit apt cache mounts are reused across rebuilds
RUN rm -f /etc/apt/apt.conf.d/docker-clean && \
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/keep-cache

# C toolchain, debugging/analysis tools and common libraries for a system monitor
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        gcc \
        clang \
        clangd \
        clang-format \
        clang-tidy \
        cmake \
        ninja-build \
        make \
        pkg-config \
        gdb \
        valgrind \
        strace \
        ltrace \
        cppcheck \
        bear \
        ccache \
        libncurses-dev \
        procps \
        man-db \
        manpages-dev \
        gcc-mingw-w64-x86-64-posix \
        mingw-w64-x86-64-dev \
        binutils-mingw-w64-x86-64 \
        file \
        lintian

# Use ccache transparently for gcc/cc/clang
ENV PATH=/usr/lib/ccache:${PATH} \
    CCACHE_DIR=/home/vscode/.ccache

COPY no-proxy-host.sh /etc/profile.d/20-no-proxy-host.sh
RUN echo '. /etc/profile.d/20-no-proxy-host.sh' >> /etc/bash.bashrc

RUN mkdir -p /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/.ccache && \
    chown -R vscode:vscode /home/vscode/.vscode-server /home/vscode/.claude /home/vscode/.ccache
