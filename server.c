#include "server.h"
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>

void server_handle_new_output(struct wl_listener *listener, void *data);
void seat_init(struct wl2_server *server, const char *name);
void seat_fini_all(struct wl2_server *server);

static struct wl2_server *global_server = NULL;

static void handle_sigint(int sig) {
    wlr_log(WLR_INFO, "Received signal %d, terminating", sig);
    if (global_server && global_server->wl_display) {
        wl_display_terminate(global_server->wl_display);
    }
}

struct wl2_xdg_surface {
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
    struct wlr_xdg_surface *xdg_surface;
    struct wlr_scene_tree *scene_tree;
};

static void xdg_surface_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct wl2_xdg_surface *surface = wl_container_of(listener, surface, map);
    wlr_log(WLR_INFO, "Window opened: %s", 
            surface->xdg_surface->toplevel->app_id);
}

static void xdg_surface_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct wl2_xdg_surface *surface = wl_container_of(listener, surface, unmap);
    wlr_log(WLR_INFO, "Window unmapped: %s", 
            surface->xdg_surface->toplevel->app_id);
}

static void xdg_surface_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct wl2_xdg_surface *surface = wl_container_of(listener, surface, commit);
    if (surface->xdg_surface->initialized) {
        uint32_t width = surface->xdg_surface->surface->current.width;
        uint32_t height = surface->xdg_surface->surface->current.height;
        wlr_log(WLR_DEBUG, "Window commit: %dx%d", width, height);
    }
}

static void xdg_surface_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct wl2_xdg_surface *surface = wl_container_of(listener, surface, destroy);
    
    wlr_log(WLR_INFO, "Window destroyed");
    
    // Destroy scene tree if exists
    if (surface->scene_tree) {
        wlr_scene_node_destroy(&surface->scene_tree->node);
    }
    
    wl_list_remove(&surface->map.link);
    wl_list_remove(&surface->unmap.link);
    wl_list_remove(&surface->commit.link);
    wl_list_remove(&surface->destroy.link);
    free(surface);
}

void server_handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct wl2_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;
    
    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        return;
    }
    
    struct wl2_xdg_surface *surface = calloc(1, sizeof(*surface));
    if (!surface) return;
    
    surface->xdg_surface = xdg_surface;
    
    // Создаем scene tree для окна
    if (server->scene) {
        surface->scene_tree = wlr_scene_tree_create(&server->scene->tree);
        if (surface->scene_tree) {
            wlr_scene_node_lower_to_bottom(&surface->scene_tree->node);
        }
    }
    
    surface->map.notify = xdg_surface_map;
    wl_signal_add(&xdg_surface->surface->events.map, &surface->map);
    
    surface->unmap.notify = xdg_surface_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &surface->unmap);
    
    surface->commit.notify = xdg_surface_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &surface->commit);
    
    surface->destroy.notify = xdg_surface_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &surface->destroy);
}

bool server_init(struct wl2_server *server) {
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);
    
    wlr_log_init(WLR_DEBUG, NULL);
    global_server = server;
    
    // Настройка WAYLAND_DISPLAY
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir) {
        runtime_dir = "/tmp/wayland-runtime";
        setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
        mkdir(runtime_dir, 0700);
        wlr_log(WLR_INFO, "XDG_RUNTIME_DIR set to %s", runtime_dir);
    }
    
    server->wl_display = wl_display_create();
    if (!server->wl_display) {
        wlr_log(WLR_ERROR, "Failed to create Wayland display");
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
    
    // Создаем бэкенд
    wlr_log(WLR_INFO, "Creating backend...");
    server->backend = wlr_backend_autocreate(event_loop, NULL);
    
    if (!server->backend) {
        wlr_log(WLR_ERROR, "Failed to create backend");
        wl_display_destroy(server->wl_display);
        return false;
    }
    
    wlr_log(WLR_INFO, "Backend created successfully");
    
    // Создаём рендерер
    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        wlr_backend_destroy(server->backend);
        wl_display_destroy(server->wl_display);
        return false;
    }
    
    wlr_renderer_init_wl_display(server->renderer, server->wl_display);
    
    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wlr_log(WLR_ERROR, "Failed to create allocator");
        wlr_renderer_destroy(server->renderer);
        wlr_backend_destroy(server->backend);
        wl_display_destroy(server->wl_display);
        return false;
    }
    
    // Создаем output layout
    server->output_layout = wlr_output_layout_create(server->wl_display);
    if (!server->output_layout) {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        return false;
    }
    
    // Создаем общую сцену
    server->scene = wlr_scene_create();
    if (!server->scene) {
        wlr_log(WLR_ERROR, "Failed to create scene");
        return false;
    }
    
    // Привязываем сцену к output layout
    wlr_scene_attach_output_layout(server->scene, server->output_layout);
    
    server->compositor = wlr_compositor_create(server->wl_display, 6, server->renderer);
    if (!server->compositor) {
        wlr_log(WLR_ERROR, "Failed to create compositor");
        return false;
    }
    
    // В wlroots 0.19 создаем xdg_shell с версией 3 или выше
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 3);
    if (!server->xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create XDG shell");
        return false;
    }
    
    wl_list_init(&server->outputs);
    wl_list_init(&server->seats);
    
    server->new_output.notify = server_handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);
    
    server->new_xdg_surface.notify = server_handle_new_xdg_surface;
    wl_signal_add(&server->xdg_shell->events.new_surface, &server->new_xdg_surface);
    
    seat_init(server, "seat0");
    
    // Создаём сокет
    const char *socket_name = NULL;
    server->socket_name = socket_name;
    
    wlr_log(WLR_INFO, "Creating Wayland socket...");
    
    if (wl_display_add_socket(server->wl_display, "wayland-0") == 0) {
        socket_name = "wayland-0";
    } else {
        for (int i = 1; i <= 32; i++) {
            char name[32];
            snprintf(name, sizeof(name), "wayland-%d", i);
            if (wl_display_add_socket(server->wl_display, name) == 0) {
                socket_name = strdup(name);
                break;
            }
        }
    }
    
    if (!socket_name) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket");
        return false;
    }
    setenv("WAYLAND_DISPLAY", socket_name, 1);
    
    // Загрузка конфига
    const char *home = getenv("HOME");
    if (!home) home = getenv("home");
    if (home) {
        char config_path[512];
        snprintf(config_path, sizeof(config_path), "%s/.config/kono/kono.conf", home);
        load_kono_config(config_path);
    }
    
    // Запускаем бэкенд
    if (!wlr_backend_start(server->backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        return false;
    }
    
    wlr_log(WLR_INFO, "Backend started successfully");
    return true;
}

void server_fini(struct wl2_server *server) {
    wlr_log(WLR_INFO, "Shutting down...");
    
    seat_fini_all(server);
    wl_display_destroy_clients(server->wl_display);
    
    if (server->output_layout) {
        wlr_output_layout_destroy(server->output_layout);
    }
    if (server->scene) {
        wlr_scene_node_destroy(&server->scene->tree.node);
    }
    if (server->allocator) {
        wlr_allocator_destroy(server->allocator);
    }
    if (server->renderer) {
        wlr_renderer_destroy(server->renderer);
    }
    if (server->backend) {
        wlr_backend_destroy(server->backend);
    }
    if (server->wl_display) {
        wl_display_destroy(server->wl_display);
    }
}

void server_run(struct wl2_server *server) {
    wlr_log(WLR_INFO, "Starting main loop...");
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", getenv("WAYLAND_DISPLAY"));
    fflush(stdout);
    wl_display_run(server->wl_display);
    wlr_log(WLR_INFO, "Main loop ended");
}