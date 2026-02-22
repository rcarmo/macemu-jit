/*
 *  evdev_input.cpp - Direct /dev/input evdev fallback for KMSDRM
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include "sysdeps.h"

#if defined(__linux__)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>

#include "adb.h"

// Evdev state
static int evdev_mouse_fd = -1;
static bool evdev_initialized = false;
static bool evdev_enabled = false;
static int evdev_abs_x = 0;
static int evdev_abs_y = 0;
static bool evdev_abs_initialized = false;

// Debug logging
static bool evdev_debug_enabled(void)
{
	static bool initialized = false;
	static bool enabled = false;

	if (!initialized) {
		const char *env = getenv("B2_DEBUG_INPUT");
		if (env && *env && strcmp(env, "0") != 0 && strcasecmp(env, "false") != 0)
			enabled = true;
		initialized = true;
	}
	return enabled;
}

// Check if a device is a mouse
static bool is_mouse_device(int fd)
{
	unsigned long evbits = 0;
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0)
		return false;

	// Check for EV_REL (relative movement) capability
	if (!(evbits & (1 << EV_REL)))
		return false;

	// Check for REL_X and REL_Y
	unsigned long relbits = 0;
	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), &relbits) < 0)
		return false;

	return (relbits & (1 << REL_X)) && (relbits & (1 << REL_Y));
}

static const char *get_env_mouse_device(void)
{
	const char *path = getenv("B2_EVDEV_MOUSE");
	if (path && *path)
		return path;
	return NULL;
}

// Find and open a mouse device
static int find_mouse_device(void)
{
	if (const char *env_path = get_env_mouse_device()) {
		int fd = open(env_path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0) {
			if (is_mouse_device(fd))
				return fd;
			close(fd);
			printf("evdev: B2_EVDEV_MOUSE=\"%s\" is not a relative mouse device\n", env_path);
		} else {
			printf("evdev: Cannot open B2_EVDEV_MOUSE=\"%s\": %s\n", env_path, strerror(errno));
		}
	}

	DIR *dir = opendir("/dev/input");
	if (!dir) {
		if (evdev_debug_enabled())
			printf("evdev: Cannot open /dev/input: %s\n", strerror(errno));
		return -1;
	}

	struct dirent *entry;
	while ((entry = readdir(dir)) != NULL) {
		if (strncmp(entry->d_name, "event", 5) != 0)
			continue;

		char path[256];
		snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);

		int fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (is_mouse_device(fd)) {
			if (evdev_debug_enabled())
				printf("evdev: Found mouse at %s\n", path);
			closedir(dir);
			return fd;
		}

		close(fd);
	}

	closedir(dir);
	if (evdev_debug_enabled())
		printf("evdev: No mouse device found\n");
	return -1;
}

// Initialize evdev input
void evdev_input_init(void)
{
	if (evdev_initialized)
		return;

	evdev_initialized = true;
	evdev_mouse_fd = find_mouse_device();
	
	if (evdev_mouse_fd >= 0) {
		evdev_enabled = true;
		if (evdev_debug_enabled())
			printf("evdev: Input initialized successfully\n");
	}
}

// Shutdown evdev input
void evdev_input_shutdown(void)
{
	if (evdev_mouse_fd >= 0) {
		close(evdev_mouse_fd);
		evdev_mouse_fd = -1;
	}
	evdev_enabled = false;
	evdev_initialized = false;
}

// Enable evdev fallback (call when SDL isn't delivering events)
void evdev_input_enable(void)
{
	if (!evdev_initialized)
		evdev_input_init();
	evdev_enabled = true;
}

// Disable evdev fallback (call when SDL starts delivering events)
void evdev_input_disable(void)
{
	evdev_enabled = false;
}

// Check if evdev is active
bool evdev_input_active(void)
{
	return evdev_enabled && evdev_mouse_fd >= 0;
}

// Poll evdev for mouse events - returns true if any events were processed
bool evdev_poll_mouse(int *dx, int *dy, int *buttons_changed, int *button_state)
{
	if (!evdev_enabled || evdev_mouse_fd < 0) {
		*dx = *dy = 0;
		*buttons_changed = 0;
		*button_state = 0;
		return false;
	}

	struct input_event ev;
	int total_dx = 0, total_dy = 0;
	int btn_changed = 0;
	static int btn_state = 0;
	bool got_events = false;

	while (read(evdev_mouse_fd, &ev, sizeof(ev)) == sizeof(ev)) {
		got_events = true;

		if (ev.type == EV_REL) {
			if (ev.code == REL_X)
				total_dx += ev.value;
			else if (ev.code == REL_Y)
				total_dy += ev.value;
		} else if (ev.type == EV_KEY) {
			int button = -1;
			if (ev.code == BTN_LEFT)
				button = 0;
			else if (ev.code == BTN_RIGHT)
				button = 1;
			else if (ev.code == BTN_MIDDLE)
				button = 2;

			if (button >= 0) {
				int mask = 1 << button;
				if (ev.value) {
					// Button pressed
					if (!(btn_state & mask)) {
						btn_state |= mask;
						btn_changed |= mask;
					}
				} else {
					// Button released
					if (btn_state & mask) {
						btn_state &= ~mask;
						btn_changed |= mask;
					}
				}
			}
		}
	}

	*dx = total_dx;
	*dy = total_dy;
	*buttons_changed = btn_changed;
	*button_state = btn_state;

	if (got_events && evdev_debug_enabled() && (total_dx || total_dy || btn_changed)) {
		printf("evdev: dx=%d dy=%d btn_changed=0x%x btn_state=0x%x\n",
		       total_dx, total_dy, btn_changed, btn_state);
	}

	return got_events;
}

// Simple helper to feed evdev events directly to ADB
void evdev_process_mouse_to_adb(bool mouse_grabbed, int screen_width, int screen_height)
{
	int dx, dy, buttons_changed, button_state;
	
	if (!evdev_poll_mouse(&dx, &dy, &buttons_changed, &button_state))
		return;

	// Handle button changes
	if (buttons_changed & 1) {
		if (button_state & 1)
			ADBMouseDown(0);
		else
			ADBMouseUp(0);
	}
	if (buttons_changed & 2) {
		if (button_state & 2)
			ADBMouseDown(1);
		else
			ADBMouseUp(1);
	}
	if (buttons_changed & 4) {
		if (button_state & 4)
			ADBMouseDown(2);
		else
			ADBMouseUp(2);
	}

	// Handle movement
	if (dx || dy) {
		if (mouse_grabbed) {
			ADBMouseMoved(dx, dy);
		} else {
			if (!evdev_abs_initialized || screen_width <= 0 || screen_height <= 0) {
				evdev_abs_x = screen_width > 0 ? screen_width / 2 : 0;
				evdev_abs_y = screen_height > 0 ? screen_height / 2 : 0;
				evdev_abs_initialized = true;
			}
			evdev_abs_x += dx;
			evdev_abs_y += dy;
			if (screen_width > 0) {
				if (evdev_abs_x < 0) evdev_abs_x = 0;
				if (evdev_abs_x >= screen_width) evdev_abs_x = screen_width - 1;
			}
			if (screen_height > 0) {
				if (evdev_abs_y < 0) evdev_abs_y = 0;
				if (evdev_abs_y >= screen_height) evdev_abs_y = screen_height - 1;
			}
			ADBMouseMoved(evdev_abs_x, evdev_abs_y);
		}
	}
}

#else // !__linux__

// Stubs for non-Linux platforms
void evdev_input_init(void) {}
void evdev_input_shutdown(void) {}
void evdev_input_enable(void) {}
void evdev_input_disable(void) {}
bool evdev_input_active(void) { return false; }
bool evdev_poll_mouse(int *dx, int *dy, int *buttons_changed, int *button_state)
{
	*dx = *dy = 0;
	*buttons_changed = 0;
	*button_state = 0;
	return false;
}
void evdev_process_mouse_to_adb(bool mouse_grabbed, int screen_width, int screen_height) {}

#endif // __linux__
