#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "camera.h"
#include "webserver.h"

#define DEFAULT_FPS 30
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_QUALITY 80
#define SERVER_PORT 8080

void *camera_thread_func(void *arg) {
    init_camera_pipeline();
    return NULL;
}

int main() {
    pthread_t camera_thread;

    camera_caps = malloc(sizeof(CameraCapabilities));
    if (!camera_caps) {
        perror("Failed to allocate memory for camera capabilities");
        exit(EXIT_FAILURE);
    }

    pthread_create(&camera_thread, NULL, camera_thread_func, NULL);

    start_http_server(SERVER_PORT);

    pthread_join(camera_thread, NULL);

    cleanup_camera_resources();
    return 0;
}
