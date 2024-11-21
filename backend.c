#include "backend.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static GstElement *pipeline = NULL;
static pthread_mutex_t pipeline_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief callback function to handle new video samples.
 * 
 * triggered when a new sample is available in the GStreamer pipeline.
 * it takes and extracts the sample data, stores it in the `current_frame` object, and notifies
 * other threads that the frame is ready for usage.
 * 
 * @param sink the GstElement that produced the sample.
 * @param user_data user data passed to the callback.
 * 
 * @return GST_FLOW_OK if success, or GST_FLOW_ERROR if error.
 */

Frame current_frame = {NULL, 0, 0};
pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t frame_cond = PTHREAD_COND_INITIALIZER;

gint fps = 30;
gint quality = 65;
gint width = 1280;
gint height = 720;

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

/**
 * @brief initializes the GStreamer pipeline.
 * 
 * creates and configures the GStreamer elements required for video
 * capturing, it sets up the pipeline and starts the video stream.
 */

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

/**
 * @brief reconfiguring the GStreamer pipeline.
 * 
 * stops and restarts the pipeline is used when the settings
 * such as frame rate, quality, or resolution are updated.
 */

void reconfigure_pipeline() {
    init_pipeline();
}

/**
 * @brief cleans up and stops the GStreamer pipeline.
 * 
 * stops the pipeline and cleans up (releases) any resources that were held by it.
 */

void cleanup_pipeline() {
    pthread_mutex_lock(&pipeline_mutex);
    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    }
    pthread_mutex_unlock(&pipeline_mutex);
}
