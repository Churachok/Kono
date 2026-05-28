#ifndef SERVER_H
#define SERVER_H

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>

// Forward declaration для scene
struct wlr_scene;

struct wl2_server {
    struct wl_display *wl_display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;

    struct wl_list outputs;
    struct wl_list seats;

    struct wl_listener new_output;
    struct wl_listener new_xdg_surface;
    
    const char *socket_name;
};

bool server_init(struct wl2_server *server);
void server_fini(struct wl2_server *server);
void server_run(struct wl2_server *server);
void spawn_rofi(void);
bool load_kono_config(const char *config_path);
bool handle_keybinding(xkb_keysym_t sym, uint32_t mods);

#endif