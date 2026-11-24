#include "common.h"
#include "worker.h"
#include "ui.h"

#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_pause{false};
std::atomic<bool> g_sigchld_received{false};

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        g_stop.store(true, std::memory_order_relaxed);
    } else if (signum == SIGCHLD) {
        g_sigchld_received.store(true, std::memory_order_relaxed);
    }
}

struct MonitorArgs {
    SharedRegion* shm;
    int num_workers;
    std::vector<int>* pipe_read_fds;
    std::vector<pid_t>* child_pids;
};

void* status_monitor_routine(void* arg) {
    auto* args = static_cast<MonitorArgs*>(arg);
    int n = args->num_workers;

    std::vector<struct pollfd> pfds(n);
    for (int i = 0; i < n; ++i) {
        pfds[i].fd = (*args->pipe_read_fds)[i];
        pfds[i].events = POLLIN;
    }

    int open_pipes = n;
    while (!g_stop.load(std::memory_order_relaxed) && open_pipes > 0) {
        int ret = poll(pfds.data(), n, 200); // Bloqueo eficiente hasta 200 ms (sin espera activa)
        if (ret > 0) {
            for (int i = 0; i < n; ++i) {
                if (pfds[i].fd >= 0 && (pfds[i].revents & POLLIN)) {
                    StatusMessage sm{};
                    ssize_t bytes_read = read(pfds[i].fd, &sm, sizeof(sm));
                    if (bytes_read == sizeof(sm)) {
                        sem_wait(&args->shm->sem);
                        args->shm->workers[i].state = sm.state;
                        sem_post(&args->shm->sem);
                    } else if (bytes_read == 0) {
                        close(pfds[i].fd);
                        pfds[i].fd = -1;
                        open_pipes--;
                    }
                } else if (pfds[i].fd >= 0 && (pfds[i].revents & (POLLHUP | POLLERR))) {
                    close(pfds[i].fd);
                    pfds[i].fd = -1;
                    open_pipes--;
                }
            }
        }

        // Verificar si algún hijo murió abruptamente (ej. kill -9)
        if (g_sigchld_received.exchange(false, std::memory_order_relaxed)) {
            int status = 0;
            pid_t dead_pid;
            while ((dead_pid = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < n; ++i) {
                    if ((*args->child_pids)[i] == dead_pid) {
                        (*args->child_pids)[i] = -1;
                        sem_wait(&args->shm->sem);
                        if (WIFSIGNALED(status)) {
                            args->shm->workers[i].state = WorkerState::DEAD;
                        } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                            args->shm->workers[i].state = WorkerState::ERROR;
                        } else if (args->shm->workers[i].state != WorkerState::FINISHED) {
                            args->shm->workers[i].state = WorkerState::FINISHED;
                        }
                        sem_post(&args->shm->sem);
                        break;
                    }
                }
            }
        }
    }

    for (int i = 0; i < n; ++i) {
        if (pfds[i].fd >= 0) close(pfds[i].fd);
    }
    return nullptr;
}

void print_usage(const char* prog) {
    std::cerr << "Uso: " << prog << " [-m lote|vivo] [-t hilos] <archivo1.log> [archivo2.log ...]\n"
              << "Opciones:\n"
              << "  -m <modo>    Modo de operacion: 'lote' (por defecto) o 'vivo'\n"
              << "  -t <hilos>   Hilos parseadores por archivo (1..16, por defecto 4)\n";
}

} // namespace

int main(int argc, char* argv[]) {
    RunMode mode = RunMode::BATCH;
    std::string mode_str = "lote";
    int num_threads = DEFAULT_THREADS;
    std::vector<std::string> log_files;

    // 1. Parsear argumentos CLI
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-m" && i + 1 < argc) {
            mode_str = argv[++i];
            if (mode_str == "lote") {
                mode = RunMode::BATCH;
            } else if (mode_str == "vivo") {
                mode = RunMode::LIVE;
            } else {
                std::cerr << "Error: modo invalido '" << mode_str << "'. Use 'lote' o 'vivo'.\n";
                return 1;
            }
        } else if (arg == "-t" && i + 1 < argc) {
            num_threads = std::atoi(argv[++i]);
            if (num_threads < MIN_THREADS || num_threads > MAX_THREADS) {
                std::cerr << "Error: numero de hilos debe estar entre "
                          << MIN_THREADS << " y " << MAX_THREADS << ".\n";
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] == '-') {
            std::cerr << "Error: opcion desconocida '" << arg << "'.\n";
            print_usage(argv[0]);
            return 1;
        } else {
            log_files.push_back(arg);
        }
    }

    if (log_files.empty()) {
        std::cerr << "Error: debe especificar al menos un archivo de log.\n";
        print_usage(argv[0]);
        return 1;
    }

    int num_workers = static_cast<int>(log_files.size());

    // 2. Configurar manejadores de señales con sigaction (prohibido signal())
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGCHLD, &sa, nullptr);

    // 3. Crear Memoria Compartida POSIX (shm_open + ftruncate + mmap)
    std::string shm_name = "/logpulse_shm_" + std::to_string(getpid());
    size_t shm_size = sizeof(SharedRegion) + num_workers * sizeof(WorkerMetrics);

    int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    if (ftruncate(shm_fd, static_cast<off_t>(shm_size)) < 0) {
        perror("ftruncate shm");
        close(shm_fd);
        shm_unlink(shm_name.c_str());
        return 1;
    }

    void* shm_ptr = mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd); // Ya está mapeado, podemos cerrar el descriptor
    if (shm_ptr == MAP_FAILED) {
        perror("mmap shm");
        shm_unlink(shm_name.c_str());
        return 1;
    }

    auto* shm = static_cast<SharedRegion*>(shm_ptr);
    std::memset(shm, 0, shm_size);
    shm->num_workers = num_workers;

    // Semáforo POSIX entre procesos (pshared = 1)
    if (sem_init(&shm->sem, 1, 1) < 0) {
        perror("sem_init");
        munmap(shm_ptr, shm_size);
        shm_unlink(shm_name.c_str());
        return 1;
    }

    // 4. Crear tuberías (pipes) y procesos trabajadores (fork)
    std::vector<int> pipe_read_fds(num_workers, -1);
    std::vector<pid_t> child_pids(num_workers, -1);

    for (int i = 0; i < num_workers; ++i) {
        int pipefd[2];
        if (pipe(pipefd) < 0) {
            perror("pipe");
            g_stop.store(true);
            break;
        }

        std::strncpy(shm->workers[i].filename, log_files[i].c_str(), sizeof(shm->workers[i].filename) - 1);
        shm->workers[i].state = WorkerState::STARTING;

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(pipefd[0]);
            close(pipefd[1]);
            g_stop.store(true);
            break;
        }

        if (pid == 0) {
            // --- PROCESO HIJO (TRABAJADOR) ---
            close(pipefd[0]); // Cierra extremo de lectura

            // Restaurar SIGINT por defecto o manejar parada limpia en el trabajador
            struct sigaction sa_child{};
            sa_child.sa_handler = signal_handler;
            sigemptyset(&sa_child.sa_mask);
            sigaction(SIGINT, &sa_child, nullptr);
            sigaction(SIGTERM, &sa_child, nullptr);

            WorkerConfig wcfg;
            wcfg.filepath = log_files[i];
            wcfg.mode = mode;
            wcfg.num_parsers = num_threads;
            wcfg.worker_index = i;
            wcfg.status_pipe_fd = pipefd[1];
            wcfg.shared_metrics = &shm->workers[i];
            wcfg.shm_sem = &shm->sem;
            wcfg.global_stop = &g_stop;
            wcfg.global_pause = &g_pause;

            int exit_code = run_worker(wcfg, nullptr, read_live_inotify);

            close(pipefd[1]);
            munmap(shm_ptr, shm_size);
            _exit(exit_code);
        } else {
            // --- PROCESO PADRE (COORDINADOR) ---
            close(pipefd[1]); // Cierra extremo de escritura
            pipe_read_fds[i] = pipefd[0];
            child_pids[i] = pid;
            shm->workers[i].pid = pid;
        }
    }

    // 5. Lanzar hilo monitor de pipes y estados de hijos
    pthread_t monitor_thread{};
    MonitorArgs m_args{shm, num_workers, &pipe_read_fds, &child_pids};
    pthread_create(&monitor_thread, nullptr, status_monitor_routine, &m_args);

    // 6. Ejecutar interfaz de usuario en tiempo real (ncurses)
    run_ncurses_ui(shm, num_workers, g_stop, g_pause, mode_str, num_threads);

    // 7. Cierre en cascada ordenado
    g_stop.store(true, std::memory_order_relaxed);
    for (int i = 0; i < num_workers; ++i) {
        if (child_pids[i] > 0) {
            kill(child_pids[i], SIGTERM);
        }
    }

    pthread_join(monitor_thread, nullptr);

    for (int i = 0; i < num_workers; ++i) {
        if (child_pids[i] > 0) {
            waitpid(child_pids[i], nullptr, 0);
        }
    }

    // 8. Limpieza obligatoria de IPC (/dev/shm debe quedar limpio)
    sem_destroy(&shm->sem);
    munmap(shm_ptr, shm_size);
    shm_unlink(shm_name.c_str());

    return 0;
}
