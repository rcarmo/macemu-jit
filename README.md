# macemu - Macintosh Emulators

## Raspberry Pi Builds

This fork provides experimental pre-built packages and Docker images optimized for Raspberry Pi, using SDL2 with framebuffer/KMS display (no X11 or desktop environment required).

### Pre-built .deb Packages

Download from [GitHub Releases](https://github.com/rcarmo/macemu/releases) or build from source.

#### Install from release:
```bash
wget https://github.com/rcarmo/macemu/releases/latest/download/basiliskii-sdl_<version>_arm64.deb
sudo dpkg -i basiliskii-sdl_<version>_arm64.deb
```

#### Build from source on Raspberry Pi:
```bash
# Install dependencies
sudo apt-get install build-essential autoconf automake wget

# Build SDL2 (optimized for Pi, no X11/Wayland)
wget https://www.libsdl.org/release/SDL2-2.32.8.tar.gz
tar -zxvf SDL2-2.32.8.tar.gz
cd SDL2-2.32.8
./configure --disable-video-opengl --disable-video-x11 --disable-pulseaudio --disable-esd --disable-video-wayland
make -j4 && sudo make install
cd ..

# Build BasiliskII
cd macemu/BasiliskII/src/Unix
NO_CONFIGURE=1 ./autogen.sh
./configure --enable-sdl-audio --enable-sdl-framework --enable-sdl-video \
            --disable-vosf --without-mon --without-esd --without-gtk \
            --disable-jit-compiler --disable-nls
CPATH=$CPATH:/usr/local/include/SDL2 make -j4
sudo make install

# Optional: enable VNC output (requires libvncserver-dev)
./configure --with-vncserver
```

### Docker Container

A Docker image is available for running BasiliskII in a privileged container with direct hardware access.

#### Quick start:
```bash
cd BasiliskII/docker
mkdir -p data
cp /path/to/mac.rom data/rom
cp /path/to/disk.img data/hd.img
cp data/basiliskii_prefs.example data/basiliskii_prefs
# Edit data/basiliskii_prefs as needed

docker compose up -d
```

#### Pull pre-built image:
```bash
docker pull ghcr.io/rcarmo/basiliskii-sdl:latest
```

The container requires privileged mode for access to:
- `/dev/fb0` - Framebuffer
- `/dev/dri` - KMS/DRM video
- `/dev/input` - Keyboard/mouse
- `/dev/snd` - Audio

See [BasiliskII/docker/README.md](BasiliskII/docker/README.md) for detailed configuration options.

### Optional VNC framebuffer streaming (SDL2)

Add these preferences to your BasiliskII prefs file:

```text
vncserver true
vncport 5900
```

Then connect with any VNC client to the host IP on the configured port.

You can also control this per run via emulator CLI options:

```bash
BasiliskII --vnc --vnc-port 5901
BasiliskII --no-vnc
```

Supported runtime options:

- `--vnc [true|false]`
- `--no-vnc`
- `--vnc-port PORT`

### PNG framebuffer dumps (SDL2, Unix)

For screen corruption debugging, you can trigger a PNG dump of the current SDL framebuffer:

```bash
kill -USR2 <basiliskii-pid>
```

The emulator writes a PNG file to `/tmp` by default and logs the path.

Optional environment variable:

- `B2_DUMP_DIR=/path/to/output` - override the output directory for PNG dumps.

This feature requires building with libpng available.

### New configure CLI option

The Unix build now supports:

- `--with-vncserver` - enable libvncserver integration
- `--without-vncserver` - force-disable VNC integration

Example:

```bash
cd BasiliskII/src/Unix
./configure --enable-sdl-video --enable-sdl-audio --with-vncserver
```

### GitHub Actions CI

This repository includes automated builds:
- **`.github/workflows/build-deb-rpi.yml`** - Builds `.deb` packages for ARM64 and ARMhf
- **`.github/workflows/docker-rpi.yml`** - Builds and pushes Docker images to GHCR

Packages are automatically uploaded to GitHub Releases when a version tag is pushed.

---

## Supported Platforms

#### BasiliskII
```
macOS     x86_64 JIT / arm64 non-JIT
Linux x86 x86_64 JIT / arm64 non-JIT
MinGW x86        JIT
```
#### SheepShaver
```
macOS     x86_64 JIT / arm64 non-JIT
Linux x86 x86_64 JIT / arm64 non-JIT
MinGW x86        JIT
```

---

## How To Build

These builds need SDL2.0.14+ framework/library installed.

https://www.libsdl.org

### BasiliskII

#### macOS
preparation:

Download gmp-6.2.1.tar.xz from https://gmplib.org.
```
$ cd ~/Downloads
$ tar xf gmp-6.2.1.tar.xz
$ cd gmp-6.2.1
$ ./configure --disable-shared
$ make
$ make check
$ sudo make install
```
Download mpfr-4.2.0.tar.xz from https://www.mpfr.org.
```
$ cd ~/Downloads
$ tar xf mpfr-4.2.0.tar.xz
$ cd mpfr-4.2.0
$ ./configure --disable-shared
$ make
$ make check
$ sudo make install
```
On an Intel Mac, the libraries should be cross-built.  
Change the `configure` command for both GMP and MPFR as follows, and ignore the `make check` command:
```
$ CFLAGS="-arch arm64" CXXFLAGS="$CFLAGS" ./configure -host=aarch64-apple-darwin --disable-shared 
```
(from https://github.com/kanjitalk755/macemu/pull/96)

about changing Deployment Target:  
If you build with an older version of Xcode, you can change Deployment Target to the minimum it supports or 10.7, whichever is greater.

build:
```
$ cd macemu/BasiliskII/src/MacOSX
$ xcodebuild build -project BasiliskII.xcodeproj -configuration Release
```
or same as Linux

#### Linux
preparation (arm64 only): Install GMP and MPFR.
```
$ cd macemu/BasiliskII/src/Unix
$ ./autogen.sh
$ make
```

#### MinGW32/MSYS2
preparation:
```
$ pacman -S base-devel mingw-w64-i686-toolchain autoconf automake mingw-w64-i686-SDL2
```
note: MinGW32 dropped GTK2 package.
See msys2/MINGW-packages#24490

build (from a mingw32.exe prompt):
```
$ cd macemu/BasiliskII/src/Windows
$ ../Unix/autogen.sh
$ make
```

### SheepShaver

#### macOS
about changing Deployment Target: see BasiliskII
```
$ cd macemu/SheepShaver/src/MacOSX
$ xcodebuild build -project SheepShaver_Xcode8.xcodeproj -configuration Release
```
or same as Linux

#### Linux
```
$ cd macemu/SheepShaver/src/Unix
$ ./autogen.sh
$ make
```
For Raspberry Pi:
https://github.com/vaccinemedia/macemu

#### MinGW32/MSYS2
preparation: same as BasiliskII  
  
build (from a mingw32.exe prompt):
```
$ cd macemu/SheepShaver
$ make links
$ cd src/Windows
$ ../Unix/autogen.sh
$ make
```

---

## Recommended key bindings for GNOME
https://github.com/kanjitalk755/macemu/blob/master/SheepShaver/doc/Linux/gnome_keybindings.txt

(from https://github.com/kanjitalk755/macemu/issues/59)
