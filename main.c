#include "backend.h"
#include "webserver.h"

/**
 * @brief main entry point for this app.
 * 
 * initializes the pipeline and starts the web server. It then enters the main loop where it processes frames and accepts requests for the web server
 * 
 * @param argc number of command-line arguments.
 * @param argv array of command-line arguments.
 * 
 * @return returns 0 if completion is successful.
 */

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
