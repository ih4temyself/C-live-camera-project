#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <microhttpd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

#define PORT 8080
#define STREAM_PATH "/stream"

static gint fps = 30;
static gint quality = 80;
static gint width = 1280;
static gint height = 720;

static GstElement *pipeline = NULL;
static pthread_mutex_t pipeline_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    unsigned char *data;
    size_t size;
    uint64_t version;
} Frame;


static Frame current_frame = {NULL, 0, 0};
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    uint64_t frame_version; 
    size_t sent;           
} connection_info;

static GstFlowReturn on_new_sample(GstElement *sink, gpointer user_data) {
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        g_printerr("Failed to pull sample from appsink\n");
        return GST_FLOW_ERROR;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_printerr("Failed to map buffer\n");
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    pthread_mutex_lock(&frame_mutex);

    if (current_frame.data) {
        free(current_frame.data);
    }

    const char *boundary_str = "--boundarydonotcross\r\n";
    const char *header = "Content-Type: image/jpeg\r\n\r\n";

    size_t boundary_size = strlen(boundary_str);
    size_t header_size = strlen(header);

    current_frame.size = boundary_size + header_size + map.size;

    current_frame.data = malloc(current_frame.size);
    if (!current_frame.data) {
        pthread_mutex_unlock(&frame_mutex);
        gst_buffer_unmap(buffer, &map);
        gst_sample_unref(sample);
        return GST_FLOW_ERROR;
    }

    memcpy(current_frame.data, boundary_str, boundary_size);
    memcpy(current_frame.data + boundary_size, header, header_size);
    memcpy(current_frame.data + boundary_size + header_size, map.data, map.size);

    current_frame.version++;

    pthread_cond_broadcast(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

void init_pipeline() {
    gst_init(NULL, NULL);
    pthread_mutex_lock(&pipeline_mutex);

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }

    pipeline = gst_pipeline_new("webcam-pipeline");

#ifdef __APPLE__
    GstElement *source = gst_element_factory_make("avfvideosrc", "source");
#elif defined(_WIN32)
    GstElement *source = gst_element_factory_make("ksvideosrc", "source");
#else
    GstElement *source = gst_element_factory_make("v4l2src", "source");
#endif

    GstElement *videoconvert = gst_element_factory_make("videoconvert", "videoconvert");
    GstElement *jpegenc = gst_element_factory_make("jpegenc", "jpegenc");
    GstElement *appsink = gst_element_factory_make("appsink", "appsink");

    if (!pipeline || !source || !videoconvert || !jpegenc || !appsink) {
        g_printerr("Failed to create GStreamer elements\n");
        exit(EXIT_FAILURE);
    }

    g_object_set(source, "device-index", 0, NULL);
    g_object_set(jpegenc, "quality", quality, NULL);
    g_object_set(appsink, "emit-signals", TRUE, NULL);
    g_object_set(appsink, "sync", FALSE, NULL);

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, videoconvert, jpegenc, appsink, NULL);

    if (!gst_element_link_many(source, videoconvert, jpegenc, appsink, NULL)) {
        g_printerr("Failed to link GStreamer elements\n");
        gst_object_unref(pipeline);
        exit(EXIT_FAILURE);
    }

    GstCaps *caps = gst_caps_new_simple("image/jpeg",
                                        "framerate", GST_TYPE_FRACTION, fps, 1,
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        NULL);
    gst_app_sink_set_caps(GST_APP_SINK(appsink), caps);
    gst_caps_unref(caps);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    pthread_mutex_unlock(&pipeline_mutex);
}

void reconfigure_pipeline() {
    init_pipeline();
}

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

static void request_completed_callback(void *cls, struct MHD_Connection *connection,
                                       void **con_cls, enum MHD_RequestTerminationCode toe) {
    connection_info *conn_info = *con_cls;
    if (conn_info) {
        free(conn_info);
    }
}

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

            // Set appropriate headers for MJPEG streaming
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

int main(int argc, char *argv[]) {
    init_pipeline();

    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION, PORT, NULL, NULL,
                                                 request_handler, NULL,
                                                 MHD_OPTION_NOTIFY_COMPLETED, request_completed_callback, NULL,
                                                 MHD_OPTION_END);

    if (!daemon) {
        g_printerr("Failed to start HTTP server\n");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return EXIT_FAILURE;
    }

    g_print("HTTP server running on port %d\n", PORT);
    g_print("Stream available at http://localhost:%d/\n", PORT);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    MHD_stop_daemon(daemon);

    pthread_mutex_lock(&pipeline_mutex);
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }
    pthread_mutex_unlock(&pipeline_mutex);

    pthread_mutex_destroy(&frame_mutex);
    pthread_cond_destroy(&frame_cond);
    pthread_mutex_destroy(&pipeline_mutex);

    return EXIT_SUCCESS;
}
