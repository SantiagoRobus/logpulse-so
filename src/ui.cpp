#include "ui.h"
#include "parser.h"

#include <ncurses.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>

namespace {

std::string get_parent_dir(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string get_basename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

void drain_fd_lines(int fd, std::string& carry, BoundedBuffer& buffer) {
    char buf[16384];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            carry.append(buf, static_cast<size_t>(n));
            size_t start = 0;
            size_t nl;
            while ((nl = carry.find('\n', start)) != std::string::npos) {
                LogItem item;
                item.line = carry.substr(start, nl - start);
                item.is_poison_pill = false;
                buffer.push(std::move(item));
                start = nl + 1;
            }
            if (start > 0) {
                carry.erase(0, start);
            }
        } else {
            break;
        }
    }
}

} // namespace

bool read_live_inotify(const std::string& filepath,
                       BoundedBuffer& buffer,
                       std::atomic<bool>* stop_flag,
                       std::atomic<bool>* pause_flag) {
    int inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        perror("inotify_init1");
        return false;
    }

    std::string parent_dir = get_parent_dir(filepath);
    std::string target_name = get_basename(filepath);

    int dir_wd = inotify_add_watch(inotify_fd, parent_dir.c_str(),
                                   IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
    if (dir_wd < 0) {
        perror("inotify_add_watch dir");
        close(inotify_fd);
        return false;
    }

    int log_fd = -1;
    int file_wd = -1;
    ino_t current_inode = 0;
    std::string carry;

    auto open_target = [&]() -> bool {
        int new_fd = open(filepath.c_str(), O_RDONLY | O_NONBLOCK);
        if (new_fd < 0) return false;
        struct stat st{};
        if (fstat(new_fd, &st) < 0) {
            close(new_fd);
            return false;
        }
        if (log_fd >= 0) close(log_fd);
        if (file_wd >= 0) inotify_rm_watch(inotify_fd, file_wd);

        log_fd = new_fd;
        current_inode = st.st_ino;
        file_wd = inotify_add_watch(inotify_fd, filepath.c_str(),
                                    IN_MODIFY | IN_MOVE_SELF | IN_DELETE_SELF);
        return true;
    };

    struct timespec wait_ts{0, 100000000L};
    while (!open_target()) {
        if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
            close(inotify_fd);
            return true;
        }
        nanosleep(&wait_ts, nullptr);
    }

    drain_fd_lines(log_fd, carry, buffer);

    struct pollfd pfd{};
    pfd.fd = inotify_fd;
    pfd.events = POLLIN;

    char ev_buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));

    while (!stop_flag || !stop_flag->load(std::memory_order_relaxed)) {
        if (pause_flag && pause_flag->load(std::memory_order_relaxed)) {
            nanosleep(&wait_ts, nullptr);
            continue;
        }

        int ret = poll(&pfd, 1, 250);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t len = read(inotify_fd, ev_buf, sizeof(ev_buf));
            if (len > 0) {
                if (log_fd >= 0) {
                    drain_fd_lines(log_fd, carry, buffer);
                }
                struct stat path_st{};
                if (stat(filepath.c_str(), &path_st) == 0 && path_st.st_ino != current_inode) {
                    if (log_fd >= 0) {
                        drain_fd_lines(log_fd, carry, buffer);
                    }
                    if (!carry.empty()) {
                        LogItem rem;
                        rem.line = std::move(carry);
                        carry.clear();
                        buffer.push(std::move(rem));
                    }
                    open_target();
                    if (log_fd >= 0) {
                        drain_fd_lines(log_fd, carry, buffer);
                    }
                }
            }
        } else if (ret == 0 && log_fd >= 0) {
            drain_fd_lines(log_fd, carry, buffer);
            struct stat path_st{};
            if (stat(filepath.c_str(), &path_st) == 0 && path_st.st_ino != current_inode) {
                drain_fd_lines(log_fd, carry, buffer);
                open_target();
                if (log_fd >= 0) drain_fd_lines(log_fd, carry, buffer);
            }
        }
    }

    if (log_fd >= 0) {
        drain_fd_lines(log_fd, carry, buffer);
        close(log_fd);
    }
    if (file_wd >= 0) inotify_rm_watch(inotify_fd, file_wd);
    inotify_rm_watch(inotify_fd, dir_wd);
    close(inotify_fd);
    return true;
}

int run_batch_read_benchmark(const std::string& filepath, int num_parsers, WorkerMetrics& out_metrics) {
    auto read_fn = [](const std::string& path,
                      BoundedBuffer& buffer,
                      std::atomic<bool>* stop_flag,
                      std::atomic<bool>*) -> bool {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        char buf[65536];
        std::string carry;
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            if (stop_flag && stop_flag->load(std::memory_order_relaxed)) break;
            carry.append(buf, static_cast<size_t>(n));
            size_t start = 0;
            size_t nl;
            while ((nl = carry.find('\n', start)) != std::string::npos) {
                LogItem item;
                item.line = carry.substr(start, nl - start);
                buffer.push(std::move(item));
                start = nl + 1;
            }
            if (start > 0) carry.erase(0, start);
        }
        if (!carry.empty()) {
            LogItem item;
            item.line = std::move(carry);
            buffer.push(std::move(item));
        }
        close(fd);
        return true;
    };

    WorkerConfig cfg;
    cfg.filepath = filepath;
    cfg.mode = RunMode::LIVE;
    cfg.num_parsers = num_parsers;
    return run_worker(cfg, &out_metrics, read_fn);
}
