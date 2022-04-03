#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BUFLEN 32

char buf[BUFLEN];

int main() {
    // long syscall(long number, ...);

    if (syscall(548, buf, BUFLEN) == -1) {
        puts("Buffer too short");
    } else {
        puts(buf);
    }
    while (1);
    return 0;
}
