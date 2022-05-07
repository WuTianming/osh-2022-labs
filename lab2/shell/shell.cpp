#include <iostream> // IO
#include <string> // std::string
#include <vector> // std::vector
#include <sstream> // std::string 转 int
#include <climits> // PATH_MAX 等常量
#include <unistd.h> // POSIX API
#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h> // wait
#include <sys/types.h>
#include <cstring>
#include <cassert>

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
    char *arg_ptrs[args.size() + 1];
    bool first = true, last = false, append = false;
    std::string redir_from, redir_to;
    for (int i = 0; i < args.size(); i++) {
        if (args[i] == "|") {
            first = last = append = false;
            arg_ptrs[i] = nullptr;
            cmd_idx.push_back(i + 1);
        } else if (args[i] == "<") {
            // read from file
            // only effective for the first command in the chain
            arg_ptrs[i] = nullptr;
            if (!first) { continue; }
            redir_from = args[i+1]; ++i;        // TODO range check
        } else if (args[i] == ">") {
            // write to file
            // only effective for the last command in the chain
            arg_ptrs[i] = nullptr;
            last = true; append = false;
            redir_to = args[i+1]; ++i;          // TODO range check
        } else if (args[i] == ">>") {
            // append to file
            // only effective for the last command in the chain
            arg_ptrs[i] = nullptr;
            last = true; append = true;
            redir_to = args[i+1]; ++i;          // TODO range check
        } else {
            arg_ptrs[i] = &args[i][0];
        }
    }
    arg_ptrs[args.size()] = nullptr;
    int cmd_count = cmd_idx.size();
    if (!last) redir_to = "";

// #define DEBUG
#ifdef DEBUG
    if (redir_from.length()) {
        std::cout << "global input file: " << redir_from << std::endl;
    }
    for (int i = 0; i < cmd_count; ++i) {
        std::cout << "cmd #" << i << ": [" << args[cmd_idx[i]];
        for (int j = cmd_idx[i] + 1; arg_ptrs[j]; ++j) {
            std::cout << ", " << arg_ptrs[j];
        }
        std::cout << "]" << std::endl;
    }
    if (redir_to.length()) {
        std::cout << "global output file: " << redir_to << std::endl;
    }
#endif

    int fds[2] = {0, 1}, fds_next[2] = {0, 1};

    for (int i = 0; i < cmd_count; ++i) {
        if (i != cmd_count - 1)
            assert(pipe(fds_next) == 0);
        pid_t pid = fork();
        if (pid == 0) {
            if (i != 0) {
                close(0); assert(dup(fds[0]) == 0); close(fds[0]);
            } else if (redir_from.length()) {
                // input redir
                fds[0] = open(redir_from.c_str(), O_RDONLY);
                if (fds[0] < 0) {
                    std::cerr << "open redirection input file failed: " << strerror(errno) << std::endl;
                    exit(255);
                }
                close(0); assert(dup(fds[0]) == 0); close(fds[0]);
            }
            if (i != cmd_count - 1) {
                close(fds_next[0]);
                close(1); assert(dup(fds_next[1]) == 1); close(fds_next[1]);
            } else if (redir_to.length()) {
                // output redir
                int oflag = O_WRONLY | O_CREAT;
                if (append) oflag |= O_APPEND;
                // Caveat open with O_CREAT must supply permission code
                fds_next[1] = open(redir_to.c_str(), oflag, 0644);
                if (fds_next[1] < 0) {
                    std::cerr << "open redirection input file failed: " << strerror(errno) << std::endl;
                }
                close(1); assert(dup(fds_next[1]) == 1); close(fds_next[1]);
            }
            execvp(args[cmd_idx[i]].c_str(), arg_ptrs + cmd_idx[i]);
            std::cerr << "exec " << i << "th subcommand failed: " << strerror(errno) << '\n';
            exit(255);
        }
        if (i != cmd_count - 1)
            close(fds_next[1]); // we dont need the write port anymore
        fds[0] = fds_next[0];   // pass on read port
    }

    // wait for subprocesses to finish
    int status;
    int retpid;
    while (true) {
        retpid = wait(&status);
        if (retpid < 0) {
            // std::cerr << "wait failed: " << strerror(errno) << '\n';
            // all subprocesses have exited
            return;
        }
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
