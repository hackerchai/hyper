#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#define MAX_WRITE_SIZE 1024

typedef struct {
    uv_tcp_t client;
    uv_write_t write_req;
    char buffer[MAX_WRITE_SIZE];
} conn_data;

static uv_loop_t *loop;

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void on_close(uv_handle_t *handle) {
    conn_data *data = (conn_data *)handle->data;
    if (data) {
        free(data);
    }
}

static void echo_write(uv_write_t *req, int status) {
    if (status) {
        fprintf(stderr, "Write error: %s\n", uv_strerror(status));
    }
    free(req);
}

static void on_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    if (nread > 0) {
        printf("Received data: %.*s", (int)nread, buf->base);

        conn_data *data = (conn_data *)client->data;
        uv_buf_t wrbuf = uv_buf_init(buf->base, nread);

        uv_write_t *req = (uv_write_t *)malloc(sizeof(uv_write_t));
        uv_write(req, client, &wrbuf, 1, echo_write);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            fprintf(stderr, "Read error: %s\n", uv_strerror(nread));
        }
        uv_close((uv_handle_t*) client, on_close);
    }

    free(buf->base);
}

static void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error: %s\n", uv_strerror(status));
        return;
    }

    conn_data *data = (conn_data *)malloc(sizeof(conn_data));
    uv_tcp_init(loop, &data->client);
    data->client.data = data;

    if (uv_accept(server, (uv_stream_t*)&data->client) == 0) {
        uv_read_start((uv_stream_t*)&data->client, alloc_buffer, on_read);
    } else {
        uv_close((uv_handle_t*)&data->client, on_close);
    }
}

static int listen_on(const char *host, const char *port, uv_tcp_t *server) {
    struct sockaddr_in addr;
    uv_ip4_addr(host, atoi(port), &addr);

    uv_tcp_init(loop, server);
    uv_tcp_bind(server, (const struct sockaddr*)&addr, 0);

    int r = uv_listen((uv_stream_t*)server, 128, on_new_connection);
    if (r) {
        fprintf(stderr, "Listen error %s\n", uv_strerror(r));
        return -1;
    }
    return 0;
}

static void signal_handler(uv_signal_t *handle, int signum) {
    printf("Signal received: %d\n", signum);
    uv_stop(handle->loop);
}

static void setup_signals() {
    static uv_signal_t sigint;
    uv_signal_init(loop, &sigint);
    uv_signal_start(&sigint, signal_handler, SIGINT);

    static uv_signal_t sigterm;
    uv_signal_init(loop, &sigterm);
    uv_signal_start(&sigterm, signal_handler, SIGTERM);
}

int main(int argc, char **argv) {
    loop = uv_default_loop();

    uv_tcp_t server;
    char *port = argc > 1 ? argv[1] : "1234"; // Default port is 1234
    listen_on("0.0.0.0", port, &server);
    setup_signals();

    return uv_run(loop, UV_RUN_DEFAULT);
}