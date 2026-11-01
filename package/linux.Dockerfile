FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl git file binutils pkg-config \
    autoconf automake libtool zlib1g-dev libudev-dev \
    libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev libgl1-mesa-dri \
    libopengl0 libxkbcommon-dev libxkbcommon-x11-0 libxcb-cursor0 \
    libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
    libxcb-shape0 libxcb-xinerama0 libxcb-xfixes0 libx11-xcb1 libxi6 \
    libxrender1 libxtst6 libxrandr2 libwayland-client0 libwayland-cursor0 libwayland-egl1 \
    libfontconfig1 fonts-dejavu-core libasound2t64 libpulse0 libnss3 libdbus-1-3 libglib2.0-0t64 \
    python3 python3-venv zip xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/aqt \
    && /opt/aqt/bin/pip install --no-cache-dir aqtinstall==3.3.0 \
    && /opt/aqt/bin/python -m aqt install-qt linux desktop 6.12.0 linux_gcc_64 \
       --outputdir /opt/Qt \
       --modules qt5compat qtcharts qtimageformats qtmultimedia qtserialport qtshadertools

ENV PATH="/opt/Qt/6.12.0/gcc_64/bin:${PATH}"
WORKDIR /work
USER ubuntu
CMD ["bash", "package/build-linux.sh"]
