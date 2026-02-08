#include "sysdeps.h"

#include <vector>

// Pull in the blitter implementation so we can call the static symbol.
#include "../src/CrossPlatform/video_blit.cpp"

static uint16 rgb555_to_rgb565(uint16 rgb555_be)
{
	uint16 r = (rgb555_be >> 10) & 0x1f;
	uint16 g = (rgb555_be >> 5) & 0x1f;
	uint16 b = rgb555_be & 0x1f;
	uint16 g6 = (uint16)((g << 1) | (g >> 4));
	return (uint16)((r << 11) | (g6 << 5) | b);
}

int main(void)
{
	const uint16 vectors[] = {
		0x0000, // black
		0x7fff, // white
		0x7c00, // red max
		0x03e0, // green max
		0x001f, // blue max
		0x4210, // mid gray
		0x1ce7, // mid green/blue mix
		0x56b5  // mixed color
	};
	const size_t count = sizeof(vectors) / sizeof(vectors[0]);
	std::vector<uint8> src(count * 2);
	std::vector<uint8> dst(count * 2, 0);

	for (size_t i = 0; i < count; ++i) {
		uint16 be = vectors[i];
		src[i * 2] = (uint8)((be >> 8) & 0xff);
		src[i * 2 + 1] = (uint8)(be & 0xff);
	}

	Blit_RGB565_OBO(dst.data(), src.data(), (uint32)(count * 2));

	const uint16 *dst_words = reinterpret_cast<const uint16 *>(dst.data());
	for (size_t i = 0; i < count; ++i) {
		uint16 expected = rgb555_to_rgb565(vectors[i]);
		uint16 actual = dst_words[i];
		if (actual != expected) {
			std::fprintf(stderr,
				"Mismatch at %zu: src=0x%04x expected=0x%04x actual=0x%04x\n",
				i, vectors[i], expected, actual);
			return 1;
		}
	}

	std::printf("OK: Blit_RGB565_OBO %zu vectors\n", count);
	return 0;
}
