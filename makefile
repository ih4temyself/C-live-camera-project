CC = gcc
CFLAGS = -g -Wall -I/usr/include/gstreamer-1.0 -I/usr/include/glib-2.0 -I/usr/include/gio-unix-2.0 -I/usr/include/libxml2
LIBS = -lgstreamer-1.0 -lglib-2.0 -lxml2 -lmicrohttpd -pthread
SRC = main.c camera.c webserver.c
OBJ = $(SRC:.c=.o)
EXEC = webcam_stream

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $(EXEC) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(EXEC)
