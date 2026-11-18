#include "worker.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <time.h>

namespace {

struct ParserThreadArgs {
    int id;
    BoundedBuffer* buffer;
    LocalStats local_stats;
    pthread_mutex_t stats_mutex;
};

struct PublisherThreadArgs {
    const WorkerConfig* config;
    std::vector<ParserThreadArgs>* parser_args;
    std::atomic<bool>* publisher_stop;
    std::chrono::steady_clock::time_point start_time;
    struct rusage usage_before;
};

long read_vm_rss_kb() {
    std::ifstream status_file("/proc/self/status");
    std::string line;
    while (std::getline(status_file, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long rss = 0;
            if (sscanf(line.c_str() + 6, "%ld", &rss) == 1) {
                return rss;
            }
        }
    }
    return 0;
}

void build_snapshot(const WorkerConfig& config,
                    std::vector<ParserThreadArgs>& parser_args,
                    WorkerState state,
                    std::chrono::steady_clock::time_point start_time,
                    const struct rusage& usage_before,
                    WorkerMetrics& out) {
    LocalStats total_stats;
    for (auto& p : parser_args) {
        pthread_mutex_lock(&p.stats_mutex);
        total_stats.merge_from(p.local_stats);
        pthread_mutex_unlock(&p.stats_mutex);
    }

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time).count();

    struct rusage usage_now{};
    getrusage(RUSAGE_SELF, &usage_now);

    std::memset(&out, 0, sizeof(WorkerMetrics));
    out.pid = getpid();
    std::strncpy(out.filename, config.filepath.c_str(), sizeof(out.filename) - 1);
    out.state = state;
    out.elapsed_seconds = elapsed;
    out.requests_per_sec = (elapsed > 0.0) ? (total_stats.total_requests / elapsed) : 0.0;
    out.minor_faults = usage_now.ru_minflt - usage_before.ru_minflt;
    out.major_faults = usage_now.ru_majflt - usage_before.ru_majflt;
    out.vm_rss_kb = read_vm_rss_kb();

    total_stats.export_to_shared(out);
}

void* parser_thread_routine(void* arg) {
    auto* args = static_cast<ParserThreadArgs*>(arg);

    while (true) {
        LogItem item = args->buffer->pop();
        if (item.is_poison_pill) {
            break;
        }
        pthread_mutex_lock(&args->stats_mutex);
        parse_log_line(item.line, args->local_stats);
        pthread_mutex_unlock(&args->stats_mutex);
    }

    return nullptr;
}

void* publisher_thread_routine(void* arg) {
    auto* args = static_cast<PublisherThreadArgs*>(arg);

    struct timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = 300000000L; // Publica cada 300 ms

    while (!args->publisher_stop->load(std::memory_order_relaxed)) {
        nanosleep(&ts, nullptr);
        if (args->publisher_stop->load(std::memory_order_relaxed)) break;

        if (args->config->shared_metrics && args->config->shm_sem) {
            bool paused = args->config->global_pause && args->config->global_pause->load(std::memory_order_relaxed);
            WorkerState st = paused ? WorkerState::PAUSED : WorkerState::RUNNING;

            WorkerMetrics snap{};
            build_snapshot(*args->config, *args->parser_args, st, args->start_time, args->usage_before, snap);

            sem_wait(args->config->shm_sem);
            *args->config->shared_metrics = snap;
            sem_post(args->config->shm_sem);
        }
    }

    return nullptr;
}

bool default_batch_mmap_reader(const std::string& filepath,
                             BoundedBuffer& buffer,
                             std::atomic<bool>* stop_flag,
                             std::atomic<bool>* pause_flag) {
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) {
        perror("open batch log");
        return false;
    }

    struct stat st{};
    if (fstat(fd, &st) < 0) {
        perror("fstat batch log");
        close(fd);
        return false;
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size == 0) {
        close(fd);
        return true;
    }

    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap batch log");
        close(fd);
        return false;
    }

    madvise(mapped, file_size, MADV_SEQUENTIAL);

    const char* data = static_cast<const char*>(mapped);
    size_t start = 0;
    struct timespec pause_ts{0, 50000000L}; // 50 ms si está pausado

    for (size_t i = 0; i < file_size; ++i) {
        if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
            break;
        }
        while (pause_flag && pause_flag->load(std::memory_order_relaxed)) {
            if (stop_flag && stop_flag->load(std::memory_order_relaxed)) break;
            nanosleep(&pause_ts, nullptr);
        }

        if (data[i] == '\n') {
            LogItem item;
            item.line.assign(data + start, i - start);
            item.is_poison_pill = false;
            buffer.push(std::move(item));
            start = i + 1;
        }
    }

    if (start < file_size && (!stop_flag || !stop_flag->load(std::memory_order_relaxed))) {
        LogItem item;
        item.line.assign(data + start, file_size - start);
        item.is_poison_pill = false;
        buffer.push(std::move(item));
    }

    munmap(mapped, file_size);
    close(fd);
    return true;
}

void notify_status_pipe(const WorkerConfig& config, WorkerState state, const char* msg) {
    if (config.status_pipe_fd < 0) return;
    StatusMessage sm{};
    sm.worker_pid = getpid();
    sm.worker_index = config.worker_index;
    sm.state = state;
    std::strncpy(sm.detail, msg, sizeof(sm.detail) - 1);
    ssize_t ignored = write(config.status_pipe_fd, &sm, sizeof(sm));
    (void)ignored;
}

} // namespace

int run_worker(const WorkerConfig& config,
               WorkerMetrics* out_local_metrics,
               LogReaderFn custom_live_reader) {
    auto t_start = std::chrono::steady_clock::now();

    struct rusage usage_before{};
    getrusage(RUSAGE_SELF, &usage_before);

    notify_status_pipe(config, WorkerState::STARTING, "Iniciando trabajador");

    BoundedBuffer buffer(BUFFER_CAPACITY);
    int n_threads = config.num_parsers;
    if (n_threads < MIN_THREADS) n_threads = MIN_THREADS;
    if (n_threads > MAX_THREADS) n_threads = MAX_THREADS;

    std::vector<pthread_t> threads(n_threads);
    std::vector<ParserThreadArgs> thread_args(n_threads);

    for (int i = 0; i < n_threads; ++i) {
        thread_args[i].id = i;
        thread_args[i].buffer = &buffer;
        pthread_mutex_init(&thread_args[i].stats_mutex, nullptr);

        if (pthread_create(&threads[i], nullptr, parser_thread_routine, &thread_args[i]) != 0) {
            perror("pthread_create parser");
            buffer.send_poison_pills(i);
            for (int j = 0; j < i; ++j) {
                pthread_join(threads[j], nullptr);
                pthread_mutex_destroy(&thread_args[j].stats_mutex);
            }
            pthread_mutex_destroy(&thread_args[i].stats_mutex);
            notify_status_pipe(config, WorkerState::ERROR, "Fallo creando hilos parseadores");
            return 1;
        }
    }

    std::atomic<bool> publisher_stop{false};
    pthread_t publisher_thread{};
    PublisherThreadArgs pub_args{&config, &thread_args, &publisher_stop, t_start, usage_before};
    bool has_publisher = (config.shared_metrics != nullptr && config.shm_sem != nullptr);
    if (has_publisher) {
        pthread_create(&publisher_thread, nullptr, publisher_thread_routine, &pub_args);
    }

    notify_status_pipe(config, WorkerState::RUNNING, "Procesando archivo");

    bool read_ok = false;
    if (config.mode == RunMode::LIVE && custom_live_reader) {
        read_ok = custom_live_reader(config.filepath, buffer, config.global_stop, config.global_pause);
    } else {
        read_ok = default_batch_mmap_reader(config.filepath, buffer, config.global_stop, config.global_pause);
    }

    buffer.send_poison_pills(n_threads);

    for (int i = 0; i < n_threads; ++i) {
        pthread_join(threads[i], nullptr);
    }

    if (has_publisher) {
        publisher_stop.store(true, std::memory_order_relaxed);
        pthread_join(publisher_thread, nullptr);
    }

    WorkerState final_state = read_ok ? WorkerState::FINISHED : WorkerState::ERROR;
    WorkerMetrics final_metrics{};
    build_snapshot(config, thread_args, final_state, t_start, usage_before, final_metrics);

    for (int i = 0; i < n_threads; ++i) {
        pthread_mutex_destroy(&thread_args[i].stats_mutex);
    }

    if (config.shared_metrics && config.shm_sem) {
        sem_wait(config.shm_sem);
        *config.shared_metrics = final_metrics;
        sem_post(config.shm_sem);
    }

    if (out_local_metrics) {
        *out_local_metrics = final_metrics;
    }

    notify_status_pipe(config, final_state, read_ok ? "Finalizado" : "Error de lectura");
    return read_ok ? 0 : 1;
}
