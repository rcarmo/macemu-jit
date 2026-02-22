/*
 *  evdev_input.h - Direct /dev/input evdev fallback for KMSDRM
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
 */

#ifndef EVDEV_INPUT_H
#define EVDEV_INPUT_H

// Initialize evdev input subsystem
extern void evdev_input_init(void);

// Shutdown evdev input
extern void evdev_input_shutdown(void);

// Enable evdev fallback (when SDL isn't delivering events)
extern void evdev_input_enable(void);

// Disable evdev fallback (when SDL is working)
extern void evdev_input_disable(void);

// Check if evdev is active
extern bool evdev_input_active(void);

// Poll evdev for mouse events
// Returns true if any events were read
extern bool evdev_poll_mouse(int *dx, int *dy, int *buttons_changed, int *button_state);

// Process evdev mouse events and send to ADB
extern void evdev_process_mouse_to_adb(bool mouse_grabbed, int screen_width, int screen_height);

#endif // EVDEV_INPUT_H
