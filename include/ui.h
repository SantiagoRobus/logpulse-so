#ifndef UI_H
#define UI_H

#include "common.h"
#include "bounded_buffer.h"
#include "worker.h"
#include <vector>
#include <string>
#include <atomic>
#include <semaphore.h>

// Estructura completa alojada en Memoria Compartida POSIX
struct SharedRegion {
    sem_t sem;
    int num_workers;
    WorkerMetrics workers[]; // Flexible array member: 1 por archivo
};

// Función lectora en vivo con inotify y manejo de rotación por inodo (Integrante 4)
bool read_live_inotify(const std::string& filepath,
                       BoundedBuffer& buffer,
                       std::atomic<bool>* stop_flag,
                       std::atomic<bool>* pause_flag);

// Lector alternativo con read() para comparativa de fallos de página (Integrante 4)
int run_batch_read_benchmark(const std::string& filepath, int num_parsers, WorkerMetrics& out_metrics);

// Exportación de métricas consolidadas a CSV usando llamadas POSIX open/write/close (Integrante 4)
bool export_metrics_csv(const std::string& csv_path, SharedRegion* shm, int num_workers);

// Bucle principal de la interfaz ncurses (Integrante 4)
void run_ncurses_ui(SharedRegion* shm,
                    int num_workers,
                    std::atomic<bool>& global_stop,
                    std::atomic<bool>& global_pause,
                    const std::string& mode_str,
                    int threads_per_worker);

#endif // UI_H
