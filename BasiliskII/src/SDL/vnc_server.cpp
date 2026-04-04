#include "sysdeps.h"

#include "vnc_server.h"
#include "prefs.h"

#include <algorithm>
#include <atomic>
#include <vector>
#include <cstring>

#if SDL_VERSION_ATLEAST(2, 0, 0) && !SDL_VERSION_ATLEAST(3, 0, 0)

#ifdef HAVE_LIBVNCSERVER
extern "C" {
#include <rfb/rfb.h>
#include <rfb/keysym.h>
}
#include <SDL_thread.h>
#endif

namespace {

static bool vnc_enabled = false;
static int vnc_port = 5900;
static bool vnc_warned_unavailable = false;

#ifdef HAVE_LIBVNCSERVER
static rfbScreenInfoPtr vnc_server = NULL;
static std::vector<uint8_t> vnc_framebuffer;
static int vnc_width = 0;
static int vnc_height = 0;
static int vnc_pointer_x = 0;
static int vnc_pointer_y = 0;
static int vnc_pointer_buttons = 0;
static SDL_Keymod vnc_mod_state = KMOD_NONE;

// --- Background thread state ---
static SDL_Thread *vnc_thread = NULL;
static SDL_mutex *vnc_mutex = NULL;		// Protects pending_* fields
static SDL_cond *vnc_cond = NULL;		// Signals new frame to VNC thread
static std::atomic<bool> vnc_thread_quit(false);

// Double-buffered surface snapshot for the VNC thread
static std::vector<uint8_t> vnc_snapshot;		// Raw pixel copy of host_surface
static int vnc_snapshot_pitch = 0;
static int vnc_snapshot_bpp = 0;
static Uint32 vnc_snapshot_rmask = 0;
static Uint32 vnc_snapshot_gmask = 0;
static Uint32 vnc_snapshot_bmask = 0;
static SDL_Rect vnc_pending_rect = {0, 0, 0, 0};
static bool vnc_pending_frame = false;

static SDL_Keycode vnc_keysym_to_sdl(rfbKeySym key)
{
	if (key >= 'A' && key <= 'Z')
		return static_cast<SDL_Keycode>(key + ('a' - 'A'));
	if (key >= 0x20 && key <= 0x7e)
		return static_cast<SDL_Keycode>(key);

	switch (key) {
		case XK_Return: return SDLK_RETURN;
		case XK_BackSpace: return SDLK_BACKSPACE;
		case XK_Tab: return SDLK_TAB;
		case XK_Escape: return SDLK_ESCAPE;
		case XK_Delete: return SDLK_DELETE;
		case XK_Home: return SDLK_HOME;
		case XK_End: return SDLK_END;
		case XK_Page_Up: return SDLK_PAGEUP;
		case XK_Page_Down: return SDLK_PAGEDOWN;
		case XK_Left: return SDLK_LEFT;
		case XK_Right: return SDLK_RIGHT;
		case XK_Up: return SDLK_UP;
		case XK_Down: return SDLK_DOWN;
		case XK_F1: return SDLK_F1;
		case XK_F2: return SDLK_F2;
		case XK_F3: return SDLK_F3;
		case XK_F4: return SDLK_F4;
		case XK_F5: return SDLK_F5;
		case XK_F6: return SDLK_F6;
		case XK_F7: return SDLK_F7;
		case XK_F8: return SDLK_F8;
		case XK_F9: return SDLK_F9;
		case XK_F10: return SDLK_F10;
		case XK_F11: return SDLK_F11;
		case XK_F12: return SDLK_F12;
		case XK_Shift_L: return SDLK_LSHIFT;
		case XK_Shift_R: return SDLK_RSHIFT;
		case XK_Control_L: return SDLK_LCTRL;
		case XK_Control_R: return SDLK_RCTRL;
		case XK_Alt_L: return SDLK_LALT;
		case XK_Alt_R: return SDLK_RALT;
		case XK_Meta_L: return SDLK_LGUI;
		case XK_Meta_R: return SDLK_RGUI;
		case XK_Super_L: return SDLK_LGUI;
		case XK_Super_R: return SDLK_RGUI;
		case XK_Caps_Lock: return SDLK_CAPSLOCK;
		case XK_Num_Lock: return SDLK_NUMLOCKCLEAR;
		case XK_KP_0: return SDLK_KP_0;
		case XK_KP_1: return SDLK_KP_1;
		case XK_KP_2: return SDLK_KP_2;
		case XK_KP_3: return SDLK_KP_3;
		case XK_KP_4: return SDLK_KP_4;
		case XK_KP_5: return SDLK_KP_5;
		case XK_KP_6: return SDLK_KP_6;
		case XK_KP_7: return SDLK_KP_7;
		case XK_KP_8: return SDLK_KP_8;
		case XK_KP_9: return SDLK_KP_9;
		case XK_KP_Decimal: return SDLK_KP_PERIOD;
		case XK_KP_Add: return SDLK_KP_PLUS;
		case XK_KP_Subtract: return SDLK_KP_MINUS;
		case XK_KP_Multiply: return SDLK_KP_MULTIPLY;
		case XK_KP_Divide: return SDLK_KP_DIVIDE;
		case XK_KP_Enter: return SDLK_KP_ENTER;
		case XK_KP_Equal: return SDLK_KP_EQUALS;
		default: return SDLK_UNKNOWN;
	}
}

static void vnc_update_mod_state(SDL_Keycode key, bool down)
{
	const SDL_Keymod bit =
		(key == SDLK_LSHIFT || key == SDLK_RSHIFT) ? KMOD_SHIFT :
		(key == SDLK_LCTRL || key == SDLK_RCTRL) ? KMOD_CTRL :
		(key == SDLK_LALT || key == SDLK_RALT) ? KMOD_ALT :
		(key == SDLK_LGUI || key == SDLK_RGUI) ? KMOD_GUI :
		KMOD_NONE;

	if (bit == KMOD_NONE)
		return;

	if (down)
		vnc_mod_state = static_cast<SDL_Keymod>(vnc_mod_state | bit);
	else
		vnc_mod_state = static_cast<SDL_Keymod>(vnc_mod_state & ~bit);
}

static void vnc_push_key_event(bool down, SDL_Keycode key)
{
	if (key == SDLK_UNKNOWN)
		return;

	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.sym = key;
	ev.key.keysym.mod = static_cast<Uint16>(vnc_mod_state);
	SDL_PushEvent(&ev);
}

static void vnc_push_pointer_button(Uint8 sdl_button, bool down, int x, int y)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.button.button = sdl_button;
	ev.button.x = x;
	ev.button.y = y;
	SDL_PushEvent(&ev);
}

static void vnc_push_pointer_motion(int x, int y)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_MOUSEMOTION;
	ev.motion.state = 0;
	ev.motion.x = x;
	ev.motion.y = y;
	ev.motion.xrel = x - vnc_pointer_x;
	ev.motion.yrel = y - vnc_pointer_y;
	SDL_PushEvent(&ev);
	vnc_pointer_x = x;
	vnc_pointer_y = y;
}

static void vnc_push_wheel(int y)
{
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = SDL_MOUSEWHEEL;
	ev.wheel.x = 0;
	ev.wheel.y = y;
	SDL_PushEvent(&ev);
}

static void vnc_keyboard_callback(rfbBool down, rfbKeySym key, rfbClientPtr)
{
	const SDL_Keycode sdl_key = vnc_keysym_to_sdl(key);
	vnc_update_mod_state(sdl_key, down != 0);
	vnc_push_key_event(down != 0, sdl_key);
}

static void vnc_pointer_callback(int button_mask, int x, int y, rfbClientPtr)
{
	/* VNC sends coordinates in framebuffer space (Mac logical resolution).
	   SDL_PushEvent expects window-space coordinates, which SDL then maps
	   back to logical space via SDL_RenderSetLogicalSize. When the SDL
	   window is larger than the logical size, this double-mapping causes
	   a coordinate scaling error. Compensate by converting VNC coords
	   from logical to window space. */
	SDL_Window *win = SDL_GetWindowFromID(1);
	if (win && vnc_width > 0 && vnc_height > 0) {
		int ww, wh;
		SDL_GetWindowSize(win, &ww, &wh);
		x = x * ww / vnc_width;
		y = y * wh / vnc_height;
	}
	vnc_push_pointer_motion(x, y);

	const int previous = vnc_pointer_buttons;
	vnc_pointer_buttons = button_mask;

	const int transitions[] = {1, 2, 4};
	const Uint8 mapped[] = {SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT};
	for (size_t i = 0; i < 3; ++i) {
		const bool was_down = (previous & transitions[i]) != 0;
		const bool now_down = (button_mask & transitions[i]) != 0;
		if (was_down != now_down)
			vnc_push_pointer_button(mapped[i], now_down, x, y);
	}

	if ((button_mask & 8) && !(previous & 8))
		vnc_push_wheel(1);
	if ((button_mask & 16) && !(previous & 16))
		vnc_push_wheel(-1);
}

static inline Uint32 read_surface_pixel(const uint8_t *pixels, int pitch, int bpp, int x, int y)
{
	const uint8_t *p = pixels + y * pitch + x * bpp;

	switch (bpp) {
		case 1:
			return *p;
		case 2: {
			uint16_t v;
			memcpy(&v, p, sizeof(v));
			return v;
		}
		case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
			return (p[0] << 16) | (p[1] << 8) | p[2];
#else
			return p[0] | (p[1] << 8) | (p[2] << 16);
#endif
		case 4: {
			uint32_t v;
			memcpy(&v, p, sizeof(v));
			return v;
		}
		default:
			return 0;
	}
}

static inline void decode_mask(Uint32 mask, Uint8 &shift, Uint8 &bits)
{
	shift = 0;
	bits = 0;
	if (!mask)
		return;

	Uint32 m = mask;
	while (!(m & 1)) {
		m >>= 1;
		shift++;
	}
	while (m & 1) {
		m >>= 1;
		bits++;
	}
}

static inline Uint8 scale_component_to_u8(Uint32 value, Uint8 bits)
{
	if (bits == 0)
		return 0;
	if (bits >= 8)
		return static_cast<Uint8>(value >> (bits - 8));

	const Uint32 maxv = (1u << bits) - 1u;
	return static_cast<Uint8>((value * 255u + (maxv / 2u)) / maxv);
}

// Convert a snapshot region to RGBA32 in vnc_framebuffer.
// Called on the VNC background thread — no SDL surface access, only our snapshot buffer.
static void vnc_convert_region(const SDL_Rect &rect, int bpp, int pitch,
							   Uint32 rmask, Uint32 gmask, Uint32 bmask,
							   const uint8_t *src_pixels)
{
	// Fast path: source is ARGB8888 or XRGB8888 with standard masks
	// (the VNC framebuffer wants R, G, B, 0xFF byte order)
	const bool is_argb8888 = (bpp == 4 &&
		((rmask == 0x00FF0000 && gmask == 0x0000FF00 && bmask == 0x000000FF) ||
		 (rmask == 0x000000FF && gmask == 0x0000FF00 && bmask == 0x00FF0000)));

	if (is_argb8888 && rmask == 0x00FF0000) {
		// Source is XRGB8888 (or ARGB8888): bytes are B, G, R, A in little-endian memory
		// We need R, G, B, 0xFF
		for (int y = rect.y; y < rect.y + rect.h; ++y) {
			const uint32_t *src_row = reinterpret_cast<const uint32_t *>(src_pixels + y * pitch) + rect.x;
			uint8_t *dst_row = vnc_framebuffer.data() + (static_cast<size_t>(y) * vnc_width + rect.x) * 4;
			for (int x = 0; x < rect.w; ++x) {
				const uint32_t px = src_row[x];
				dst_row[0] = (px >> 16) & 0xFF;	// R
				dst_row[1] = (px >> 8) & 0xFF;		// G
				dst_row[2] = px & 0xFF;				// B
				dst_row[3] = 0xFF;
				dst_row += 4;
			}
		}
	} else if (is_argb8888 && rmask == 0x000000FF) {
		// Source is XBGR8888 (or ABGR8888): bytes are R, G, B, A in little-endian memory
		for (int y = rect.y; y < rect.y + rect.h; ++y) {
			const uint32_t *src_row = reinterpret_cast<const uint32_t *>(src_pixels + y * pitch) + rect.x;
			uint8_t *dst_row = vnc_framebuffer.data() + (static_cast<size_t>(y) * vnc_width + rect.x) * 4;
			for (int x = 0; x < rect.w; ++x) {
				const uint32_t px = src_row[x];
				dst_row[0] = px & 0xFF;				// R
				dst_row[1] = (px >> 8) & 0xFF;		// G
				dst_row[2] = (px >> 16) & 0xFF;		// B
				dst_row[3] = 0xFF;
				dst_row += 4;
			}
		}
	} else {
		// Generic slow path for non-32bpp or unusual masks
		Uint8 rshift = 0, gshift = 0, bshift = 0;
		Uint8 rbits = 0, gbits = 0, bbits = 0;
		decode_mask(rmask, rshift, rbits);
		decode_mask(gmask, gshift, gbits);
		decode_mask(bmask, bshift, bbits);

		for (int y = rect.y; y < rect.y + rect.h; ++y) {
			for (int x = rect.x; x < rect.x + rect.w; ++x) {
				const Uint32 pixel = read_surface_pixel(src_pixels, pitch, bpp, x, y);
				const Uint32 rv = (rmask != 0) ? ((pixel & rmask) >> rshift) : 0;
				const Uint32 gv = (gmask != 0) ? ((pixel & gmask) >> gshift) : 0;
				const Uint32 bv = (bmask != 0) ? ((pixel & bmask) >> bshift) : 0;
				const Uint8 r = scale_component_to_u8(rv, rbits);
				const Uint8 g = scale_component_to_u8(gv, gbits);
				const Uint8 b = scale_component_to_u8(bv, bbits);

				const size_t dest = (static_cast<size_t>(y) * vnc_width + x) * 4;
				vnc_framebuffer[dest + 0] = r;
				vnc_framebuffer[dest + 1] = g;
				vnc_framebuffer[dest + 2] = b;
				vnc_framebuffer[dest + 3] = 0xFF;
			}
		}
	}
}

// Background VNC thread: waits for new frames and processes them
static int vnc_thread_func(void *)
{
	while (!vnc_thread_quit.load(std::memory_order_acquire)) {
		bool have_frame = false;
		SDL_Rect rect = {0, 0, 0, 0};
		int bpp = 0;
		int pitch = 0;
		Uint32 rmask = 0;
		Uint32 gmask = 0;
		Uint32 bmask = 0;

		SDL_LockMutex(vnc_mutex);
		if (!vnc_pending_frame && !vnc_thread_quit.load(std::memory_order_relaxed))
			SDL_CondWaitTimeout(vnc_cond, vnc_mutex, 50);  // Wake every 50ms for rfbProcessEvents

		if (vnc_thread_quit.load(std::memory_order_relaxed)) {
			SDL_UnlockMutex(vnc_mutex);
			break;
		}

		if (vnc_pending_frame) {
			have_frame = true;
			rect = vnc_pending_rect;
			bpp = vnc_snapshot_bpp;
			pitch = vnc_snapshot_pitch;
			rmask = vnc_snapshot_rmask;
			gmask = vnc_snapshot_gmask;
			bmask = vnc_snapshot_bmask;
			vnc_pending_frame = false;

			if (rect.w > 0 && rect.h > 0) {
				vnc_convert_region(rect, bpp, pitch, rmask, gmask, bmask, vnc_snapshot.data());
				have_frame = true;
			}
		}
		SDL_UnlockMutex(vnc_mutex);

		if (have_frame) {
			rfbMarkRectAsModified(vnc_server, rect.x, rect.y, rect.x + rect.w, rect.y + rect.h);
		}

		// Process VNC protocol events (client connections, encoding, etc.)
		rfbProcessEvents(vnc_server, 0);
	}
	return 0;
}

static void vnc_shutdown_server()
{
	if (vnc_thread) {
		vnc_thread_quit.store(true, std::memory_order_release);
		if (vnc_cond) SDL_CondSignal(vnc_cond);
		SDL_WaitThread(vnc_thread, NULL);
		vnc_thread = NULL;
	}
	if (vnc_cond) { SDL_DestroyCond(vnc_cond); vnc_cond = NULL; }
	if (vnc_mutex) { SDL_DestroyMutex(vnc_mutex); vnc_mutex = NULL; }

	if (!vnc_server)
		return;

	rfbShutdownServer(vnc_server, TRUE);
	rfbScreenCleanup(vnc_server);
	vnc_server = NULL;
	vnc_framebuffer.clear();
	vnc_snapshot.clear();
	vnc_width = 0;
	vnc_height = 0;
}

static bool vnc_ensure_server(int width, int height, int bpp, int pitch)
{
	if (!vnc_enabled)
		return false;

	if (vnc_server && vnc_width == width && vnc_height == height &&
		vnc_snapshot_bpp == bpp && vnc_snapshot_pitch == pitch)
		return true;

	vnc_shutdown_server();

	if (width <= 0 || height <= 0)
		return false;

	vnc_width = width;
	vnc_height = height;
	vnc_snapshot_bpp = bpp;
	vnc_snapshot_pitch = pitch;
	vnc_framebuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
	vnc_snapshot.assign(static_cast<size_t>(height) * static_cast<size_t>(pitch), 0);

	char arg0[] = "BasiliskII";
	char *argv[] = { arg0 };
	int argc = 1;
	vnc_server = rfbGetScreen(&argc, argv, width, height, 8, 3, 4);
	if (!vnc_server) {
		printf("WARNING: Failed to initialize libvncserver instance\n");
		vnc_framebuffer.clear();
		vnc_snapshot.clear();
		vnc_width = 0;
		vnc_height = 0;
		return false;
	}

	vnc_server->desktopName = const_cast<char *>("BasiliskII");
	vnc_server->alwaysShared = TRUE;
	vnc_server->autoPort = FALSE;
	vnc_server->port = vnc_port;
	vnc_server->frameBuffer = reinterpret_cast<char *>(vnc_framebuffer.data());
	vnc_server->kbdAddEvent = vnc_keyboard_callback;
	vnc_server->ptrAddEvent = vnc_pointer_callback;
	vnc_pointer_x = 0;
	vnc_pointer_y = 0;
	vnc_pointer_buttons = 0;
	vnc_mod_state = KMOD_NONE;

	rfbInitServer(vnc_server);
	printf("VNC server enabled on port %d (%dx%d)\n", vnc_port, width, height);

	// Start background VNC thread
	vnc_mutex = SDL_CreateMutex();
	vnc_cond = SDL_CreateCond();
	vnc_thread_quit.store(false, std::memory_order_release);
	vnc_pending_frame = false;
	if (vnc_mutex && vnc_cond) {
		vnc_thread = SDL_CreateThread(vnc_thread_func, "VNCThread", NULL);
		if (!vnc_thread)
			printf("WARNING: Failed to create VNC background thread, using synchronous VNC updates\n");
	} else {
		printf("WARNING: Failed to initialize VNC thread synchronization, using synchronous VNC updates\n");
		if (vnc_cond) { SDL_DestroyCond(vnc_cond); vnc_cond = NULL; }
		if (vnc_mutex) { SDL_DestroyMutex(vnc_mutex); vnc_mutex = NULL; }
	}

	return true;
}
#endif

}

void VNCServerInitFromPrefs()
{
	vnc_enabled = PrefsFindBool("vncserver");
	const int configured_port = PrefsFindInt32("vncport");
	if (configured_port > 0 && configured_port <= 65535)
		vnc_port = configured_port;
	else
		vnc_port = 5900;

#ifndef HAVE_LIBVNCSERVER
	if (vnc_enabled && !vnc_warned_unavailable) {
		printf("WARNING: vncserver=true but this build was made without libvncserver support\n");
		vnc_warned_unavailable = true;
	}
#endif
}

void VNCServerShutdown()
{
#ifdef HAVE_LIBVNCSERVER
	vnc_shutdown_server();
#endif
}

void VNCServerUpdate(SDL_Surface *surface, const SDL_Rect &updated_rect)
{
#ifdef HAVE_LIBVNCSERVER
	if (!vnc_enabled || !surface)
		return;

	if (!vnc_ensure_server(surface->w, surface->h,
						   surface->format->BytesPerPixel, surface->pitch))
		return;

	if (updated_rect.w <= 0 || updated_rect.h <= 0)
		return;

	// Use full surface for VNC snapshot instead of partial dirty rect.
	// The host_surface is correct (verified via PNG dump), but partial
	// dirty rects leave stale palette-converted pixels in the VNC
	// framebuffer for regions drawn once (scrollbar backgrounds etc).
	SDL_Rect clipped = { 0, 0, surface->w, surface->h };
	if (clipped.x < 0) {
		clipped.w += clipped.x;
		clipped.x = 0;
	}
	if (clipped.y < 0) {
		clipped.h += clipped.y;
		clipped.y = 0;
	}
	clipped.w = std::min(clipped.w, surface->w - clipped.x);
	clipped.h = std::min(clipped.h, surface->h - clipped.y);
	if (clipped.w <= 0 || clipped.h <= 0)
		return;

	// Snapshot the dirty region of the surface into our private buffer.
	// This is a fast memcpy per scanline — much cheaper than per-pixel conversion.
	const bool needs_lock = SDL_MUSTLOCK(surface) != 0;
	if (needs_lock && SDL_LockSurface(surface) != 0)
		return;

	const int bpp = surface->format->BytesPerPixel;
	if (!vnc_thread || !vnc_mutex || !vnc_cond) {
		const int row_bytes = clipped.w * bpp;
		for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
			const uint8_t *src = static_cast<const uint8_t *>(surface->pixels) + y * surface->pitch + clipped.x * bpp;
			uint8_t *dst = vnc_snapshot.data() + y * surface->pitch + clipped.x * bpp;
			memcpy(dst, src, row_bytes);
		}

		if (needs_lock)
			SDL_UnlockSurface(surface);

		vnc_convert_region(clipped, bpp, surface->pitch,
					   surface->format->Rmask,
					   surface->format->Gmask,
					   surface->format->Bmask,
					   vnc_snapshot.data());
		rfbMarkRectAsModified(vnc_server, clipped.x, clipped.y, clipped.x + clipped.w, clipped.y + clipped.h);
		rfbProcessEvents(vnc_server, 0);
		return;
	}

	// Snapshot and signal the VNC background thread under the same mutex.
	const int row_bytes = clipped.w * bpp;
	SDL_LockMutex(vnc_mutex);
	for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
		const uint8_t *src = static_cast<const uint8_t *>(surface->pixels) + y * surface->pitch + clipped.x * bpp;
		uint8_t *dst = vnc_snapshot.data() + y * surface->pitch + clipped.x * bpp;
		memcpy(dst, src, row_bytes);
	}

	if (needs_lock)
		SDL_UnlockSurface(surface);

	vnc_snapshot_bpp = bpp;
	vnc_snapshot_pitch = surface->pitch;
	vnc_snapshot_rmask = surface->format->Rmask;
	vnc_snapshot_gmask = surface->format->Gmask;
	vnc_snapshot_bmask = surface->format->Bmask;
	// Merge with any existing pending rect
	if (vnc_pending_frame && vnc_pending_rect.w > 0 && vnc_pending_rect.h > 0) {
		int x1 = std::min((int)vnc_pending_rect.x, (int)clipped.x);
		int y1 = std::min((int)vnc_pending_rect.y, (int)clipped.y);
		int x2 = std::max(vnc_pending_rect.x + vnc_pending_rect.w, clipped.x + clipped.w);
		int y2 = std::max(vnc_pending_rect.y + vnc_pending_rect.h, clipped.y + clipped.h);
		vnc_pending_rect.x = x1;
		vnc_pending_rect.y = y1;
		vnc_pending_rect.w = x2 - x1;
		vnc_pending_rect.h = y2 - y1;
	} else {
		vnc_pending_rect = clipped;
	}
	vnc_pending_frame = true;
	SDL_CondSignal(vnc_cond);
	SDL_UnlockMutex(vnc_mutex);
#else
	(void)surface;
	(void)updated_rect;
#endif
}

void VNCServerProcessEvents()
{
#ifdef HAVE_LIBVNCSERVER
	// No-op: VNC events are now processed by the background thread
#endif
}

#else

void VNCServerInitFromPrefs()
{
}

void VNCServerShutdown()
{
}

void VNCServerUpdate(SDL_Surface *surface, const SDL_Rect &updated_rect)
{
	(void)surface;
	(void)updated_rect;
}

void VNCServerProcessEvents()
{
}

#endif
