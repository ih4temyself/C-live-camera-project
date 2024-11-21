#ifndef BACKEND_H
#define BACKEND_H

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <pthread.h>

void init_pipeline();
void reconfigure_pipeline();
void cleanup_pipeline();

typedef struct {
    unsigned char *data;
    size_t size;
    uint64_t version;
} Frame;

extern Frame current_frame;
extern pthread_mutex_t frame_mutex;
extern pthread_cond_t frame_cond;

extern gint fps;
extern gint quality;
extern gint width;
extern gint height;

#endif /* BACKEND_H */
