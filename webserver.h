#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <microhttpd.h>

typedef struct {
    unsigned char *data;
    size_t size;
    size_t sent;
} connection_info;

enum MHD_Result request_handler(void *cls, struct MHD_Connection *connection,
                                const char *url, const char *method,
                                const char *version, const char *upload_data,
                                size_t *upload_data_size, void **con_cls);

ssize_t stream_callback(void *cls, uint64_t pos, char *buf, size_t max);

void request_completed_callback(void *cls, struct MHD_Connection *connection,
                                 void **con_cls, enum MHD_RequestTerminationCode toe);

void start_http_server(int port);

#endif
