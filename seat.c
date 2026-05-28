#include "server.h"
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

struct wl2_seat {
    struct wl2_server *server;
    struct wlr_seat *wlr_seat;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor_manager;
    struct wl_list link;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;
    struct wl_listener cursor_frame;
    struct wl_listener key_event;
    struct wl_listener modifiers;
};

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, modifiers);
    struct wlr_keyboard *keyboard = data;
    
    wlr_seat_set_keyboard(seat->wlr_seat, keyboard);
    wlr_seat_keyboard_notify_modifiers(seat->wlr_seat, 
                                        &keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, key_event);
    struct wlr_keyboard_key_event *event = data;
    struct wlr_keyboard *keyboard = seat->wlr_seat->keyboard_state.keyboard;
    
    if (!keyboard) return;

    // Получаем consumed модификаторы
    xkb_mod_mask_t consumed_mods = xkb_state_key_get_consumed_mods2(
        keyboard->xkb_state, event->keycode, XKB_CONSUMED_MODE_XKB);
    
    // Получаем символы (исправленный API для wlroots 0.19)
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->xkb_state, 
                                        event->keycode, &syms);
    
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard) & ~consumed_mods;
    
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && nsyms > 0) {
        if (handle_keybinding(syms[0], modifiers)) {
            return;
        }
    }
    
    // Передаем события клиентам
    wlr_seat_set_keyboard(seat->wlr_seat, keyboard);
    wlr_seat_keyboard_notify_key(seat->wlr_seat, event->time_msec, 
                                  event->keycode, event->state);
}

static void cursor_motion(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    wlr_cursor_move(seat->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    wlr_seat_pointer_notify_motion(seat->wlr_seat, event->time_msec, 
                                    seat->cursor->x, seat->cursor->y);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    wlr_cursor_warp_absolute(seat->cursor, &event->pointer->base, event->x, event->y);
    wlr_seat_pointer_notify_motion(seat->wlr_seat, event->time_msec, 
                                    seat->cursor->x, seat->cursor->y);
}

static void cursor_button(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, cursor_button);
    struct wlr_pointer_button_event *event = data;
    wlr_seat_pointer_notify_button(seat->wlr_seat, event->time_msec, 
                                    event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    wlr_seat_pointer_notify_axis(seat->wlr_seat, event->time_msec, 
                                  event->orientation, event->delta, 
                                  event->delta_discrete, event->source,
                                  WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct wl2_seat *seat = wl_container_of(listener, seat, cursor_frame);
    wlr_seat_pointer_notify_frame(seat->wlr_seat);
}

static void seat_handle_new_input(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
    case WLR_INPUT_DEVICE_POINTER: {
        struct wlr_pointer *pointer = wlr_pointer_from_input_device(device);
        wlr_cursor_attach_input_device(seat->cursor, device);
        
        seat->cursor_motion.notify = cursor_motion;
        wl_signal_add(&pointer->events.motion, &seat->cursor_motion);
        
        seat->cursor_motion_absolute.notify = cursor_motion_absolute;
        wl_signal_add(&pointer->events.motion_absolute, &seat->cursor_motion_absolute);
        
        seat->cursor_button.notify = cursor_button;
        wl_signal_add(&pointer->events.button, &seat->cursor_button);
        
        seat->cursor_axis.notify = cursor_axis;
        wl_signal_add(&pointer->events.axis, &seat->cursor_axis);
        
        seat->cursor_frame.notify = cursor_frame;
        wl_signal_add(&pointer->events.frame, &seat->cursor_frame);
        
        wlr_log(WLR_INFO, "Pointer device connected");
        break;
    }
    case WLR_INPUT_DEVICE_KEYBOARD: {
        struct wlr_keyboard *kb = wlr_keyboard_from_input_device(device);
        
        // Используем дефолтную раскладку через wlr_keyboard
        struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!ctx) {
            wlr_log(WLR_ERROR, "Failed to create XKB context");
            break;
        }
        
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, 
                                     XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) {
            wlr_log(WLR_ERROR, "Failed to create keymap");
            xkb_context_unref(ctx);
            break;
        }
        
        wlr_keyboard_set_keymap(kb, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(ctx);
        
        seat->key_event.notify = keyboard_handle_key;
        wl_signal_add(&kb->events.key, &seat->key_event);
        
        seat->modifiers.notify = keyboard_handle_modifiers;
        wl_signal_add(&kb->events.modifiers, &seat->modifiers);
        
        wlr_keyboard_set_repeat_info(kb, 25, 600);
        wlr_seat_set_keyboard(seat->wlr_seat, kb);
        
        wlr_log(WLR_INFO, "Keyboard device connected");
        break;
    }
    default:
        break;
    }
    
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(seat->wlr_seat, caps);
}

static void seat_handle_request_cursor(struct wl_listener *listener, void *data) {
    struct wl2_seat *seat = wl_container_of(listener, seat, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;
    struct wlr_seat_client *focused_client = seat->wlr_seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(seat->cursor, event->surface, 
                               event->hotspot_x, event->hotspot_y);
    }
}

void seat_init(struct wl2_server *server, const char *name) {
    struct wl2_seat *seat = calloc(1, sizeof(*seat));
    if (!seat) return;
    
    seat->server = server;
    seat->wlr_seat = wlr_seat_create(server->wl_display, name);
    if (!seat->wlr_seat) {
        free(seat);
        return;
    }

    seat->cursor = wlr_cursor_create();
    if (!seat->cursor) {
        wlr_seat_destroy(seat->wlr_seat);
        free(seat);
        return;
    }

    wlr_cursor_attach_output_layout(seat->cursor, server->output_layout);

    seat->xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
    if (seat->xcursor_manager) {
        wlr_xcursor_manager_load(seat->xcursor_manager, 1);
        wlr_cursor_set_xcursor(seat->cursor, seat->xcursor_manager, "left_ptr");
    }

    seat->new_input.notify = seat_handle_new_input;
    wl_signal_add(&server->backend->events.new_input, &seat->new_input);
    
    seat->request_cursor.notify = seat_handle_request_cursor;
    wl_signal_add(&seat->wlr_seat->events.request_set_cursor, &seat->request_cursor);

    wl_list_insert(&server->seats, &seat->link);
}

void seat_fini_all(struct wl2_server *server) {
    struct wl2_seat *seat, *tmp;
    wl_list_for_each_safe(seat, tmp, &server->seats, link) {
        wl_list_remove(&seat->link);
        
        if (seat->xcursor_manager) {
            wlr_xcursor_manager_destroy(seat->xcursor_manager);
        }
        if (seat->cursor) {
            wlr_cursor_destroy(seat->cursor);
        }
        if (seat->wlr_seat) {
            wlr_seat_destroy(seat->wlr_seat);
        }
        free(seat);
    }
}