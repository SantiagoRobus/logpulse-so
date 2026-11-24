#ifndef WORKER_H
#define WORKER_H

#include "common.h"
#include "bounded_buffer.h"
#include "parser.h"
#include <string>
#include <atomic>
#include <functional>
#include <pthread.h>
#include <semaphore.h>

enum class RunMode {
    BATCH, // Modo lote
    LIVE   // Modo en vivo
};

struct WorkerConfig {
    std::string filepath;
    RunMode mode = RunMode::BATCH;
    int num_parsers = DEFAULT_THREADS;
    int worker_index = 0;
    int status_pipe_fd = -1;                 // -1 si corre sin coordinador
    WorkerMetrics* shared_metrics = nullptr; // nullptr si corre sin shm
    sem_t* shm_sem = nullptr;
    std::atomic<bool>* global_stop = nullptr;
    std::atomic<bool>* global_pause = nullptr;
};

using LogReaderFn = std::function<bool(const std::string&, BoundedBuffer&, std::atomic<bool>*, std::atomic<bool>*)>;

int run_worker(const WorkerConfig& config,
               WorkerMetrics* out_local_metrics = nullptr,
               LogReaderFn custom_live_reader = nullptr);

#endif // WORKER_H
