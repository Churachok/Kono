#include "server.h"
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <stdlib.h>

struct wl2_output {
    struct wlr_output *wlr_output;
    struct wl2_server *server;
    struct wlr_scene_rect *background;

    struct wl_list link;
    struct wl_listener destroy;
    struct wl_listener frame;
};

static void output_frame_notify(struct wl_listener *listener, void *data) {
    (void)data;
    
    struct wl2_output *output = wl_container_of(listener, output, frame);
    struct wlr_output *wlr_output = output->wlr_output;
    struct wlr_scene *scene = output->server->scene;
    
    if (!scene) {
        wlr_log(WLR_ERROR, "No scene available");
        return;
    }

    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, wlr_output);
    if (!scene_output) {
        wlr_log(WLR_ERROR, "No scene output found for %s", wlr_output->name);
        return;
    }
    
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
    if (pass) {
        wlr_scene_output_commit(scene_output, NULL);
        wlr_render_pass_submit(pass);
    }
    
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
}

static void output_destroy_notify(struct wl_listener *listener, void *data) {
    (void)data;
    
    struct wl2_output *output = wl_container_of(listener, output, destroy);
    wlr_log(WLR_INFO, "Output %s destroyed", output->wlr_output->name);
    
    wl_list_remove(&output->link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->frame.link);
    free(output);
}

void server_handle_new_output(struct wl_listener *listener, void *data) {
    struct wl2_server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_log(WLR_INFO, "New output detected: %s", wlr_output->name);

    // Проверяем, что сцена создана
    if (!server->scene) {
        wlr_log(WLR_ERROR, "Scene not initialized");
        return;
    }

    // Инициализация рендера вывода
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);
    
    // Добавляем вывод в layout и получаем его позицию
    wlr_output_layout_add_auto(server->output_layout, wlr_output);
    
    // Получаем позицию вывода из layout
    struct wlr_output_layout_output *layout_output = 
        wlr_output_layout_get(server->output_layout, wlr_output);
    
    int output_lx = 0, output_ly = 0;
    if (layout_output) {
        output_lx = layout_output->x;
        output_ly = layout_output->y;
    }
    
    struct wl2_output *output = calloc(1, sizeof(*output));
    if (!output) return;
    
    output->wlr_output = wlr_output;
    output->server = server;
    
    // Создаем фон для этого вывода
    float bg_color[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    output->background = wlr_scene_rect_create(
        &server->scene->tree, 
        wlr_output->width, 
        wlr_output->height, 
        bg_color
    );
    
    if (output->background) {
        wlr_scene_node_set_position(&output->background->node, output_lx, output_ly);
        wlr_scene_node_lower_to_bottom(&output->background->node);
    }

    // Создаем scene output для привязки вывода к сцене
    struct wlr_scene_output *scene_output = wlr_scene_output_create(
        server->scene, wlr_output
    );
    
    if (!scene_output) {
        wlr_log(WLR_ERROR, "Failed to create scene output for %s", wlr_output->name);
        if (output->background) {
            wlr_scene_node_destroy(&output->background->node);
        }
        free(output);
        return;
    }

    // Настройка режима вывода
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    
    if (!wl_list_empty(&wlr_output->modes)) {
        struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
        if (mode) {
            wlr_output_state_set_mode(&state, mode);
        }
    }
    
    wlr_output_state_set_enabled(&state, true);
    
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "Failed to commit initial output state for %s", 
                wlr_output->name);
        wlr_output_state_finish(&state);
        wlr_scene_output_destroy(scene_output);
        if (output->background) {
            wlr_scene_node_destroy(&output->background->node);
        }
        free(output);
        return;
    }
    
    wlr_output_state_finish(&state);

    output->destroy.notify = output_destroy_notify;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    output->frame.notify = output_frame_notify;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    wl_list_insert(&server->outputs, &output->link);
    
    wlr_log(WLR_INFO, "Output %s ready (%dx%d at %d,%d)", 
            wlr_output->name, wlr_output->width, wlr_output->height,
            output_lx, output_ly);
}