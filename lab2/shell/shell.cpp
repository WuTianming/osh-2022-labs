#include <iostream> // IO
#include <string> // std::string
#include <vector> // std::vector
#include <sstream> // std::string 转 int
#include <climits> // PATH_MAX 等常量
#include <unistd.h> // POSIX API
#include <pwd.h>
#include <sys/wait.h> // wait
#include <sys/types.h>
#include <cstring>
#include <assert.h>

std::vector<std::string> split(std::string s, const std::string &delimiter);
void fork_and_exec(std::vector<std::string> &args);
void execute_with_pipe(std::vector<std::string> &args);

int main() {
    std::ios::sync_with_stdio(false);
    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    const char *homedir = pw->pw_dir;

    std::string cmd;
    while (true) {
        std::cout << (uid ? "$ " : "# ") << std::flush;

        // 读入一行。std::getline 结果不包含换行符。
        std::getline(std::cin, cmd);
        if (std::cin.eof()) {
            cmd = "exit";
        }

        // 按空格分割命令为单词
        // 可以考虑写写 escape magic
        // example: echo "qwq qwq"|lolcat  ==>  echo, qwq qwq, |, lolcat
        // std::vector<std::string> args = magic_split(cmd);
        std::vector<std::string> args = split(cmd, " ");

        if (args.empty()) {
            continue;
        }

        if (args[0] == "cd") {
            if (args.size() <= 1) {
                args.push_back(homedir);
            } else if (args[1][0] == '~') {
                args[1] = std::string(homedir) + std::string(args[1].c_str() + 1);
            }

            int ret = chdir(args[1].c_str());
            if (ret < 0) {
                std::cout << "cd failed: " << strerror(errno) << '\n';
            }
            continue;
        } // cd

        if (args[0] == "pwd") {
            std::string cwd;

            // 预先分配好空间
            cwd.resize(PATH_MAX);

            // std::string to char *: &s[0]（C++17 以上可以用 s.data()）
            // std::string 保证其内存是连续的
            const char *ret = getcwd(&cwd[0], PATH_MAX);
            if (ret == nullptr) {
                std::cout << "cwd failed\n";
            } else {
                std::cout << ret << "\n";
            }
            continue;
        } // pwd

        if (args[0] == "export") {
            for (auto i = ++args.begin(); i != args.end(); i++) {
                std::string key = *i;
                std::string value;

                // std::string::npos = const max size_t
                size_t pos;
                if ((pos = i->find('=')) != std::string::npos) {
                    key = i->substr(0, pos);
                    value = i->substr(pos + 1);
                } else {
                    // if it is already assigned as internal variable,
                    //   export the variable
                }

                int ret = setenv(key.c_str(), value.c_str(), 1);
                if (ret < 0) {
                    std::cout << "export failed\n";
                }
            }
            continue;
        } // export

        if (args[0] == "exit") {
            if (args.size() <= 1) {
                return 0;
            }

            // std::string 转 int
            std::stringstream code_stream(args[1]);
            int code = 0;
            code_stream >> code;

            // 转换失败
            if (!code_stream.eof() || code_stream.fail()) {
                std::cout << "Invalid exit code\n";
                continue;
            }

            return code;
        } // exit

        // fork_and_exec(args);
        execute_with_pipe(args);
    }
}

void execute_with_pipe(std::vector<std::string> &args) {
    std::vector<int> cmd_idx;
    cmd_idx.push_back(0);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "|") {
            cmd_idx.push_back(i + 1);
        }
    }
    cmd_idx.push_back(args.size() + 1);

    // (1) setup pipe
    // (2) replace stdout and stdin (in forked processes)
    //     by closing stdin/stdout then use dup to occupy lowest fd
    // (3) exec
    int fds[2];
    pipe(fds);      // [read; write]

    pid_t pid = fork();

    char *arg_ptrs[args.size() + 1];
    if (pid == 0) {
        for (int i = cmd_idx[1]; i < cmd_idx[2] - 1; i++)
            arg_ptrs[i - cmd_idx[1]] = &args[i][0];
        arg_ptrs[cmd_idx[2] - cmd_idx[1] - 1] = nullptr;
    } else {
        for (int i = cmd_idx[0]; i < cmd_idx[1] - 1; i++)
            arg_ptrs[i - cmd_idx[0]] = &args[i][0];
        arg_ptrs[cmd_idx[1] - cmd_idx[0] - 1] = nullptr;
    }

    if (pid == 0) {
        // child process, reads
        close(0);
        assert(dup(fds[0]) == 0);
        close(fds[0]);
        execvp(args[cmd_idx[1]].c_str(), arg_ptrs);
        std::cout << "exec failed: " << strerror(errno) << '\n';
        exit(255);
    } else {
        // parent process, writes
        close(1);
        assert(dup(fds[1]) == 1);
        close(fds[1]);
        execvp(args[cmd_idx[0]].c_str(), arg_ptrs);
        std::cout << "exec failed: " << strerror(errno) << '\n';
        exit(255);
    }
}

void fork_and_exec(std::vector<std::string> &args) {
    pid_t pid = fork();

    char *arg_ptrs[args.size() + 1];
    for (int i = 0; i < args.size(); i++) {
        arg_ptrs[i] = &args[i][0];
    }
    arg_ptrs[args.size()] = nullptr;

    if (pid == 0) {
        execvp(args[0].c_str(), arg_ptrs);
        std::cout << "exec failed: " << strerror(errno) << '\n';
        exit(255);
    }

    int status;
    int retpid = wait(&status);
    if (retpid < 0) {
        std::cout << "wait failed: " << strerror(errno) << '\n';
    } else if (WIFEXITED(status)) {
        // child process exited
        if (WEXITSTATUS(status) == 255) {
            std::cout << "child exec failed.\n";
        }
    } else if (WIFSIGNALED(status)) {
        ;
    }
}

// 经典的 cpp string split 实现
// https://stackoverflow.com/a/14266139/11691878
std::vector<std::string> split(std::string s, const std::string &delimiter) {
    std::vector<std::string> res;
    size_t pos = 0;
    std::string token;
    while ((pos = s.find(delimiter)) != std::string::npos) {
        token = s.substr(0, pos);
        res.push_back(token);
        s = s.substr(pos + delimiter.length());
    }
    res.push_back(s);
    return res;
}
