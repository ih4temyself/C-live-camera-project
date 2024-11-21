#include "webserver.h"
#include "backend.h"
#include <microhttpd.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>

#define PORT 8080
#define STREAM_PATH "/stream"

typedef struct {
    uint64_t frame_version; /** version of the frame sent to the client (at the moment)*/
    size_t sent; /** amount of data sent to the client so far. */    
} connection_info;

static struct MHD_Daemon *mhd_daemon = NULL;

/**
 * @brief callback function to stream the MJPEG data to the client.
 *
 * @param cls connection information.
 * @param pos current position in the stream.
 * @param buf buffer to store the data.
 * @param max maximum number of bytes to send.
 *
 * @return number of bytes sent, or -1 if error.
 */

static ssize_t stream_callback(void *cls, uint64_t pos, char *buf, size_t max) {
    connection_info *conn_info = (connection_info *)cls;

    pthread_mutex_lock(&frame_mutex);

    while (conn_info->frame_version == current_frame.version) {
        pthread_cond_wait(&frame_cond, &frame_mutex);
    }

    conn_info->frame_version = current_frame.version;
    conn_info->sent = 0;

    size_t remaining = current_frame.size - conn_info->sent;
    size_t to_send = (max < remaining) ? max : remaining;

    memcpy(buf, current_frame.data + conn_info->sent, to_send);

    conn_info->sent += to_send;

    pthread_mutex_unlock(&frame_mutex);

    return to_send;
}

/**
 * @brief callback function to handle completed requests.
 *
 * @param cls connection information.
 * @param connection MHD connection object.
 * @param con_cls connection's custom object.
 * @param toe termination code.
 */

static void request_completed_callback(void *cls, struct MHD_Connection *connection,
                                       void **con_cls, enum MHD_RequestTerminationCode toe) {
    connection_info *conn_info = *con_cls;
    if (conn_info) {
        free(conn_info);
    }
}

/**
 * @brief parses the query string and returns the value associated with the specified key.
 *
 * @param data query string.
 * @param key key to search for.
 *
 * @return dynamically allocated string containing the value, or NULL (if not found).
 */

char *get_parameter_value(const char *data, const char *key) {
    char *key_pos = strstr(data, key);
    if (!key_pos)
        return NULL;
    key_pos += strlen(key);
    if (*key_pos != '=')
        return NULL;
    key_pos++;
    char *end_pos = strchr(key_pos, '&');
    size_t len = end_pos ? (size_t)(end_pos - key_pos) : strlen(key_pos);
    char *value = malloc(len + 1);
    if (!value)
        return NULL;
    strncpy(value, key_pos, len);
    value[len] = '\0';
    return value;
}

/**
 * @brief handles incoming HTTP requests, so both GET and POST methods.
 *
 * @param cls custom data passed to the handler.
 * @param connection MHD connection object.
 * @param url URL requested.
 * @param method HTTP (GET, POST).
 * @param version HTTP version.
 * @param upload_data data uploaded by the client.
 * @param upload_data_size size of the uploaded data.
 * @param con_cls pointer to a custom connection object.
 *
 * @return result code indicating success or failure.
 */

static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                       const char *url, const char *method,
                                       const char *version, const char *upload_data,
                                       size_t *upload_data_size, void **con_cls) {
    if (strcmp(method, "GET") == 0) {
        if (strcmp(url, STREAM_PATH) == 0) {
            if (!*con_cls) {
                connection_info *conn_info = malloc(sizeof(connection_info));
                if (!conn_info) {
                    return MHD_NO;
                }
                conn_info->frame_version = current_frame.version - 1; 
                conn_info->sent = 0;
                *con_cls = conn_info;
            }

            struct MHD_Response *response = MHD_create_response_from_callback(
                MHD_SIZE_UNKNOWN, 65536, stream_callback, *con_cls, NULL);

            if (!response) {
                return MHD_NO;
            }

            MHD_add_response_header(response, "Content-Type", "multipart/x-mixed-replace; boundary=boundarydonotcross");
            MHD_add_response_header(response, "Cache-Control", "no-cache");
            MHD_add_response_header(response, "Connection", "close");

            int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);

            return ret;
        } else if (strcmp(url, "/") == 0) {
            char page[4096];
            snprintf(page, sizeof(page),
                     "<!DOCTYPE html>"
                     "<html>"
                     "<head>"
                     "<title>MJPEG Stream</title>"
                     "<style>"
                     "body { font-family: Arial, sans-serif; }"
                     "</style>"
                     "<script>"
                     "function reloadStream() {"
                     "    var img = document.getElementById('stream');"
                     "    img.onerror = function() {"
                     "        setTimeout(function() {"
                     "            img.src = '/stream?rand=' + Math.random();"
                     "        }, 1000);"
                     "    };"
                     "}"
                     "window.onload = function() {"
                     "    reloadStream();"
                     "};"
                     "</script>"
                     "</head>"
                     "<body>"
                     "<h1>Live Stream</h1>"
                     "<img id=\"stream\" src=\"/stream\" alt=\"MJPEG Stream\" onerror=\"reloadStream()\" />"
                     "<h2>Adjust Settings</h2>"
                     "<form method=\"POST\" action=\"/settings\">"
                     "FPS: <input type=\"number\" name=\"fps\" value=\"%d\"><br>"
                     "Quality: <input type=\"number\" name=\"quality\" value=\"%d\"><br>"
                     "Resolution Width: <input type=\"number\" name=\"width\" value=\"%d\"><br>"
                     "Resolution Height: <input type=\"number\" name=\"height\" value=\"%d\"><br>"
                     "<input type=\"submit\" value=\"Apply\">"
                     "</form>"
                     "</body>"
                     "</html>",
                     fps, quality, width, height);

            struct MHD_Response *response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_MUST_COPY);
            if (!response)
                return MHD_NO;

            MHD_add_response_header(response, "Content-Type", "text/html");
            int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        } else {
            return MHD_NO;
        }
    } else if (strcmp(method, "POST") == 0 && strcmp(url, "/settings") == 0) {
        if (!*con_cls) {
            *con_cls = calloc(1, sizeof(char *));
            return MHD_YES;
        }

        if (*upload_data_size != 0) {
            char **post_data = (char **)con_cls;
            size_t new_size = (*post_data ? strlen(*post_data) : 0) + *upload_data_size + 1;
            *post_data = realloc(*post_data, new_size);
            if (*post_data == NULL) {
                return MHD_NO;
            }
            strncat(*post_data, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        } else {
            char **post_data = (char **)con_cls;
            // Parse the POST data
            char *fps_value = get_parameter_value(*post_data, "fps");
            char *quality_value = get_parameter_value(*post_data, "quality");
            char *width_value = get_parameter_value(*post_data, "width");
            char *height_value = get_parameter_value(*post_data, "height");

            if (fps_value) {
                int new_fps = atoi(fps_value);
                if (new_fps > 0 && new_fps != fps) {
                    fps = new_fps;
                }
                free(fps_value);
            }
            if (quality_value) {
                int new_quality = atoi(quality_value);
                if (new_quality > 0 && new_quality != quality) {
                    quality = new_quality;
                }
                free(quality_value);
            }
            if (width_value) {
                int new_width = atoi(width_value);
                if (new_width > 0 && new_width != width) {
                    width = new_width;
                }
                free(width_value);
            }
            if (height_value) {
                int new_height = atoi(height_value);
                if (new_height > 0 && new_height != height) {
                    height = new_height;
                }
                free(height_value);
            }

            free(*post_data);
            *post_data = NULL;

            reconfigure_pipeline();

            char page[4096];
            snprintf(page, sizeof(page),
                     "<!DOCTYPE html>"
                     "<html>"
                     "<head>"
                     "<title>MJPEG Stream</title>"
                     "<style>"
                     "body { font-family: Arial, sans-serif; }"
                     "</style>"
                     "<script>"
                     "function reloadStream() {"
                     "    var img = document.getElementById('stream');"
                     "    img.onerror = function() {"
                     "        setTimeout(function() {"
                     "            img.src = '/stream?rand=' + Math.random();"
                     "        }, 1000);"
                     "    };"
                     "}"
                     "window.onload = function() {"
                     "    reloadStream();"
                     "};"
                     "</script>"
                     "</head>"
                     "<body>"
                     "<h1>Live Stream</h1>"
                     "<img id=\"stream\" src=\"/stream\" alt=\"MJPEG Stream\" onerror=\"reloadStream()\" />"
                     "<h2>Adjust Settings</h2>"
                     "<form method=\"POST\" action=\"/settings\">"
                     "FPS: <input type=\"number\" name=\"fps\" value=\"%d\"><br>"
                     "Quality: <input type=\"number\" name=\"quality\" value=\"%d\"><br>"
                     "Resolution Width: <input type=\"number\" name=\"width\" value=\"%d\"><br>"
                     "Resolution Height: <input type=\"number\" name=\"height\" value=\"%d\"><br>"
                     "<input type=\"submit\" value=\"Apply\">"
                     "</form>"
                     "</body>"
                     "</html>",
                     fps, quality, width, height);

            struct MHD_Response *response = MHD_create_response_from_buffer(strlen(page), (void *)page, MHD_RESPMEM_MUST_COPY);
            if (!response)
                return MHD_NO;

            MHD_add_response_header(response, "Content-Type", "text/html");
            int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);

            return ret;
        }
    }

    return MHD_NO;
}

/**
 * @brief starts the web server and listens for incoming HTTP requests.
 */

void start_webserver() {
    mhd_daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, PORT, NULL, NULL,
                              request_handler, NULL,
                              MHD_OPTION_NOTIFY_COMPLETED, request_completed_callback, NULL,
                              MHD_OPTION_END);

    if (!mhd_daemon) {
        g_printerr("Failed to start HTTP server\n");
        exit(EXIT_FAILURE);
    }

    g_print("HTTP server running on port %d\n", PORT);
    g_print("Stream available at http://localhost:%d/\n", PORT);
}

/**
 * @brief stops the web server and cleans up.
 */

void stop_webserver() {
    if (mhd_daemon) {
        MHD_stop_daemon(mhd_daemon);
        mhd_daemon = NULL;
    }
}
