CC = gcc
CFLAGS = -Wall
INCLUDES = -Icwebsocket/lib
LIBS = -lX11 -lpthread
DEPS = cwebsocket/lib/websocket.h
OBJ = cwebsocket/lib/base64_enc.o cwebsocket/lib/sha1.o cwebsocket/lib/websocket.o server.o

%.o: %.c $(DEPS)
	$(CC) $(CFLAGS) -c -o $@ $< $(LIBS)

server: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf cwebsocket/lib/*.o *.o server
