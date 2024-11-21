CC = gcc
CFLAGS = -Wall -pthread $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 libmicrohttpd)
LDFLAGS = $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 libmicrohttpd)
TARGET = streamer

all: $(TARGET)

$(TARGET): main.c
	$(CC) $(CFLAGS) -o $(TARGET) main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)