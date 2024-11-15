#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <microhttpd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>

// Define constants
#define PORT 8080
#define STREAM_PATH "/stream"

// Define default streaming parameters
static gint fps = 15;
static gint quality = 80; 
static gint width = 1280;
static gint height = 720;

// GStreamer pipeline elements
static GstElement *pipeline = NULL, *source = NULL, *videoconvert = NULL, *jpegenc = NULL, *appsink = NULL;

// Structure to hold frame data
typedef struct {
    unsigned char *data;
    size_t size;
} Frame;

// Shared frame buffer
static Frame current_frame = {NULL, 0};
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

// Structure to hold per-connection information
typedef struct {
    unsigned char *data; // Pointer to the MJPEG frame data
    size_t size;         // Total size of the frame data
    size_t sent;         // Number of bytes already sent
} connection_info;

// Function to handle new frames from GStreamer
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

    // Free the previous frame and store the new one
    if (current_frame.data) {
        free(current_frame.data);
    }
    current_frame.size = map.size;
    current_frame.data = malloc(map.size);
    if (current_frame.data) {
        memcpy(current_frame.data, map.data, map.size);
    }

    pthread_cond_broadcast(&frame_cond);
    pthread_mutex_unlock(&frame_mutex);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);

    return GST_FLOW_OK;
}

// Function to initialize GStreamer pipeline for MJPEG
void init_pipeline() {
    gst_init(NULL, NULL);

    pipeline = gst_pipeline_new("webcam-pipeline");

    #ifdef __APPLE__
        source = gst_element_factory_make("avfvideosrc", "source");
    #elif defined(_WIN32)
        source = gst_element_factory_make("ksvideosrc", "source");
    #else
        source = gst_element_factory_make("v4l2src", "source");
    #endif

    videoconvert = gst_element_factory_make("videoconvert", "videoconvert");
    jpegenc = gst_element_factory_make("jpegenc", "jpegenc");
    appsink = gst_element_factory_make("appsink", "appsink");

    if (!pipeline || !source || !videoconvert || !jpegenc || !appsink) {
        g_printerr("Failed to create GStreamer elements\n");
        exit(EXIT_FAILURE);
    }

    // Configure elements
    g_object_set(source, "device-index", 0, NULL);
    g_object_set(jpegenc, "quality", quality, NULL);
    g_object_set(appsink, "emit-signals", TRUE, NULL);
    g_object_set(appsink, "sync", FALSE, NULL);

    GstCaps *caps = gst_caps_new_simple("image/jpeg",
                                        "framerate", GST_TYPE_FRACTION, fps, 1,
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        NULL);
    gst_app_sink_set_caps(GST_APP_SINK(appsink), caps);
    gst_caps_unref(caps);

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, videoconvert, jpegenc, appsink, NULL);

    if (!gst_element_link_many(source, videoconvert, jpegenc, appsink, NULL)) {
        g_printerr("Failed to link GStreamer elements\n");
        gst_object_unref(pipeline);
        exit(EXIT_FAILURE);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

// MJPEG boundary string
const char *boundary = "boundarydonotcross";

// Function to send MJPEG frame to the client
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

// Function to clean up connection_info
static void request_completed_callback(void *cls, struct MHD_Connection *connection,
                                       void **con_cls, enum MHD_RequestTerminationCode toe) {
    connection_info *conn_info = *con_cls;
    if (conn_info) {
        free(conn_info->data);
        free(conn_info);
    }
}

// Function to handle HTTP requests
static enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                       const char *url, const char *method,
                                       const char *version, const char *upload_data,
                                       size_t *upload_data_size, void **con_cls) {
    if (strcmp(method, "GET") != 0) {
        return MHD_NO;
    }

    if (strcmp(url, STREAM_PATH) != 0) {
        // Serve the index.html for the root path
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

            MHD_add_response_header(response, "Content-Type", "text/html");
            int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
            MHD_destroy_response(response);
            return ret;
        }

        return MHD_NO;
    }

    if (!*con_cls) {
        connection_info *conn_info = malloc(sizeof(connection_info));
        if (!conn_info) {
            return MHD_NO;
        }
        conn_info->data = NULL;
        conn_info->size = 0;
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
}

int main(int argc, char *argv[]) {
    init_pipeline();

    struct MHD_Daemon *daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
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
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    pthread_mutex_destroy(&frame_mutex);
    pthread_cond_destroy(&frame_cond);

    return EXIT_SUCCESS;
}
