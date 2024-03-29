#define USE_CUSTOM_HISTORY

#include <map>
#include <string>
#include <vector>

#include <fstream>
#include <sstream>
#include <iostream>

#include <cctype>
#include <cassert>
#include <climits>
#include <cstring>

#include <pwd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <termios.h>
#include <sys/wait.h>
#include <sys/types.h>

#include <readline/readline.h>  // for GNU Readline

#ifndef USE_CUSTOM_HISTORY
#include <readline/history.h>   // for GNU history
#else
std::vector<std::string> history_lines;
int current_position_in_history = 0;
void my_readhist(const char *fname);
int my_writehist(const char *fname);
int arrow_function(int, int);
int arrow_function(int, int);
#endif

std::string expand_hist(std::string);

std::string sanitize(std::string);
std::vector<std::string> split(std::string s, const std::string &delimiter);

void fork_and_exec(std::vector<std::string> &args);
void execute_with_pipe(std::vector<std::string> &args);

sigjmp_buf ctrlc_buf;
static void sigintHandler(int sig) {
    // Ctrl-C sends SIGINT to the whole process group
    // bash 处理 Ctrl-C 的方法是让子进程 setpgid 脱离进程组。
    // see: SETPGID(2), TCSETPGRP(3)
    // see: https://unix.stackexchange.com/questions/500922/how-ctrl-c-is-working-inside-the-shell
    // the subprocess should receive SIGINT as usual
    // the shell should wait for the subprocess to end
    // and continue for new loop
    // Caveat C++ library routines are usually OS-agnostic,
    //        thus will mask the effect of EINTR
    // write(STDERR_FILENO, "Caught SIGINT!\n", 15);
    if (sig == SIGINT) {
        // printf("You pressed Ctrl+C\n");
        write(STDERR_FILENO, "\n", 1);
        siglongjmp(ctrlc_buf, 1);
    }
}

int main() {
    std::ios::sync_with_stdio(false);

    struct sigaction new_action, old_action;
    sigaction(SIGINT, NULL, &old_action);
    old_action.sa_flags &= ~SA_RESTART;     // make getline fail with EINTR on SIGINT
    old_action.sa_handler = sigintHandler;

    if (sigaction(SIGINT, &old_action, NULL) < 0) {
        std::cerr << "cannot set SIGINT handler" << std::endl;
        exit(1);
    }

    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);
    const char *homedir = pw->pw_dir;

#ifndef USE_CUSTOM_HISTORY
    using_history();
    read_history("myshell_histfile");
#else
    rl_bind_keyseq("\\e[A", arrow_function);
    rl_bind_keyseq("\\e[B", arrow_function);
    my_readhist("myshell_histfile");
#endif

    std::string cmd;
    while (true) {
        // std::cout << (uid ? "$ " : "# ") << std::flush;

        // Caveat C++ library routines mask EINTR
        // had to resort to GNU readline
        // longjmp is dirty, but ... well, ok
        while (sigsetjmp(ctrlc_buf, 1) != 0);   // copied from stackoverflow https://stackoverflow.com/questions/16828378/readline-get-a-new-prompt-on-sigint
        std::cout << std::flush;                // refrain from flushing everytime (std::endl)
        current_position_in_history = history_lines.size();
        char* line = readline(uid ? "$ " : "# ");
        if (line == nullptr) cmd = "exit";
        else cmd = line;

        cmd = expand_hist(sanitize(cmd));
#ifndef USE_CUSTOM_HISTORY
        add_history(cmd.c_str());
#else
        history_lines.push_back(cmd);
#endif

        // 按空格分割命令为单词
        // TODO 可以考虑写写 escape magic
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
                std::cerr << "cd failed: " << strerror(errno) << '\n';
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

        if (args[0] == "history") {
            int idx = 0;
            if (args.size() <= 1) {
                // show all history
#ifndef USE_CUSTOM_HISTORY
                idx = history_length;
#else
                idx = history_lines.size();
#endif
            } else {
                idx = atoi(args[1].c_str());    // no error checking, sorry xD
            }
#ifndef USE_CUSTOM_HISTORY
            HIST_ENTRY **histptr = history_list();
            for (int i = history_length - idx; i < history_length; ++i) {
                printf("%5d  %s\n", i+history_base, histptr[i]->line);
            }
#else
            for (int i = history_lines.size() - idx; i < history_lines.size(); ++i) {
                printf("%5d  %s\n", i+1, history_lines[i].c_str());
            }
#endif
            continue;
        }

        if (args[0] == "exit") {
#ifndef USE_CUSTOM_HISTORY
            if (write_history("myshell_histfile") != 0) {
#else
            if (my_writehist("myshell_histfile") != 0) {
#endif
                perror("cannot write history file");
            }

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
    // struct termios settings;
    // if (tcgetattr(STDIN_FILENO, &settings) < 0) {
    //     perror("error in tcgetattr");
    //     exit(255);
    // }

    std::vector<int> cmd_idx;
    cmd_idx.push_back(0);
    char *arg_ptrs[args.size() + 1];
    bool first = true, last = false;
    std::map<int, std::string> redir_from, redir_to;
    std::map<int, bool> append;
    for (int i = 0; i < args.size(); i++) {
        arg_ptrs[i] = nullptr;
        int len = args[i].length();
        if (!len) continue;
        if (args[i] == "|") {
            first = last = false;
            append.clear();
            redir_to.clear();
            cmd_idx.push_back(i + 1);
        } else if (args[i][len - 1] == '<' || args[i][len - 1] == '>') {
            bool error = false;
            int fildes;
            char *endpos = nullptr;
            fildes = strtol(args[i].c_str(), &endpos, 10);
            if (endpos == &args[i][0] + len - 1) {
                if (*endpos == '<') {
                    // read from file
                    // only effective for the first command in the chain
                    if (!first) { continue; }
                    if (endpos == &args[i][0]) fildes = 0;
                    redir_from[fildes] = args[i+1]; ++i;
                    // TODO range check
                } else if (*endpos == '>') {
                    // write to file
                    // only effective for the last command in the chain
                    if (endpos == &args[i][0]) fildes = 1;
                    last = true; append[fildes] = 0;
                    redir_to[fildes] = args[i+1]; ++i;
                    // TODO range check
                } else {
                    error = true;
                }
            } else if (endpos == &args[i][0] + len - 2) {
                if (*endpos == '>' && *(endpos + 1) == '>') {
                    // append to file
                    // only effective for the last command in the chain
                    if (endpos == &args[i][0]) fildes = 1;
                    last = true; append[fildes] = 1;
                    redir_to[fildes] = args[i+1]; ++i;
                    // TODO range check
                } else {
                    error = true;
                }
            } else {
                error = true;
            }
            if (error) {
                std::cerr << "parsing redirection failed" << std::endl;
            }
        } else {
            arg_ptrs[i] = &args[i][0];
        }
    }
    arg_ptrs[args.size()] = nullptr;
    int cmd_count = cmd_idx.size();
    if (!last) redir_to.clear();

// #define DEBUG
#ifdef DEBUG
    if (redir_from.size()) {
        for (auto p : redir_from)
            std::cout << "#" << p.first << " input file: " << p.second << std::endl;
    }
    for (int i = 0; i < cmd_count; ++i) {
        std::cout << "cmd #" << i << ": [" << args[cmd_idx[i]];
        for (int j = cmd_idx[i] + 1; arg_ptrs[j]; ++j) {
            std::cout << ", " << arg_ptrs[j];
        }
        std::cout << "]" << std::endl;
    }
    if (redir_to.size()) {
        for (auto p : redir_to)
            std::cout << "#" << p.first << " output file: " << p.second << std::endl;
    }
#endif

    int fds[2] = {0, 1}, fds_next[2] = {0, 1};

    bool first_process = true;
    pid_t leader_pid = 0;
    for (int i = 0; i < cmd_count; ++i) {
        if (i != cmd_count - 1)
            assert(pipe(fds_next) == 0);
        pid_t pid = fork();
        if (pid == 0) {
            if (first_process) leader_pid = getpid();
            setpgid(0, leader_pid);
            if (i != 0) {
                close(0); assert(dup(fds[0]) == 0); close(fds[0]);
            } else if (redir_from.size()) {
                // input redir
                for (std::pair<int, std::string> rfile : redir_from) {
                    fds[0] = open(rfile.second.c_str(), O_RDONLY);
                    if (fds[0] < 0) {
                        std::cerr << "open redirection input file failed: " << strerror(errno) << std::endl;
                        exit(255);
                    }
                    close(rfile.first); assert(dup(fds[0]) == rfile.first); close(fds[0]);
                }
            }
            if (i != cmd_count - 1) {
                close(fds_next[0]);
                close(1); assert(dup(fds_next[1]) == 1); close(fds_next[1]);
            } else if (redir_to.size()) {
                // output redir
                int oflag = O_WRONLY | O_CREAT;
                int aflag = O_WRONLY | O_CREAT | O_APPEND;
                // Caveat open with O_CREAT must supply permission code
                for (std::pair<int, std::string> wfile : redir_to) {
                    fds_next[1] =
                        open(wfile.second.c_str(),
                             append[wfile.first] ? aflag : oflag,
                             0644);
                    if (fds_next[1] < 0) {
                        std::cerr << "open redirection output file failed: " << strerror(errno) << std::endl;
                        exit(255);
                    }
                    close(wfile.first); assert(dup(fds_next[1]) == wfile.first); close(fds_next[1]);
                }
            }
            execvp(args[cmd_idx[i]].c_str(), arg_ptrs + cmd_idx[i]);
            std::cerr << "exec " << i << "th subcommand failed: " << strerror(errno) << '\n';
            exit(255);
        }
        if (first_process) {
            leader_pid = pid;
        }
        setpgid(pid, leader_pid);       // the parent calls setpgid again to avoid timing problems
        if (first_process) {
            tcsetpgrp(STDIN_FILENO, leader_pid);
            kill(pid, SIGCONT);         // mitigate race
            first_process = false;
        }
        if (i != cmd_count - 1)
            close(fds_next[1]); // we dont need the write port anymore
        fds[0] = fds_next[0];   // pass on read port
    }

    // wait for subprocesses to finish
    int status;
    while (wait(&status) >= 0);
    signal(SIGTTOU, SIG_IGN);
    if (tcsetpgrp(STDIN_FILENO, getpgid(getpid())) < 0) {
        // 不过好像这里设置前台失败的话，怎么输出错误信息都是没法看到的吧……
        std::cerr << "set oneself as foreground process group failed: " << strerror(errno) << std::endl;
        exit(255);
    }
    signal(SIGTTOU, SIG_DFL);
    // tcsetattr(STDIN_FILENO, TCSAFLUSH, &settings);
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
        std::cerr << "exec failed: " << strerror(errno) << '\n';
        exit(255);
    }

    int status;
    int retpid = wait(&status);
    if (retpid < 0) {
        std::cerr << "wait failed: " << strerror(errno) << '\n';
    } else if (WIFEXITED(status)) {
        // child process exited
        if (WEXITSTATUS(status) == 255) {
            std::cout << "child exec failed.\n";
        }
    } else if (WIFSIGNALED(status)) {
        ;
    }
}

std::string expand_hist(std::string in) {
    if (in.length() <= 1) return in;
    std::string ret;
    // use a state machine to do the substitution...
    char prev = '\0';
    bool in_number = false;
    int now_number = 0;
    for (int i = 0; i < in.length(); ++i) {
        if (in_number) {
            if (in[i] == '!') {
                // '!!' combination
                in_number = false;
#ifndef USE_CUSTOM_HISTORY
                ret.append(history_get(history_length - 1)->line);
#else
                ret.append(*history_lines.rbegin());
#endif
            } else if (isdigit(in[i])) {
                now_number = now_number * 10 + (in[i] - '0');
            } else {
#ifndef USE_CUSTOM_HISTORY
                ret.append(history_get(now_number)->line);
#else
                ret.append(history_lines[now_number - 1]);
#endif
                ret.push_back(in[i]);
                in_number = false;
            }
        } else {
            if (in[i] == '!') {
                in_number = true;
                now_number = 0;
            } else {
                ret.push_back(in[i]);
            }
        }
    }
    if (in_number) {
        if (in[in.length() - 1] == '!') {
            ret.push_back('!');
        } else {
#ifndef USE_CUSTOM_HISTORY
            ret.append(history_get(now_number)->line);
#else
            ret.append(history_lines[now_number - 1]);
#endif
        }
    }
    return ret;
}

// remove duplicate spaces
std::string sanitize(std::string in) {
    std::string ret;
    while (in.length() && isspace(in[0]))
        in = in.substr(1);
    while (in.length() && isspace(*in.rbegin()))
        in.erase(in.size() - 1);
    if (!in.length()) return ret;
    ret.push_back(in[0]);
    for (int i = 1; i < in.length(); ++i) {
        if (isspace(in[i - 1]) && isspace(in[i])) continue;
        ret.push_back(in[i]);
    }
    return ret;
}

// 经典的 cpp string split 实现
// https://stackoverflow.com/a/14266139/11691878
std::vector<std::string> split(std::string s, const std::string &delimiter) {
    std::vector<std::string> res;
    if (s.length() == 0) return res;
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

int arrow_function(int p, int q) {
    if (q == 'A') {
        // up arrow
        if (current_position_in_history != 0) {
            rl_delete_text(0, rl_end);
            rl_point = 0;
            current_position_in_history--;
            rl_insert_text(history_lines[current_position_in_history].c_str());
        }
    } else {
        // down arrow
        if (current_position_in_history < history_lines.size() - 1) {
            rl_delete_text(0, rl_end);
            rl_point = 0;
            current_position_in_history++;
            rl_insert_text(history_lines[current_position_in_history].c_str());
        }
    }
    return 0;
}

void my_readhist(const char *fname) {
    std::ifstream fin(fname);
    std::string str;
    if (!fin) return;
    while (std::getline(fin, str)) {
        history_lines.push_back(str);
    }
    current_position_in_history = history_lines.size();
}

int my_writehist(const char *fname) {
    std::ofstream fout(fname);
    if (!fout) return -1;
    for (auto s : history_lines) {
        fout << s << std::endl;
    }
    return 0;
}