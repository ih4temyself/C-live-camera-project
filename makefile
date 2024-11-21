CC = gcc
CFLAGS = -Wall -pthread $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 libmicrohttpd)
LDFLAGS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 libmicrohttpd)
TARGET = streamer
SOURCES = main.c backend.c webserver.c
OBJS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)
