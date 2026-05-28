#include "server.h"
#include <wlr/backend/session.h>
#include <wlr/util/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

static int tty_fd = -1;
static struct wl_event_source *tty_source = NULL;

static int handle_tty_input(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct wl2_server *server = data;
    
    if (mask & WL_EVENT_HANGUP) {
        wlr_log(WLR_INFO, "TTY hangup, terminating");
        wl_display_terminate(server->wl_display);
        return 0;
    }
    
    return 0;
}

bool setup_tty(struct wl2_server *server) {
    const char *tty_path = ttyname(STDIN_FILENO);
    if (!tty_path) {
        tty_path = getenv("TTY");
    }
    if (!tty_path) {
        wlr_log(WLR_ERROR, "No TTY found");
        return false;
    }
    
    wlr_log(WLR_INFO, "Using TTY: %s", tty_path);
    
    tty_fd = open(tty_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty_fd < 0) {
        wlr_log(WLR_ERROR, "Failed to open TTY: %s", strerror(errno));
        return false;
    }
    
    struct wl_event_loop *event_loop = wl_display_get_event_loop(server->wl_display);
    tty_source = wl_event_loop_add_fd(event_loop, tty_fd, 
                                       WL_EVENT_READABLE | WL_EVENT_HANGUP,
                                       handle_tty_input, server);
    if (!tty_source) {
        close(tty_fd);
        return false;
    }
    
    return true;
}

void cleanup_tty(void) {
    if (tty_source) {
        wl_event_source_remove(tty_source);
        tty_source = NULL;
    }
    if (tty_fd >= 0) {
        close(tty_fd);
        tty_fd = -1;
    }
}