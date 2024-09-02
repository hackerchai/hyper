#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include <uv.h>

#include "hyper.h"

static const int MAX_EVENTS = 128;
const hyper_executor *exec;
hyper_http1_serverconn_options *http1_opts;
hyper_http2_serverconn_options *http2_opts;

typedef struct conn_data_s {
    uv_tcp_t stream;
    uv_poll_t poll_handle;
    uint32_t event_mask;
    hyper_waker *read_waker;
    hyper_waker *write_waker;
    atomic_bool is_closing;  // Add this flag to prevent double-free
    int closed_handles;
} conn_data;

typedef struct service_userdata_s {
    char host[128];
    char port[8];
    const hyper_executor *executor;
} service_userdata;

static uv_loop_t *loop;
static uv_tcp_t server;
static uv_check_t check_handle;
static uv_signal_t sigint_handle, sigterm_handle;
static volatile bool should_exit = false;

// UV callback functions
static void on_signal(uv_signal_t *handle, int signum);
static void close_walk_cb(uv_handle_t* handle, void* arg);
static void on_close(uv_handle_t* handle);
static void on_poll(uv_poll_t* handle, int status, int events);
static void on_new_connection(uv_stream_t *server, int status);
static void on_check(uv_check_t* handle);

// Hyper callback functions
static size_t read_cb(void *userdata, hyper_context *ctx, uint8_t *buf, size_t buf_len);
static size_t write_cb(void *userdata, hyper_context *ctx, const uint8_t *buf, size_t buf_len);
static int print_each_header(void *userdata, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len);
static int print_body_chunk(void *userdata, const hyper_buf *chunk);
static int send_each_body_chunk(void *userdata, hyper_context *ctx, hyper_buf **chunk);
static void server_callback(void *userdata, hyper_request *request, hyper_response_channel *channel);

// Utility functions
static bool update_conn_data_registrations(conn_data *conn, bool create);

// Cleanup functions
static void free_conn_data(void *userdata);
static void free_service_userdata(void *userdata);

// Create functions
static conn_data *create_conn_data();
static hyper_io *create_io(conn_data *conn);
static service_userdata *create_service_userdata();

// uv callback for signal
static void on_signal(uv_signal_t *handle, int signum) {
    printf("Caught signal %d... exiting\n", signum);
    should_exit = true;
    uv_signal_stop(&sigint_handle);
    uv_signal_stop(&sigterm_handle);
    uv_close((uv_handle_t*)&server, NULL);
    uv_stop(loop);
}

// uv callback for close walk
static void close_walk_cb(uv_handle_t* handle, void* arg) {
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

// uv callback for close
static void on_close(uv_handle_t* handle) {
    conn_data* conn = (conn_data*)handle->data;

    if (conn) {
        conn->closed_handles++;
        if (conn->closed_handles >= 2) {
            printf("All handles closed, freeing conn data\n");
            free(conn);
        }
    }
}

// uv callback for poll
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

// update conn data registrations
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

// hyper callback for read
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

// hyper callback for write
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


/* Cleanup functions*/

// free conn data
static void free_conn_data(void *userdata) {
    conn_data *conn = (conn_data *)userdata;

    if (conn && !atomic_exchange(&conn->is_closing, true)) {
        printf("Closing connection...\n");
        if (conn->read_waker) {
            hyper_waker_free(conn->read_waker);
            conn->read_waker = NULL;
        }
        if (conn->write_waker) {
            hyper_waker_free(conn->write_waker);
            conn->write_waker = NULL;
        }

        if (!uv_is_closing((uv_handle_t*)&conn->poll_handle)) {
            uv_close((uv_handle_t*)&conn->poll_handle, on_close);
        }

        if (!uv_is_closing((uv_handle_t*)&conn->stream)) {
            uv_close((uv_handle_t*)&conn->stream, on_close);
        }
    }
}

// free service userdata
static void free_service_userdata(void *userdata) {
    service_userdata *cast_userdata = (service_userdata *)userdata;
    if (cast_userdata != NULL) {
        // Note: We don't free conn here because it's managed separately
        free(cast_userdata);
    }
}


/* Create functions */

// create conn data
static conn_data *create_conn_data() {
    conn_data *conn = calloc(1, sizeof(conn_data));
    if (!conn) {
        fprintf(stderr, "Failed to allocate conn_data\n");
        return NULL;
    }

    atomic_init(&conn->is_closing, false);

    return conn;
}

// create service userdata
static service_userdata *create_service_userdata() {
    service_userdata *userdata = (service_userdata *)calloc(1, sizeof(service_userdata));
    if (userdata == NULL) {
        fprintf(stderr, "Failed to allocate service_userdata\n");
    }
    return userdata;
}

// create io
static hyper_io *create_io(conn_data *conn) {
    hyper_io *io = hyper_io_new();
    hyper_io_set_userdata(io, (void *)conn, free_conn_data);
    hyper_io_set_read(io, read_cb);
    hyper_io_set_write(io, write_cb);

    return io;
}


/* Hyper callback functions */

// print each header
static int print_each_header(
    void *userdata, const uint8_t *name, size_t name_len, const uint8_t *value, size_t value_len
) {
    printf("%.*s: %.*s\n", (int)name_len, name, (int)value_len, value);
    return HYPER_ITER_CONTINUE;
}

// print body chunk
static int print_body_chunk(void *userdata, const hyper_buf *chunk) {
    const uint8_t *buf = hyper_buf_bytes(chunk);
    size_t len = hyper_buf_len(chunk);
    write(1, buf, len);
    return HYPER_ITER_CONTINUE;
}

// send each body chunk
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

// server callback for hyper io
static void server_callback(void *userdata, hyper_request *request, hyper_response_channel *channel) {
    service_userdata *service_data = (service_userdata *)userdata;

    printf("Handling request on connection from %s:%s\n", service_data->host, service_data->port);

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

    // We don't close the connection here. Let hyper handle keep-alive.
    // free request
    hyper_request_free(request);
}

// uv callback for new connection to create hyper server connection
static void on_new_connection(uv_stream_t *server, int status) {
    if (status < 0) {
        fprintf(stderr, "New connection error %s\n", uv_strerror(status));
        return;
    }

    conn_data *conn = create_conn_data();
    if (!conn) {
        fprintf(stderr, "Failed to create conn_data\n");
        return;
    }

    uv_tcp_init(loop, &conn->stream);
    conn->stream.data = conn;

    if (uv_accept(server, (uv_stream_t*)&conn->stream) == 0) {
        int r = uv_poll_init(loop, &conn->poll_handle, conn->stream.io_watcher.fd);
        if (r < 0) {
            printf("on_new_connection: uv_poll_init error\n");
            fprintf(stderr, "uv_poll_init error: %s\n", uv_strerror(r));
            uv_close((uv_handle_t*)&conn->stream, on_close);
            free(conn);
            return;
        }

        conn->poll_handle.data = conn;

        if (!update_conn_data_registrations(conn, true)) {
            uv_close((uv_handle_t*)&conn->poll_handle, on_close);
            uv_close((uv_handle_t*)&conn->stream, on_close);
            return;
        }

        service_userdata *userdata = create_service_userdata();
        if (userdata == NULL) {
            fprintf(stderr, "Failed to create service_userdata\n");
            uv_close((uv_handle_t*)&conn->poll_handle, on_close);
            uv_close((uv_handle_t*)&conn->stream, on_close);
            return;
        }
        userdata->executor = exec;

        struct sockaddr_storage addr;
        int addrlen = sizeof(addr);
        uv_tcp_getpeername(&conn->stream, (struct sockaddr*)&addr, &addrlen);

        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&addr;
            uv_ip4_name(s, userdata->host, sizeof(userdata->host));
            snprintf(userdata->port, sizeof(userdata->port), "%d", ntohs(s->sin_port));
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
            uv_ip6_name(s, userdata->host, sizeof(userdata->host));
            snprintf(userdata->port, sizeof(userdata->port), "%d", ntohs(s->sin6_port));
        }

        hyper_io *io = create_io(conn);

        hyper_service *service = hyper_service_new(server_callback);
        hyper_service_set_userdata(service, userdata, free_service_userdata);

        hyper_task *serverconn = hyper_serve_httpX_connection(http1_opts, http2_opts, io, service);
        hyper_executor_push(userdata->executor, serverconn);
    } else {
        uv_close((uv_handle_t*)&conn->poll_handle, on_close);
        uv_close((uv_handle_t*)&conn->stream, on_close);
    }
}

// uv callback for uv_check_t
static void on_check(uv_check_t* handle) {
    hyper_task *task = hyper_executor_poll(exec);
    while (task != NULL) {
        if (hyper_task_type(task) == HYPER_TASK_ERROR) {
            printf("hyper task failed with error!\n");

            hyper_error *err = hyper_task_value(task);
            printf("error code: %d\n", hyper_error_code(err));
            uint8_t errbuf[256];
            size_t errlen = hyper_error_print(err, errbuf, sizeof(errbuf));
            printf("details: %.*s\n", (int)errlen, errbuf);

            hyper_error_free(err);
            hyper_task_free(task);
        } else if (hyper_task_type(task) == HYPER_TASK_EMPTY) {
            printf("internal hyper task complete\n");
            hyper_task_free(task);
        } else if (hyper_task_type(task) == HYPER_TASK_SERVERCONN) {
            printf("server connection task complete\n");
            hyper_task_free(task);
        }

        task = hyper_executor_poll(exec);
    }

    if (should_exit) {
        printf("Shutdown initiated, cleaning up...\n");
        uv_check_stop(handle);
    }
}

int main(int argc, char *argv[]) {
    exec = hyper_executor_new();
    if (exec == NULL) {
        fprintf(stderr, "Failed to create hyper executor\n");
        return 1;
    }

    http1_opts = hyper_http1_serverconn_options_new(exec);
    if (http1_opts == NULL) {
        fprintf(stderr, "Failed to create http1_opts\n");
        return 1;
    }
    hyper_code result = hyper_http1_serverconn_options_header_read_timeout(http1_opts, 5 * 1000);
    if (result != HYPERE_OK) {
        fprintf(stderr, "Failed to set header read timeout for http1_opts\n");
        return 1;
    }

    http2_opts = hyper_http2_serverconn_options_new(exec);
    if (http2_opts == NULL) {
        fprintf(stderr, "Failed to create http2_opts\n");
        return 1;
    }
    result = hyper_http2_serverconn_options_keep_alive_interval(http2_opts, 5);
    if (result != HYPERE_OK) {
        fprintf(stderr, "Failed to set keep alive interval for http2_opts\n");
        return 1;
    }
    result = hyper_http2_serverconn_options_keep_alive_timeout(http2_opts, 5);
    if (result != HYPERE_OK) {
        fprintf(stderr, "Failed to set keep alive timeout for http2_opts\n");
        return 1;
    }

    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port = argc > 2 ? argv[2] : "1234";
    printf("listening on port %s on %s...\n", port, host);

    loop = uv_default_loop();

    int r = uv_tcp_init(loop, &server);
    if (r != 0) {
        fprintf(stderr, "Failed to initialize server\n");
        return 1;
    }

    struct sockaddr_in addr;
    r = uv_ip4_addr(host, atoi(port), &addr);
    if (r != 0) {
        fprintf(stderr, "Failed to set address for server\n");
        return 1;
    }

    r = uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);
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

    r = uv_signal_init(loop, &sigint_handle);
    if (r != 0) {
        fprintf(stderr, "Failed to initialize signal handler for SIGINT\n");
        return 1;
    }
    r = uv_signal_start(&sigint_handle, on_signal, SIGINT);
    if (r != 0) {
        fprintf(stderr, "Failed to start signal handler for SIGINT\n");
        return 1;
    }

    r = uv_signal_init(loop, &sigterm_handle);
    if (r != 0) {
        fprintf(stderr, "Failed to initialize signal handler for SIGTERM\n");
        return 1;
    }
    r = uv_signal_start(&sigterm_handle, on_signal, SIGTERM);
    if (r != 0) {
        fprintf(stderr, "Failed to start signal handler for SIGTERM\n");
        return 1;
    }

    r = uv_check_init(loop, &check_handle);
    if (r != 0) {
        fprintf(stderr, "Failed to initialize check handler\n");
        return 1;
    }
    r = uv_check_start(&check_handle, on_check);
    if (r != 0) {
        fprintf(stderr, "Failed to start check handler\n");
        return 1;
    }

    printf("http handshake (hyper v%s) ...\n", hyper_version());
    
    uv_run(loop, UV_RUN_DEFAULT);

    // Cleanup
    printf("Closing all handles...\n");
    uv_walk(loop, close_walk_cb, NULL);
    uv_run(loop, UV_RUN_DEFAULT);

    uv_loop_close(loop);
    if (exec) {
        hyper_executor_free(exec);
    }
    if (http1_opts) {
        hyper_http1_serverconn_options_free(http1_opts);
    }
    if (http2_opts) {
        hyper_http2_serverconn_options_free(http2_opts);
    }

    printf("Shutdown complete.\n");
    return 0;
}