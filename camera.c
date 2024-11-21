#include "camera.h"
#include <gst/gst.h>
#include <stdlib.h>
#include <pthread.h>

static GstElement *pipeline = NULL, *source = NULL, *videoconvert = NULL, *jpegenc = NULL, *appsink = NULL;

Frame current_frame = {NULL, 0, 0};
pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;
CameraCapabilities *camera_caps = NULL;

gint fps = 30;
gint quality = 80;
gint width = 1280;
gint height = 720;

void init_camera_pipeline() {
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

    g_signal_connect(appsink, "new-sample", G_CALLBACK(handle_new_frame), NULL);

    gst_bin_add_many(GST_BIN(pipeline), source, videoconvert, jpegenc, appsink, NULL);

    if (!gst_element_link_many(source, videoconvert, jpegenc, appsink, NULL)) {
        g_printerr("Failed to link GStreamer elements\n");
        gst_object_unref(pipeline);
        exit(EXIT_FAILURE);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}

void update_camera_settings(gint new_fps, gint new_width, gint new_height, gint new_quality) {
    if (new_fps != fps || new_width != width || new_height != height || new_quality != quality) {
        stop_camera_pipeline();
        fps = new_fps;
        width = new_width;
        height = new_height;
        quality = new_quality;
        init_camera_pipeline();
    }
}

void stop_camera_pipeline() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
}

void cleanup_camera_resources() {
    if (current_frame.data) {
        free(current_frame.data);
    }
    pthread_mutex_destroy(&frame_mutex);
    pthread_cond_destroy(&frame_cond);
}

void handle_new_frame(GstElement *sink) {
    GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
    if (!sample) {
        g_printerr("Failed to pull sample from appsink\n");
        return;
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;

    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        g_printerr("Failed to map buffer\n");
        gst_sample_unref(sample);
        return;
    }

    pthread_mutex_lock(&frame_mutex);

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
}

gboolean get_supported_fps(gint width, gint height, GArray **fps_array) {
    return TRUE;
}

gboolean get_supported_resolutions(GArray **resolutions_array) {
    return TRUE;
}
