#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <uv.h>

#include "hyper.h"

static const int MAX_EVENTS = 128;
static const hyper_executor *exec;

typedef struct conn_data_s {
    uv_tcp_t stream;
    uv_poll_t poll_handle;
    uint32_t event_mask;
    hyper_waker *read_waker;
    hyper_waker *write_waker;
    hyper_task *conn_task;  // Add this to keep track of the connection task
    int is_closing;  // Add this flag to prevent double-free
} conn_data;

typedef struct service_userdata_s {
    char host[128];
    char port[8];
    const hyper_executor *executor;
} service_userdata;

static uv_loop_t *loop;
static uv_tcp_t server;
static uv_signal_t sigint_handle, sigterm_handle;

static void on_signal(uv_signal_t *handle, int signum) {
    printf("Caught signal %d... exiting\n", signum);
    uv_signal_stop(&sigint_handle);
    uv_signal_stop(&sigterm_handle);
    uv_close((uv_handle_t*)&server, NULL);
    uv_stop(loop);
}

static void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    buf->base = malloc(suggested_size);
    buf->len = suggested_size;
}

static void on_close(uv_handle_t* handle) {
    free(handle);
}

static void close_conn(uv_handle_t* handle) {
    conn_data* conn = (conn_data*)handle->data;
    if (conn) {
        printf("Closing connection\n");
        if (conn->read_waker) {
            hyper_waker_free(conn->read_waker);
            conn->read_waker = NULL;
        }
        if (conn->write_waker) {
            hyper_waker_free(conn->write_waker);
            conn->write_waker = NULL;
        }
        if (conn->conn_task) {
            hyper_task_free(conn->conn_task);
            conn->conn_task = NULL;
        }
        free(conn);
    }
    free(handle);
}

static void on_poll(uv_poll_t* handle, int status, int events) {
    conn_data* conn = (conn_data*)handle->data;
    
    if (status < 0) {
        fprintf(stderr, "Poll error: %s\n", uv_strerror(status));
        return;
    }

    if (events & UV_READABLE && conn->read_waker) {
        hyper_waker_wake(conn->read_waker);
        conn->read_waker = NULL;
    }

    if (events & UV_WRITABLE && conn->write_waker) {
        hyper_waker_wake(conn->write_waker);
        conn->write_waker = NULL;
    }
}

static bool update_conn_data_registrations(conn_data *conn, bool create) {
    int events = 0;
    if (conn->event_mask & UV_READABLE) events |= UV_READABLE;
    if (conn->event_mask & UV_WRITABLE) events |= UV_WRITABLE;

    int r = uv_poll_start(&conn->poll_handle, events, on_poll);
    if (r < 0) {
        fprintf(stderr, "uv_poll_start error: %s\n", uv_strerror(r));
        return false;
    }
    return true;
}

static size_t read_cb(void *userdata, hyper_context *ctx, uint8_t *buf, size_t buf_len) {
    conn_data *conn = (conn_data *)userdata;
    ssize_t ret = recv(conn->stream.io_watcher.fd, buf, buf_len, 0);

    if (ret >= 0) {
        return ret;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return HYPER_IO_ERROR;
    }

    if (conn->read_waker != NULL) {
        hyper_waker_free(conn->read_waker);
    }

    if (!(conn->event_mask & UV_READABLE)) {
        conn->event_mask |= UV_READABLE;
        if (!update_conn_data_registrations(conn, false)) {
            return HYPER_IO_ERROR;
        }
    }

    conn->read_waker = hyper_context_waker(ctx);
    return HYPER_IO_PENDING;
}

static size_t write_cb(void *userdata, hyper_context *ctx, const uint8_t *buf, size_t buf_len) {
    conn_data *conn = (conn_data *)userdata;
    ssize_t ret = send(conn->stream.io_watcher.fd, buf, buf_len, 0);

    if (ret >= 0) {
        return ret;
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK) {
        return HYPER_IO_ERROR;
    }

    if (conn->write_waker != NULL) {
        hyper_waker_free(conn->write_waker);
    }

    if (!(conn->event_mask & UV_WRITABLE)) {
        conn->event_mask |= UV_WRITABLE;
        if (!update_conn_data_registrations(conn, false)) {
            return HYPER_IO_ERROR;
        }
    }

    conn->write_waker = hyper_context_waker(ctx);
    return HYPER_IO_PENDING;
}

static conn_data *create_conn_data(uv_tcp_t *client) {
    conn_data *conn = calloc(1, sizeof(conn_data));
    if (!conn) {
        fprintf(stderr, "Failed to allocate conn_data\n");
        return NULL;
    }
    memcpy(&conn->stream, client, sizeof(uv_tcp_t));
    conn->is_closing = 0;

    int r = uv_poll_init(loop, &conn->poll_handle, client->io_watcher.fd);
    if (r < 0) {
        fprintf(stderr, "uv_poll_init error: %s\n", uv_strerror(r));
        free(conn);
        return NULL;
    }

    conn->poll_handle.data = conn;
    conn->stream.data = conn;

    if (!update_conn_data_registrations(conn, true)) {
        uv_close((uv_handle_t*)&conn->poll_handle, NULL);
        free(conn);
        return NULL;
    }

    return conn;
}

static void free_conn_data(void *userdata) {
    conn_data *conn = (conn_data *)userdata;
    if (conn && !conn->is_closing) {
        conn->is_closing = 1;
        printf("Freeing connection data\n");
        if (!uv_is_closing((uv_handle_t*)&conn->poll_handle)) {
            uv_close((uv_handle_t*)&conn->poll_handle, NULL);
        }
        if (!uv_is_closing((uv_handle_t*)&conn->stream)) {
            uv_close((uv_handle_t*)&conn->stream, close_conn);
        }
    }
}

static hyper_io *create_io(conn_data *conn) {
    hyper_io *io = hyper_io_new();
    hyper_io_set_userdata(io, (void *)conn, free_conn_data);
    hyper_io_set_read(io, read_cb);
    hyper_io_set_write(io, write_cb);

    return io;
}

static service_userdata *create_service_userdata() {
    return (service_userdata *)calloc(1, sizeof(service_userdata));
}

static void free_service_userdata(void *userdata) {
    service_userdata *cast_userdata = (service_userdata *)userdata;
    free(cast_userdata);
}

static int print_each_header(
    void *userdata, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len
) {
    printf("%.*s: %.*s\n", (int)name_len, name, (int)value_len, value);
    return HYPER_ITER_CONTINUE;
}

static int print_body_chunk(void *userdata, const hyper_buf *chunk) {
    const uint8_t *buf = hyper_buf_bytes(chunk);
    size_t len = hyper_buf_len(chunk);
    write(1, buf, len);
    return HYPER_ITER_CONTINUE;
}

static int send_each_body_chunk(void *userdata, hyper_context *ctx, hyper_buf **chunk) {
    int *chunk_count = (int *)userdata;
    if (*chunk_count > 0) {
        char data[64];
        snprintf(data, sizeof(data), "Chunk %d\n", *chunk_count);
        *chunk = hyper_buf_copy((const uint8_t*)data, strlen(data));
        (*chunk_count)--;
        return HYPER_POLL_READY;
    } else {
        *chunk = NULL;
        return HYPER_POLL_READY;
    }
}

static void server_callback(void *userdata, hyper_request *request, hyper_response_channel *channel) {
    service_userdata *service_data = (service_userdata *)userdata;
    printf("Received request from %s:%s\n", service_data->host, service_data->port);

    if (request == NULL) {
        fprintf(stderr, "Error: Received null request\n");
        return;
    }

    uint8_t scheme[64] = {0};
    uint8_t authority[256] = {0};
    uint8_t path_and_query[1024] = {0};
    size_t scheme_len = sizeof(scheme);
    size_t authority_len = sizeof(authority);
    size_t path_and_query_len = sizeof(path_and_query);

    enum hyper_code uri_result = hyper_request_uri_parts(request, scheme, &scheme_len, authority, &authority_len, path_and_query, &path_and_query_len);
    if (uri_result == HYPERE_OK) {
        printf("Scheme: %.*s\n", (int)scheme_len, scheme);
        printf("Authority: %.*s\n", (int)authority_len, authority);
        printf("Path and Query: %.*s\n", (int)path_and_query_len, path_and_query);
    } else {
        fprintf(stderr, "Failed to get URI parts. Error code: %d\n", uri_result);
    }

    int version = hyper_request_version(request);
    printf("HTTP Version: ");
    switch(version) {
        case HYPER_HTTP_VERSION_NONE: printf("None\n"); break;
        case HYPER_HTTP_VERSION_1_0: printf("HTTP/1.0\n"); break;
        case HYPER_HTTP_VERSION_1_1: printf("HTTP/1.1\n"); break;
        case HYPER_HTTP_VERSION_2: printf("HTTP/2\n"); break;
        default: printf("Unknown (%d)\n", version);
    }

    uint8_t method[32] = {0};
    size_t method_len = sizeof(method);
    enum hyper_code method_result = hyper_request_method(request, method, &method_len);
    if (method_result == HYPERE_OK) {
        printf("Method: %.*s\n", (int)method_len, method);
    } else {
        fprintf(stderr, "Failed to get request method. Error code: %d\n", method_result);
    }

    printf("Headers:\n");
    hyper_headers *req_headers = hyper_request_headers(request);
    if (req_headers != NULL) {
        hyper_headers_foreach(req_headers, print_each_header, NULL);
    } else {
        fprintf(stderr, "Error: Failed to get request headers\n");
    }

    if (method_len > 0 && (strncmp((char *)method, "POST", method_len) == 0 || strncmp((char *)method, "PUT", method_len) == 0)) {
        printf("Request Body:\n");
        hyper_body *body = hyper_request_body(request);
        if (body != NULL) {
            hyper_task *task = hyper_body_foreach(body, print_body_chunk, NULL, NULL);
            if (task != NULL) {
                hyper_executor_push(service_data->executor, task);
            } else {
                fprintf(stderr, "Error: Failed to create body foreach task\n");
            }
        } else {
            fprintf(stderr, "Error: Failed to get request body\n");
        }
    }

    hyper_response *response = hyper_response_new();
    if (response != NULL) {
        hyper_response_set_status(response, 200);
        hyper_headers *rsp_headers = hyper_response_headers(response);
        if (rsp_headers != NULL) {
            hyper_headers_set(rsp_headers, (unsigned char *)"Content-Type", 12, (unsigned char *)"text/plain", 10);
            hyper_headers_set(rsp_headers, (unsigned char *)"Cache-Control", 13, (unsigned char *)"no-cache", 8);
        } else {
            fprintf(stderr, "Error: Failed to get response headers\n");
        }

        if (method_len > 0 && strncmp((char *)method, "GET", method_len) == 0) {
            hyper_body *body = hyper_body_new();
            if (body != NULL) {
                hyper_body_set_data_func(body, send_each_body_chunk);
                int *chunk_count = (int *)malloc(sizeof(int));
                if (chunk_count != NULL) {
                    *chunk_count = 10;
                    hyper_body_set_userdata(body, (void *)chunk_count, free);
                    hyper_response_set_body(response, body);
                } else {
                    fprintf(stderr, "Error: Failed to allocate chunk_count\n");
                }
            } else {
                fprintf(stderr, "Error: Failed to create response body\n");
            }
        }

        hyper_response_channel_send(channel, response);
    } else {
        fprintf(stderr, "Error: Failed to create response\n");
    }

    hyper_request_free(request);
}

static void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }

    uv_tcp_t *client = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    uv_tcp_init(loop, client);
    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        service_userdata *userdata = create_service_userdata();
        userdata->executor = exec;

        struct sockaddr_storage addr;
        int addrlen = sizeof(addr);
        uv_tcp_getpeername(client, (struct sockaddr*)&addr, &addrlen);

        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&addr;
            uv_ip4_name(s, userdata->host, sizeof(userdata->host));
            snprintf(userdata->port, sizeof(userdata->port), "%d", ntohs(s->sin_port));
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
            uv_ip6_name(s, userdata->host, sizeof(userdata->host));
            snprintf(userdata->port, sizeof(userdata->port), "%d", ntohs(s->sin6_port));
        }

        printf("New incoming connection from (%s:%s)\n", userdata->host, userdata->port);

        conn_data *conn = create_conn_data(client);
        if (!conn) {
            fprintf(stderr, "Failed to create conn_data\n");
            uv_close((uv_handle_t*)client, on_close);
            free_service_userdata(userdata);
            return;
        }

        hyper_io *io = create_io(conn);

        hyper_service *service = hyper_service_new(server_callback);
        hyper_service_set_userdata(service, userdata, free_service_userdata);

        hyper_http1_serverconn_options *http1_opts = hyper_http1_serverconn_options_new(userdata->executor);
        hyper_http1_serverconn_options_header_read_timeout(http1_opts, 1000 * 5);

        hyper_http2_serverconn_options *http2_opts = hyper_http2_serverconn_options_new(userdata->executor);
        hyper_http2_serverconn_options_keep_alive_interval(http2_opts, 5);
        hyper_http2_serverconn_options_keep_alive_timeout(http2_opts, 5);

        hyper_task *serverconn = hyper_serve_httpX_connection(http1_opts, http2_opts, io, service);
        conn->conn_task = serverconn;  // Store the task in conn_data
        hyper_task_set_userdata(serverconn, conn, free_conn_data);
        hyper_executor_push(userdata->executor, serverconn);

        hyper_http1_serverconn_options_free(http1_opts);
        hyper_http2_serverconn_options_free(http2_opts);
    } else {
        uv_close((uv_handle_t*)client, on_close);
    }
}

int main(int argc, char *argv[]) {
    exec = hyper_executor_new();
    if (exec == NULL) {
        fprintf(stderr, "Failed to create hyper executor\n");
        return 1;
    }

    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "1234";
    printf("listening on port %s on %s...\n", port, host);

    loop = uv_default_loop();

    uv_tcp_init(loop, &server);

    struct sockaddr_in addr;
    uv_ip4_addr(host, atoi(port), &addr);

    int r = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
    if (r != 0) {
        fprintf(stderr, "Bind error %s\n", uv_strerror(r));
        return 1;
    }

    // Set SO_REUSEADDR
    int yes = 1;
    r = setsockopt(server.io_watcher.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (r != 0) {
        fprintf(stderr, "setsockopt error %s\n", strerror(errno));
        return 1;
    }

    r = uv_listen((uv_stream_t*)&server, SOMAXCONN, on_new_connection);
    if (r != 0) {
        fprintf(stderr, "Listen error %s\n", uv_strerror(r));
        return 1;
    }

    uv_signal_init(loop, &sigint_handle);
    uv_signal_start(&sigint_handle, on_signal, SIGINT);

    uv_signal_init(loop, &sigterm_handle);
    uv_signal_start(&sigterm_handle, on_signal, SIGTERM);

    printf("http handshake (hyper v%s) ...\n", hyper_version());
    
    while (1) {
        uv_run(loop, UV_RUN_NOWAIT);

        hyper_task *task = hyper_executor_poll(exec);
        while (task != NULL) {
            if (hyper_task_type(task) == HYPER_TASK_ERROR) {
                hyper_error *err = hyper_task_value(task);
                uint8_t errbuf[256];
                size_t errlen = hyper_error_print(err, errbuf, sizeof(errbuf));
                fprintf(stderr, "Task error: %.*s\n", (int)errlen, errbuf);
                hyper_error_free(err);
            } else if (hyper_task_type(task) == HYPER_TASK_EMPTY) {
                conn_data *conn = (conn_data *)hyper_task_userdata(task);
                if (conn != NULL) {
                    printf("Connection closed. Cleaning up resources.\n");
                    free_conn_data(conn);
                } else {
                    fprintf(stderr, "Warning: Empty task with no associated connection data\n");
                }
            }
            hyper_task_free(task);
            task = hyper_executor_poll(exec);
        }
    }

    // Cleanup
    uv_loop_close(loop);
    hyper_executor_free(exec);

    return 0;
}