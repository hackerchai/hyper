# Build the example client
#

RPATH=$(PWD)/../../target/release
HYPER_CFLAGS += -I../include -ggdb3
HYPER_LDFLAGS += -L$(RPATH) -Wl,-rpath,$(RPATH)
LIBS = -lhyper

# Use pkg-config to get libuv flags
UV_CFLAGS := $(shell pkg-config --cflags libuv)
UV_LDFLAGS := $(shell pkg-config --libs libuv)

all: client upload server

%.o : %.c ../include/hyper.h
	${CC} -c -o $@ $< $(HYPER_CFLAGS) $(UV_CFLAGS) $(CFLAGS)

client: client.o
	$(CC) -o $@ $< $(HYPER_LDFLAGS) $(UV_LDFLAGS) $(LDFLAGS) $(LIBS)
upload: upload.o
	$(CC) -o $@ $< $(HYPER_LDFLAGS) $(UV_LDFLAGS) $(LDFLAGS) $(LIBS)
server: server.o
	$(CC) -o $@ $< $(HYPER_LDFLAGS) $(UV_LDFLAGS) $(LDFLAGS) $(LIBS)
server_legacy: server_legacy.o
	$(CC) -o $@ $< $(HYPER_LDFLAGS) $(UV_LDFLAGS) $(LDFLAGS) $(LIBS)
server_epoll: server_epoll.o
	$(CC) -o $@ $< $(HYPER_LDFLAGS) $(LDFLAGS) $(LIBS)

clean:
	rm -f *.o client server upload