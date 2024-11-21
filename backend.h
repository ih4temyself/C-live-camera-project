#ifndef BACKEND_H
#define BACKEND_H

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <pthread.h>

/**
 * @brief initializes the video processing pipeline.
 * 
 * sets up the GStreamer pipeline and other needed elements for video capture.
 */

void init_pipeline();
/**
 * @brief reconfigures the pipeline.
 * 
 * stops the current pipeline and restarts it (to apply new settings).
 */

void reconfigure_pipeline();
/**
 * @brief cleans up and releases resources used by the pipeline.
 * 
 * frees any allocated memory and resets the pipeline state.
 */

void cleanup_pipeline();


typedef struct {
    unsigned char *data; /** raw frame data */
    size_t size; /** size of the frame data */
    uint64_t version; /** version number of the current frame */
} Frame;

extern Frame current_frame; /** current video frame */
extern pthread_mutex_t frame_mutex; /** mutex to protect frame data */
extern pthread_cond_t frame_cond; /** condition variable for frame updates */

extern gint fps; /** frames per second for video capture */
extern gint quality; /**< video quality */
extern gint width; /** width of the video frame */
extern gint height; /** height of the video frame */

#endif /* BACKEND_H */
