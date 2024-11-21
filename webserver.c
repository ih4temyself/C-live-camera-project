// webserver.c

#include "webserver.h"
#include <microhttpd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "camera.h"

static ssize_t stream_callback(void *cls, uint64_t pos, char *buf, size_t max) {
    connection_info *conn_info = (connection_info *)cls;

    pthread_mutex_lock(&frame_mutex);

    while (current_frame.size == 0) {
        pthread_cond_wait(&frame_cond, &frame_mutex);
    }

    if (!conn_info->data) {
        const char *boundary_str = "--boundarydonotcross\r\n";
        const char *header = "Content-Type: image/jpeg\r\n\r\n";
        size_t total_size = strlen(boundary_str) + strlen(header) + current_frame.size;

        conn_info->data = malloc(total_size);
        if (!conn_info->data) {
            pthread_mutex_unlock(&frame_mutex);
            return MHD_NO;
        }

        memcpy(conn_info->data, boundary_str, strlen(boundary_str));
        memcpy(conn_info->data + strlen(boundary_str), header, strlen(header));
        memcpy(conn_info->data + strlen(boundary_str) + strlen(header), current_frame.data, current_frame.size);

        conn_info->size = total_size;
        conn_info->sent = 0;
    }

    size_t remaining = conn_info->size - conn_info->sent;
    size_t to_send = (max < remaining) ? max : remaining;
    memcpy(buf, conn_info->data + conn_info->sent, to_send);
    conn_info->sent += to_send;

    if (conn_info->sent >= conn_info->size) {
        free(conn_info->data);
        conn_info->data = NULL;
    }

    pthread_mutex_unlock(&frame_mutex);

    return to_send;
}

void request_completed_callback(void *cls, struct MHD_Connection *connection,
                                 void **con_cls, enum MHD_RequestTerminationCode toe) {
    connection_info *conn_info = *con_cls;
    if (conn_info) {
        free(conn_info->data);
        free(conn_info);
    }
}

enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                const char *url, const char *method,
                                const char *version, const char *upload_data,
                                size_t *upload_data_size, void **con_cls) {
    if (strcmp(method, "GET") != 0) {
        return MHD_NO;
    }

    if (strcmp(url, "/") == 0) {
        const char *page = "<!DOCTYPE html>"
                           "<html>"
                           "<head><title>MJPEG Stream</title></head>"
                           "<body>"
                           "<h1>Live Stream</h1>"
                           "<img src=\"/stream\" alt=\"MJPEG Stream\" />"
                           "</body>"
                           "</html>";

        struct MHD_Response *response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_PERSISTENT);
        if (!response)
            return MHD_NO;

        MHD_add_response_header(response, "Content-Type",         MHD_add_response_header(response, "Content-Type", "text/html");
        enum MHD_Result res = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return res;
    }

    if (strcmp(url, "/stream") == 0) {
        connection_info *conn_info = malloc(sizeof(connection_info));
        if (!conn_info) {
            return MHD_NO;
        }

        conn_info->data = NULL;
        conn_info->sent = 0;
        conn_info->size = 0;

        struct MHD_Response *response = MHD_create_response_from_callback(
            MHD_SIZE_UNKNOWN,  // Unknown size, stream dynamically
            1024,              // Max buffer size for streaming
            &stream_callback,  // Callback to stream MJPEG data
            conn_info,         // Pointer to our connection info
            &request_completed_callback  // Completion callback
        );

        if (!response) {
            free(conn_info);
            return MHD_NO;
        }

        MHD_add_response_header(response, "Content-Type", "multipart/x-mixed-replace; boundary=boundarydonotcross");
        enum MHD_Result res = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);

        return res;
    }

    return MHD_NO;
}

void start_http_server(int port) {
    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, port, NULL, NULL,
                                                 &request_handler, NULL, MHD_OPTION_END);
    if (daemon == NULL) {
        g_printerr("Failed to start HTTP server\n");
        exit(EXIT_FAILURE);
    }

    printf("HTTP Server started on port %d\n", port);
}

