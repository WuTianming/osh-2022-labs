#include <cstdio>
#include <cstdlib>

#include <unistd.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/ptrace.h>

int main(int argc, char **argv) {
    pid_t pid = fork();
    switch (pid) {
    case -1:
        exit(1);
    case 0:
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execvp(argv[1], argv + 1);
        exit(1);
    }
    waitpid(pid, 0, 0);
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_EXITKILL);

    struct user_regs_struct regs;
    long syscall;
    int status;

    while (true) {
        // entering syscall
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) exit(0);
        ptrace(PTRACE_GETREGS, pid, 0, &regs);
        syscall = regs.orig_rax;
        fprintf(stderr, "%ld(%ld, %ld, %ld, %ld, %ld, %ld)", syscall,
                (long)regs.rdi, (long)regs.rsi, (long)regs.rdx,
                (long)regs.r10, (long)regs.r8, (long)regs.r9);

        // leaving syscall
        ptrace(PTRACE_SYSCALL, pid, 0, 0);
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) exit(0);
        ptrace(PTRACE_GETREGS, pid, 0, &regs);
        fprintf(stderr, " = %ld\n", (long)regs.rax);
    }
    return 0;
}