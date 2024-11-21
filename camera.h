#ifndef CAMERA_H
#define CAMERA_H

#include <gst/gst.h>
#include <pthread.h>

typedef struct {
    unsigned char *data;
    size_t size;
    uint64_t version;
} Frame;

typedef struct {
    GArray *framerates;
    GArray *resolutions;
} CameraCapabilities;

extern Frame current_frame;
extern pthread_mutex_t frame_mutex;
extern pthread_cond_t frame_cond;
extern CameraCapabilities *camera_caps;

extern gint fps;
extern gint quality;
extern gint width;
extern gint height;


void init_camera_pipeline();
void update_camera_settings(gint new_fps, gint new_width, gint new_height, gint new_quality);
void stop_camera_pipeline();
void cleanup_camera_resources();
void handle_new_frame(GstElement *sink);
gboolean get_supported_fps(gint width, gint height, GArray **fps_array);
gboolean get_supported_resolutions(GArray **resolutions_array);

#endif