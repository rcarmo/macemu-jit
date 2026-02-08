# BasiliskII Blitter Tests

This folder contains lightweight host-side checks for the video blitters. They do not require an emulator build or ROMs.

## Blit_RGB565_OBO Test

Validates the RGB555 (Mac big-endian) to RGB565 (host little-endian) conversion path used by `Blit_RGB565_OBO`.

### Build

From the repository root:

```bash
clang++ -std=c++17 -O2 \
  -I BasiliskII/tests \
  -I BasiliskII/src/include \
  -I BasiliskII/src/CrossPlatform \
  BasiliskII/tests/test_blit_rgb565_obo.cpp \
  -o BasiliskII/tests/test_blit_rgb565_obo
```

### Run

```bash
./BasiliskII/tests/test_blit_rgb565_obo
```

Expected output:

```
OK: Blit_RGB565_OBO 8 vectors
```

### Notes

- The test uses a minimal `sysdeps.h` stub in this folder to avoid `config.h`.
- This validates the math for the RGB555->RGB565 conversion, but does not replace hardware testing.
