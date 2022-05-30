#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <liburing.h>

#define BUFLEN 1000
#define MAXN 32

bool online[MAXN];
int client_fds[MAXN];

// user data, used to tag completion messages
struct req_tag {
    enum { ACCEPT, SEND, RECV } event_type;
    int client_id;
    char *buf; size_t len;
};

// helper functions

void add_accept_request(struct io_uring *ring, int fd0) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, fd0, NULL, NULL, 0);
    struct req_tag *tag = malloc(sizeof(struct req_tag));
    tag->event_type = ACCEPT;
    io_uring_sqe_set_data(sqe, tag);
    io_uring_submit(ring);
}

void add_close_request(struct io_uring *ring, int fd) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_close(sqe, fd);
    io_uring_sqe_set_data(sqe, NULL);   // NULL tag signifies close operation
    io_uring_submit(ring);
}

void add_recv_request(struct io_uring *ring, int client_id, int len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct req_tag *tag = malloc(sizeof(struct req_tag));
    tag->event_type = RECV;
    tag->client_id = client_id;
    tag->buf = malloc(len + 3);
    tag->len = len;
    io_uring_prep_recv(sqe, client_fds[client_id], tag->buf, len, 0);
    io_uring_sqe_set_data(sqe, tag);
    io_uring_submit(ring);
}

void add_send_request(struct io_uring *ring, int client_id, char *buf, int len) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct req_tag *tag = malloc(sizeof(struct req_tag));
    tag->event_type = SEND;
    tag->client_id = client_id;
    tag->buf = buf;
    tag->len = len;
    io_uring_prep_send(sqe, client_fds[client_id], tag->buf, len, 0);
    io_uring_sqe_set_data(sqe, tag);
    io_uring_submit(ring);
}

const char *header = "Message: ";

void broadcast_messages(struct io_uring *ring, struct req_tag *orig) {
    int prev = 0;
    for (int i = 0; i < orig->len; i++)
        if (orig->buf[i] == '\n') {
            int len = i - prev + 1;
            char *buf = malloc(len + 10);
            strcpy(buf, header);
            strncpy(buf + 9, orig->buf + prev, len);
            for (int i = 0; i < MAXN; i++) {
                if (i == orig->client_id || !online[i]) continue;
                char *sendbuf = strndup(buf, len + 9);
                add_send_request(ring, i, sendbuf, len + 9);
            }
            free(buf);
            prev = i + 1;
        }
    if (prev != orig->len) {
        int len = orig->len - prev;
        char *buf = malloc(len + 10);
        strcpy(buf, header);
        strncpy(buf + 9, orig->buf, len);
        for (int i = 0; i < MAXN; i++) {
            if (i == orig->client_id || !online[i]) continue;
            char *sendbuf = strndup(buf, len + 9);
            add_send_request(ring, i, sendbuf, len + 9);
        }
        free(buf);
    }
}

int main(int argc, char **argv) {
    int port = atoi(argv[1]);
    int fd;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    socklen_t addr_len = sizeof(addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("bind");
        return 1;
    }
    if (listen(fd, 32)) {
        perror("listen");
        return 1;
    }

    struct io_uring ring;
    io_uring_queue_init(256, &ring, 0);
    struct io_uring_cqe *cqe;       // completion queue entry

    // server loop
    add_accept_request(&ring, fd);
    while (true) {
        int ret = io_uring_wait_cqe(&ring, &cqe);
        struct req_tag *tag = (struct req_tag *)cqe->user_data;
        if (tag == NULL) {
            // was a close operation
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            exit(1);
        }
        if (cqe->res < 0) {
            fprintf(stderr, "Async request failed: %s for event: %d\n",
                    strerror(-cqe->res), tag->event_type);
            // exit(1);
        }
        switch (tag->event_type) {
        case ACCEPT: {
            bool served = false;
            int client_id = -1;
            for (int i = 0; i < MAXN; i++) {
                if (!online[i]) {
                    client_fds[i] = cqe->res;
                    online[i] = true;
                    client_id = i;
                    served = true;
                    break;
                }
            }
            if (served) {
                add_recv_request(&ring, client_id, BUFLEN);
            } else {
                add_close_request(&ring, cqe->res);
            }
            add_accept_request(&ring, fd);
            break;
        }
        case RECV:
            if (!cqe->res) {
                // read zero bytes, client disconnect
                add_close_request(&ring, client_fds[tag->client_id]);
                online[tag->client_id] = false;
            } else {
                tag->len = cqe->res;    // actual message length
                broadcast_messages(&ring, tag);
                add_recv_request(&ring, tag->client_id, BUFLEN);
            }
            free(tag->buf);
            break;
        case SEND:
            // send completed, free the buffers
            free(tag->buf);
            break;
        }
        free(tag);
        io_uring_cqe_seen(&ring, cqe);
    }
    return 0;
}
