FROM debian:11

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ant \
    autoconf \
    default-jdk-headless \
    default-jre-headless \
    libasound2-dev \
    automake \
    bash \
    binutils \
    bison \
    build-essential \
    ca-certificates \
    curl \
    desktop-file-utils \
    file \
    flex \
    gawk \
    gettext \
    git \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libbluray-bdj \
    libdbus-1-dev \
    libdrm-dev \
    libebml-dev \
    libegl1-mesa-dev \
    libfribidi-dev \
    libfreetype6-dev \
    libgl1-mesa-dev \
    libgles2-mesa-dev \
    libogg-dev \
    liblua5.2-dev \
    libmatroska-dev \
    libpulse-dev \
    libqt5svg5-dev \
    libqt5x11extras5-dev \
    libsamplerate0-dev \
    libsystemd-dev \
    libudfread-dev \
    libwayland-bin \
    libwayland-dev \
    libvorbis-dev \
    libx11-dev \
    libx11-xcb-dev \
    libxcb-composite0-dev \
    libxcb-icccm4-dev \
    libxcb-image0-dev \
    libxcb-keysyms1-dev \
    libxcb-randr0-dev \
    libxcb-render-util0-dev \
    libxcb-shape0-dev \
    libxcb-shm0-dev \
    libxcb-sync-dev \
    libxcb-util-dev \
    libxcb-xv0-dev \
    libxcb-xfixes0-dev \
    libxcb-xinerama0-dev \
    libxcb-xkb-dev \
    libxkbcommon-dev \
    libxkbcommon-x11-dev \
    libxml2-dev \
    libswresample-dev \
    libswscale-dev \
    libtool \
    lua5.2 \
    meson \
    ninja-build \
    patchelf \
    pkg-config \
    python3 \
    python3-pip \
    python3-setuptools \
    qtbase5-dev \
    qtbase5-private-dev \
    qtwayland5 \
    qttools5-dev \
    qttools5-dev-tools \
    rsync \
    squashfs-tools \
    wget \
    wayland-protocols \
    xz-utils \
 && rm -rf /var/lib/apt/lists/*

RUN python3 -m pip install --no-cache-dir "meson>=1.3.2,<2"

RUN mkdir -p \
    /opt/open3doled-appimage \
    /opt/open3doled-appimage/src \
    /opt/open3doled-appimage/build \
    /opt/open3doled-appimage/AppDir \
    /opt/open3doled-appimage/tools \
    /out \
    /work

WORKDIR /work

CMD ["/bin/bash"]
