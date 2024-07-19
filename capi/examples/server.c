#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <sys/time.h>
#include <signal.h>

#define MAX_EVENTS 128
#define LISTEN_BACKLOG 32
#define BUFFER_SIZE 1024

void die(const char *message) {
    perror(message);
    exit(EXIT_FAILURE);
}

int setup_listener(const char *port) {
    struct addrinfo hints, *result, *rp;
    int sfd, s;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC; // Allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP socket
    hints.ai_flags = AI_PASSIVE; // For wildcard IP address

    s = getaddrinfo(NULL, port, &hints, &result);
    if (s != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) continue;

        int yes = 1;
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt");
            close(sfd);
            continue;
        }

        if (bind(sfd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(sfd, LISTEN_BACKLOG) == 0) {
            // Set the socket to non-blocking mode
            fcntl(sfd, F_SETFL, O_NONBLOCK);
            freeaddrinfo(result);
            return sfd;
        }

        close(sfd);
    }

    freeaddrinfo(result);
    return -1;
}

int main(int argc, char *argv[]) {
    int kq, n, listener, new_conn, nev;
    struct kevent event;
    struct kevent change_list[MAX_EVENTS];
    struct sockaddr_storage their_addr;
    socklen_t addr_size;
    char buf[BUFFER_SIZE];

    if (argc != 2) {
        fprintf(stderr, "Usage: %s [port]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    listener = setup_listener(argv[1]);
    if (listener < 0) {
        die("Failed to set up listener");
    }

    kq = kqueue();
    if (kq == -1) {
        die("kqueue");
    }

    EV_SET(&event, listener, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(kq, &event, 1, NULL, 0, NULL) == -1) {
        die("kevent register");
    }

    printf("Server started on port %s\n", argv[1]);

    while (1) {
        nev = kevent(kq, NULL, 0, change_list, MAX_EVENTS, NULL);
        if (nev < 0) {
            die("kevent wait");
        } else if (nev == 0) {
            printf("kevent timeout\n");
            continue;
        }

        for (n = 0; n < nev; n++) {
            if (change_list[n].flags & EV_ERROR) {
                fprintf(stderr, "EV_ERROR: %s\n", strerror(change_list[n].data));
                continue;
            }

            if (change_list[n].ident == listener) {
                addr_size = sizeof(their_addr);
                new_conn = accept(listener, (struct sockaddr *)&their_addr, &addr_size);
                if (new_conn == -1) {
                    perror("accept");
                    continue;
                }

                fcntl(new_conn, F_SETFL, O_NONBLOCK); // Set non-blocking
                EV_SET(&event, new_conn, EVFILT_READ, EV_ADD, 0, 0, NULL);
                kevent(kq, &event, 1, NULL, 0, NULL);
                printf("Accepted connection\n");
            } else if (change_list[n].filter == EVFILT_READ) {
                int fd = change_list[n].ident;
                ssize_t count = read(fd, buf, sizeof(buf));
                if (count > 0) {
                    printf("Received %zd bytes: %.*s\n", count, (int)count, buf);
                    write(fd, buf, count);  // Echo back
                } else if (count == 0) {
                    close(fd);
                    printf("Connection closed\n");
                } else {
                    perror("read");
                }
            }
        }
    }

    close(listener);
    return 0;
}