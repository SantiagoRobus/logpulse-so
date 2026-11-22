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
namespace {

const char* state_to_str(WorkerState st) {
    switch (st) {
        case WorkerState::STARTING: return "INICIANDO";
        case WorkerState::RUNNING:  return "ACTIVO";
        case WorkerState::PAUSED:   return "PAUSADO";
        case WorkerState::FINISHED: return "FINALIZADO";
        case WorkerState::ERROR:    return "ERROR";
        case WorkerState::DEAD:     return "MUERTO (SIG)";
        default:                    return "DESCONOCIDO";
    }
}

} // namespace

bool export_metrics_csv(const std::string& csv_path, SharedRegion* shm, int num_workers) {
    if (!shm) return false;
    int fd = open(csv_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;

    std::vector<WorkerMetrics> snap(num_workers);
    sem_wait(&shm->sem);
    for (int i = 0; i < num_workers; ++i) snap[i] = shm->workers[i];
    sem_post(&shm->sem);

    std::string out = "archivo,pid,estado,peticiones_totales,mal_formadas,bytes_totales,http_2xx,http_3xx,http_4xx,http_5xx,req_por_seg,tiempo_s,minor_faults,major_faults,vm_rss_kb\n";
    for (int i = 0; i < num_workers; ++i) {
        const auto& w = snap[i];
        char row[1024];
        snprintf(row, sizeof(row),
                 "\"%s\",%d,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.4f,%ld,%ld,%ld\n",
                 w.filename, w.pid, state_to_str(w.state),
                 w.total_requests, w.malformed_lines, w.total_bytes,
                 w.status_2xx, w.status_3xx, w.status_4xx, w.status_5xx,
                 w.requests_per_sec, w.elapsed_seconds,
                 w.minor_faults, w.major_faults, w.vm_rss_kb);
        out += row;
    }

    ssize_t written = write(fd, out.data(), out.size());
    close(fd);
    return written == static_cast<ssize_t>(out.size());
}

void run_ncurses_ui(SharedRegion* shm,
                    int num_workers,
                    std::atomic<bool>& global_stop,
                    std::atomic<bool>& global_pause,
                    const std::string& mode_str,
                    int threads_per_worker) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        init_pair(1, COLOR_CYAN, COLOR_BLACK);
        init_pair(2, COLOR_GREEN, COLOR_BLACK);
        init_pair(3, COLOR_YELLOW, COLOR_BLACK);
        init_pair(4, COLOR_RED, COLOR_BLACK);
    }

    int selected_worker = 0;
    std::string status_banner = "Listo. Controles: [p] Pausar/Reanudar | [e] Exportar CSV | [Tab] Cambiar archivo | [q] Salir";
    std::vector<WorkerMetrics> snap(num_workers);
    struct timespec sleep_ts{0, 200000000L};

    while (!global_stop.load(std::memory_order_relaxed)) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') {
            global_stop.store(true, std::memory_order_relaxed);
            break;
        } else if (ch == 'p' || ch == 'P') {
            bool cur = global_pause.load(std::memory_order_relaxed);
            global_pause.store(!cur, std::memory_order_relaxed);
            status_banner = !cur ? "PAUSADO por usuario. Presione [p] para reanudar."
                                 : "REANUDADO. Procesando registros...";
        } else if (ch == 'e' || ch == 'E') {
            std::string csv_file = "logpulse_reporte_" + std::to_string(time(nullptr)) + ".csv";
            if (export_metrics_csv(csv_file, shm, num_workers)) {
                status_banner = "Exportado exitosamente a: " + csv_file;
            } else {
                status_banner = "Error al exportar CSV.";
            }
        } else if (ch == '\t' || ch == KEY_RIGHT) {
            selected_worker = (selected_worker + 1) % num_workers;
        } else if (ch == KEY_LEFT) {
            selected_worker = (selected_worker - 1 + num_workers) % num_workers;
        } else if (ch == KEY_RESIZE) {
            resize_term(0, 0);
        }

        sem_wait(&shm->sem);
        for (int i = 0; i < num_workers; ++i) snap[i] = shm->workers[i];
        sem_post(&shm->sem);

        erase();
        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        (void)max_x;

        attron(A_BOLD | COLOR_PAIR(1));
        mvprintw(0, 1, "LOGPULSE - Analizador Concurrente de Logs | Modo: %s | Hilos/Archivo: %d",
                 mode_str.c_str(), threads_per_worker);
        attroff(A_BOLD | COLOR_PAIR(1));

        int row = 2;
        mvprintw(row++, 1, "PROCESOS TRABAJADORES (%d):", num_workers);
        for (int i = 0; i < num_workers && row < max_y - 12; ++i) {
            const auto& w = snap[i];
            if (i == selected_worker) attron(A_REVERSE);
            mvprintw(row++, 2, "[%d] PID:%-6d | %-11s | Reqs:%-8lu | ErrFmt:%-5lu | %.1f req/s | %s",
                     i + 1, w.pid, state_to_str(w.state),
                     w.total_requests, w.malformed_lines, w.requests_per_sec, w.filename);
            if (i == selected_worker) attroff(A_REVERSE);
        }

        const auto& cur = snap[selected_worker];
        row++;
        if (row < max_y - 8) {
            attron(A_BOLD);
            mvprintw(row++, 1, "DETALLE ARCHIVO [%d]: %s (Tiempo: %.2fs | VmRSS: %ld KB | MinFlt: %ld)",
                     selected_worker + 1, cur.filename, cur.elapsed_seconds, cur.vm_rss_kb, cur.minor_faults);
            attroff(A_BOLD);
            mvprintw(row++, 2, "HTTP 2xx: %-8lu | HTTP 3xx: %-8lu | HTTP 4xx: %-8lu | HTTP 5xx: %-8lu | Bytes: %lu",
                     cur.status_2xx, cur.status_3xx, cur.status_4xx, cur.status_5xx, cur.total_bytes);
            row++;
            mvprintw(row++, 2, "TOP IPs (Top 5)                      | TOP URLs SOLICITADAS (Top 5)         | TOP URLs LENTAS (Prom. s)");
            for (int k = 0; k < 5 && row < max_y - 3; ++k) {
                mvprintw(row++, 2, "%-20s (%-7lu)     | %-24s (%-7lu) | %-20s (%.3fs)",
                         cur.top_ips[k].count > 0 ? cur.top_ips[k].ip : "-", cur.top_ips[k].count,
                         cur.top_urls[k].count > 0 ? cur.top_urls[k].url : "-", cur.top_urls[k].count,
                         cur.top_slow_urls[k].count > 0 ? cur.top_slow_urls[k].url : "-", cur.top_slow_urls[k].avg_time);
            }
        }

        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(max_y - 1, 1, "%s", status_banner.c_str());
        attroff(A_BOLD | COLOR_PAIR(3));

        refresh();
        nanosleep(&sleep_ts, nullptr);
    }
    endwin();
}
