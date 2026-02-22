#include "sysdeps.h"

#include "vnc_server.h"
#include "prefs.h"

#include <algorithm>
#include <vector>

#if SDL_VERSION_ATLEAST(2, 0, 0) && !SDL_VERSION_ATLEAST(3, 0, 0)

#ifdef HAVE_LIBVNCSERVER
extern "C" {
#include <rfb/rfb.h>
#include <rfb/keysym.h>
}
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

static inline Uint32 read_surface_pixel(SDL_Surface *surface, int x, int y)
{
	const int bytes_per_pixel = surface->format->BytesPerPixel;
	const uint8_t *row = static_cast<const uint8_t *>(surface->pixels) + y * surface->pitch;
	const uint8_t *p = row + x * bytes_per_pixel;

	switch (bytes_per_pixel) {
		case 1:
			return *p;
		case 2:
			return *reinterpret_cast<const uint16_t *>(p);
		case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
			return (p[0] << 16) | (p[1] << 8) | p[2];
#else
			return p[0] | (p[1] << 8) | (p[2] << 16);
#endif
		case 4:
			return *reinterpret_cast<const uint32_t *>(p);
		default:
			return 0;
	}
}

static void vnc_shutdown_server()
{
	if (!vnc_server)
		return;

	rfbShutdownServer(vnc_server, TRUE);
	rfbScreenCleanup(vnc_server);
	vnc_server = NULL;
	vnc_framebuffer.clear();
	vnc_width = 0;
	vnc_height = 0;
}

static bool vnc_ensure_server(int width, int height)
{
	if (!vnc_enabled)
		return false;

	if (vnc_server && vnc_width == width && vnc_height == height)
		return true;

	vnc_shutdown_server();

	if (width <= 0 || height <= 0)
		return false;

	vnc_width = width;
	vnc_height = height;
	vnc_framebuffer.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);

	char arg0[] = "BasiliskII";
	char *argv[] = { arg0 };
	int argc = 1;
	vnc_server = rfbGetScreen(&argc, argv, width, height, 8, 3, 4);
	if (!vnc_server) {
		printf("WARNING: Failed to initialize libvncserver instance\n");
		vnc_framebuffer.clear();
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

	if (!vnc_ensure_server(surface->w, surface->h))
		return;

	if (updated_rect.w <= 0 || updated_rect.h <= 0) {
		rfbProcessEvents(vnc_server, 0);
		return;
	}

	SDL_Rect clipped = updated_rect;
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
	if (clipped.w <= 0 || clipped.h <= 0) {
		rfbProcessEvents(vnc_server, 0);
		return;
	}

	const bool needs_lock = SDL_MUSTLOCK(surface) != 0;
	if (needs_lock && SDL_LockSurface(surface) != 0)
		return;

	for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
		for (int x = clipped.x; x < clipped.x + clipped.w; ++x) {
			Uint8 r, g, b;
			const Uint32 source_pixel = read_surface_pixel(surface, x, y);
			SDL_GetRGB(source_pixel, surface->format, &r, &g, &b);

			const size_t dest = (static_cast<size_t>(y) * static_cast<size_t>(vnc_width) + static_cast<size_t>(x)) * 4;
			vnc_framebuffer[dest + 0] = r;
			vnc_framebuffer[dest + 1] = g;
			vnc_framebuffer[dest + 2] = b;
			vnc_framebuffer[dest + 3] = 0xff;
		}
	}

	if (needs_lock)
		SDL_UnlockSurface(surface);

	rfbMarkRectAsModified(vnc_server, clipped.x, clipped.y, clipped.x + clipped.w, clipped.y + clipped.h);
	rfbProcessEvents(vnc_server, 0);
#else
	(void)surface;
	(void)updated_rect;
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

#endif
