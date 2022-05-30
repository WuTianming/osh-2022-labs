#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

struct Pipe {
    int fd_send;
    int fd_recv;
};

void my_bulk_send(int fd, const void *buf, size_t n, int flags) {
    size_t sent = 0;
    while (sent < n)
        sent += send(fd, buf + sent, n - sent, flags);
}

// Pipe echoer
void *handle_chat(void *data) {
    struct Pipe *pipe = (struct Pipe *)data;
    ssize_t len;
#ifdef RAW_BINARY_TRANSFER
    char header[] = "";
    char footer[] = "";
#else
    char header[] = "Message: ";
    char footer[] = "[pending]\n";
#endif
    char recvbuffer[1024];
    while ((len = recv(pipe->fd_send, recvbuffer, 1000, 0)) > 0) {
        size_t prev = 0;
        for (size_t idx = 0; idx < len; ++idx) {
            if (recvbuffer[idx] == '\n') {
                my_bulk_send(pipe->fd_recv, header, sizeof(header) - 1, 0);
                my_bulk_send(pipe->fd_recv, recvbuffer + prev, idx - prev + 1, 0);
                prev = idx + 1;
            }
        }
        if (prev != len) {
            // message truncated because of stream transfer
            // there is no way to know if this message is lower half of another
            my_bulk_send(pipe->fd_recv, header, sizeof(header) - 1, 0);
            my_bulk_send(pipe->fd_recv, recvbuffer + prev, len - prev, 0);
            my_bulk_send(pipe->fd_recv, footer, sizeof(footer) - 1, 0);
        }
    }
    return NULL;
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
    int fd1 = accept(fd, NULL, NULL);
    int fd2 = accept(fd, NULL, NULL);
    if (fd1 == -1 || fd2 == -1) {
        perror("accept");
        return 1;
    }
    pthread_t thread1, thread2;
    struct Pipe pipe1;
    struct Pipe pipe2;
    pipe1.fd_send = fd1;
    pipe1.fd_recv = fd2;
    pipe2.fd_send = fd2;
    pipe2.fd_recv = fd1;
    pthread_create(&thread1, NULL, handle_chat, (void *)&pipe1);
    pthread_create(&thread2, NULL, handle_chat, (void *)&pipe2);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    return 0;
}
