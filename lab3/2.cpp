#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <queue>
#include <list>
#include <memory>       // for shared pointers
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>

using namespace std;

const int MAXN = 32;

const char header[] = "Message: ";

struct msg {
    const char *data;
    size_t len;
    int counter;
    msg(char *data, size_t len) : data(data), len(len), counter(0) {}
    ~msg() {
        delete[] data;
    }
};

// shared pointers provide thread-safe use_count counter
// pthread_cond_t producer_ready[MAXN];
queue<msg *> msg_queue[MAXN];
pthread_mutex_t queue_mutexes[MAXN];
sem_t msg_ready[MAXN];
pthread_t workers[MAXN][2];
bool online[MAXN];
int fd_client[MAXN];

void my_bulk_send(int fd, const char *buf, size_t n, int flags) {
    size_t sent = 0;
    while (sent < n)
        sent += send(fd, buf + sent, n - sent, flags);
}

void *receive_msg(void *__id) {
    unsigned long long id = (unsigned long long)__id;
    ssize_t len;

    const int BUFLEN = 1024;
    char *recvbuffer = new char[BUFLEN];
    list<msg *> buf_list;
    while ((len = recv(fd_client[id], recvbuffer, 1000, 0)) > 0) {
        msg *M = new msg(recvbuffer, len);
        buf_list.push_back(M);
        for (int i = 0; i < MAXN; i++)
            if (online[i] && i != id) {
                pthread_mutex_lock(&queue_mutexes[i]);
                ++M->counter;
                msg_queue[i].push(M);
                pthread_mutex_unlock(&queue_mutexes[i]);
                sem_post(&msg_ready[i]);
            }
        recvbuffer = new char[BUFLEN];
        while (!buf_list.empty() && (*(buf_list.begin()))->counter == 0) {
            msg *M = *(buf_list.begin());
            delete M;
            buf_list.pop_front();
        }
    }
    // getting 0 from recv() means client disconnect
    online[id] = false;
    sem_post(&msg_ready[id]);   // releases the semaphore so that the broadcaster exits
    return NULL;
}

void *broadcast_msg(void *__id) {
    unsigned long long id = (unsigned long long)__id;
    while (true) {
        sem_wait(&msg_ready[id]);
        if (!online[id]) break;
        if (msg_queue[id].empty()) continue;
        msg *M = msg_queue[id].front();
        size_t prev = 0;
        for (size_t idx = 0; idx < M->len; ++idx) {
            if (M->data[idx] == '\n') {
                my_bulk_send(fd_client[id], header, sizeof(header) - 1, 0);
                my_bulk_send(fd_client[id], M->data + prev, idx - prev + 1, 0);
                prev = idx + 1;
            }
        }
        if (prev != M->len) {
            my_bulk_send(fd_client[id], header, sizeof(header) - 1, 0);
            my_bulk_send(fd_client[id], M->data + prev, M->len - prev, 0);
        }
        pthread_mutex_lock(&queue_mutexes[id]);
        msg_queue[id].pop();
        --M->counter;
        pthread_mutex_unlock(&queue_mutexes[id]);
    }
    while (!msg_queue[id].empty()) {
        pthread_mutex_lock(&queue_mutexes[id]);
        msg_queue[id].pop();
        --msg_queue[id].front()->counter;
        pthread_mutex_unlock(&queue_mutexes[id]);
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
    if (listen(fd, 2)) {
        perror("listen");
        return 1;
    }
    const char reject[] = "cannot accept more connections, sorry.\n";

    for (int i = 0; i < MAXN; i++)
        pthread_mutex_init(&queue_mutexes[i], NULL);
    for (int i = 0; i < MAXN; i++)
        sem_init(&msg_ready[i], 0, 0);

    while (true) {
        // accept new connections and launch new threads
        int fd_tmp = accept(fd, NULL, NULL);
        bool served = false;
        for (int i = 0; i < MAXN; i++) {
            if (!online[i]) {
                fd_client[i] = fd_tmp;
                online[i] = true;
                pthread_create(&workers[i][1], NULL, broadcast_msg, (void *)i);
                pthread_detach(workers[i][1]);
                pthread_create(&workers[i][0], NULL, receive_msg, (void *)i);
                pthread_detach(workers[i][0]);
                served = true;
                break;
            }
        }
        if (!served) {
            my_bulk_send(fd_tmp, reject, sizeof(reject) - 1, 0);
            close(fd_tmp);
        }
    }
    return 0;
}
