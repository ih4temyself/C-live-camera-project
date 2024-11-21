#include "backend.h"
#include "webserver.h"

int main(int argc, char *argv[]) {
    init_pipeline();
    start_webserver();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    stop_webserver();
    cleanup_pipeline();

    return 0;
}
