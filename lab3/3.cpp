#include <cstdio>
#include <string>       // 摆烂了，zero copy 不可能的，string 满天飞预定……
#include <cstring>
#include <cstdlib>
#include <queue>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <errno.h>

using namespace std;

const int MAXN = 32;

const char header[] = "Message: ";

bool online[MAXN];
int fd_client[MAXN];

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

// for non-blocking send calls, message queues are still necessary
queue<string> msg_queue[MAXN];          // already segmented with newline character
size_t resume_pos[MAXN];
ssize_t async_send(int id) {
    // for non-blocking send, when the buffer is full send() returns EWOULDBLOCK
    // and the send operation must be suspended until select() returns writable again
    // this function deals with suspending & resuming the transaction
    if (msg_queue[id].empty()) return 0;
    size_t sent = resume_pos[id];
    size_t n = msg_queue[id].front().length();
    const char *buf = msg_queue[id].front().c_str();
    while (sent < n) {
        ssize_t ret = send(fd_client[id], buf + sent, n, 0);    // may cause SIGPIPE
        if (ret <= 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                // printf("client disconnected\n");
                close(fd_client[id]);
                return -1;
            } else {
                resume_pos[id] = sent;
                return sent;
            }
        } else {
            sent += ret;
        }
    }
    resume_pos[id] = 0;
    msg_queue[id].pop();
    return sent;
}

char buffer[1024];

void read_msg(int i) {
    ssize_t len;
    while (true) {
        if ((len = recv(fd_client[i], buffer, 1000, 0)) > 0) {
            size_t prev = 0;
            if (buffer[len - 1] != '\n') {
                buffer[len] = '\n';
                ++len;
            }
            for (size_t idx = 0; idx < len; ++idx) {
                if (buffer[idx] == '\n') {
                    char tmp = buffer[idx + 1];
                    buffer[idx + 1] = '\0';
                    string msg = string(header) + (buffer + prev);
                    buffer[idx + 1] = tmp;
                    for (int j = 0; j < MAXN; j++) {
                        if (online[j] && j != i) {
                            msg_queue[j].push(msg);
                        }
                    }
                    prev = idx + 1;
                }
            }
        } else {
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                close(fd_client[i]);
                online[i] = false;
            }
            break;
        }
    }
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int port = atoi(argv[1]);
    int fd;
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket");
        return 1;
    }
    // need the listening socket to be non-blocking
    set_nonblocking(fd);
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
    const char reject[] = "cannot accept more connections, sorry.\n";

    fd_set readable, writable;

    while (true) {
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(fd, &readable);
        int max_fd = fd;
        for (int i = 0; i < MAXN; i++) {
            if (online[i]) {
                max_fd = max(max_fd, fd_client[i]);
                FD_SET(fd_client[i], &readable);
                FD_SET(fd_client[i], &writable);
            }
        }
        if (select(max_fd + 1, &readable, &writable, NULL, NULL) > 0) {
            if (FD_ISSET(fd, &readable)) {
                // new connection
                int fd_tmp = accept(fd, NULL, NULL);
                set_nonblocking(fd_tmp);
                bool served = false;
                for (int i = 0; i < MAXN; i++) {
                    if (!online[i]) {
                        fd_client[i] = fd_tmp;
                        online[i] = true;
                        served = true;
                        break;
                    }
                }
                if (!served) {
                    // send(fd_tmp, reject, sizeof(reject) - 1, 0);
                    close(fd_tmp);
                }
            }
            for (int i = 0; i < MAXN; i++) {
                if (FD_ISSET(fd_client[i], &writable)) {
                    while (async_send(i) > 0);
                }
                if (FD_ISSET(fd_client[i], &readable)) {
                    read_msg(i);
                }
            }
        } else {
            break;
        }
    }
    return 0;
}
