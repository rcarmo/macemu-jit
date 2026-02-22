# BasiliskII SDL Docker Image (x86_64 native build)
# Optimized for framebuffer/KMS display without X11

FROM debian:bookworm-slim AS builder

ARG SDL_VERSION=2.32.8

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    autoconf \
    automake \
    libtool \
    pkg-config \
    libasound2-dev \
    libudev-dev \
    libdbus-1-dev \
    libsamplerate0-dev \
    libmpfr-dev \
    libgmp-dev \
    libvncserver-dev \
    libpng-dev \
    wget \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

# Build SDL2 from source with Raspberry Pi optimized settings
RUN wget -q https://www.libsdl.org/release/SDL2-${SDL_VERSION}.tar.gz \
    && tar -zxf SDL2-${SDL_VERSION}.tar.gz \
    && cd SDL2-${SDL_VERSION} \
    && ./configure \
        --prefix=/usr/local \
        --disable-video-opengl \
        --disable-video-x11 \
        --disable-pulseaudio \
        --disable-esd \
        --disable-video-wayland \
        --enable-libudev \
        --enable-input-events \
    && make -j$(nproc) \
    && make install

# Copy BasiliskII source
COPY BasiliskII /build/BasiliskII

# Build BasiliskII
WORKDIR /build/BasiliskII/src/Unix
ENV PKG_CONFIG_PATH=/usr/local/lib/pkgconfig
ENV CPPFLAGS="-I/usr/local/include -I/usr/local/include/SDL2"
ENV LDFLAGS="-L/usr/local/lib"
RUN NO_CONFIGURE=1 ./autogen.sh \
    && ./configure \
        --prefix=/usr \
        --enable-sdl-audio \
        --enable-sdl-video \
        --disable-vosf \
        --without-mon \
        --without-esd \
        --without-gtk \
        --with-vncserver \
        --disable-jit-compiler \
        --disable-nls \
    && make -j$(nproc)

# Runtime image
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    libasound2 \
    libsamplerate0 \
    libudev1 \
    libdbus-1-3 \
    libmpfr6 \
    libgmp10 \
    libvncserver1 \
    libpng16-16 \
    && rm -rf /var/lib/apt/lists/*

# Copy SDL2 libraries
COPY --from=builder /usr/local/lib/libSDL2* /usr/local/lib/
RUN ldconfig

# Copy BasiliskII binary and data files
COPY --from=builder /build/BasiliskII/src/Unix/BasiliskII /usr/bin/
COPY --from=builder /build/BasiliskII/src/Unix/keycodes /usr/share/basiliskii-sdl/
COPY --from=builder /build/BasiliskII/src/Unix/fbdevices /usr/share/basiliskii-sdl/
COPY --from=builder /build/BasiliskII/src/Unix/tunconfig /usr/share/basiliskii-sdl/

# Create non-root user (will run as root for device access, but prepared for rootless)
RUN useradd -m -s /bin/bash basilisk \
    && mkdir -p /home/basilisk/.basilisk_ii \
    && chown -R basilisk:basilisk /home/basilisk

# Volume for ROM, disk images, and preferences
VOLUME ["/data"]

# Environment variables
ENV SDL_VIDEODRIVER=kmsdrm
ENV SDL_AUDIODRIVER=alsa

WORKDIR /data

# Default command - can be overridden
ENTRYPOINT ["/usr/bin/BasiliskII"]
CMD ["--config", "/data/basiliskii_prefs"]
