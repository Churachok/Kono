#include "server.h"
#include <stdlib.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    struct wl2_server server;
    if (!server_init(&server)) {
        return EXIT_FAILURE;
    }
    server_run(&server);
    server_fini(&server);
    return EXIT_SUCCESS;
}

