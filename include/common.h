#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <cstddef>
#include <sys/types.h>

// Constantes del sistema
constexpr size_t MAX_LINE_LEN = 1024;
constexpr size_t BUFFER_CAPACITY = 4096;
constexpr size_t MAX_IP_LEN = 46;       // Soporta IPv4 e IPv6
constexpr size_t MAX_URL_LEN = 256;
constexpr size_t TOP_N = 10;
constexpr int MIN_THREADS = 1;
constexpr int MAX_THREADS = 16;
constexpr int DEFAULT_THREADS = 4;

// Estados de un proceso trabajador
enum class WorkerState : int {
    STARTING = 0,
    RUNNING = 1,
    PAUSED = 2,
    FINISHED = 3,
    ERROR = 4,
    DEAD = 5
};

// Mensaje enviado por la tubería (pipe) de estado hacia el coordinador
struct StatusMessage {
    pid_t worker_pid;
    int worker_index;
    WorkerState state;
    char detail[128];
};

// Entradas para los rankings Top 10
struct IpStat {
    char ip[MAX_IP_LEN];
    uint64_t count;
};

struct UrlCountStat {
    char url[MAX_URL_LEN];
    uint64_t count;
};

struct UrlLatencyStat {
    char url[MAX_URL_LEN];
    uint64_t count;
    double total_time;
    double avg_time;
};

// Métricas consolidadas por archivo (reside en Memoria Compartida)
struct WorkerMetrics {
    pid_t pid;
    char filename[256];
    WorkerState state;
    bool is_poison_pill; // Uso interno si se requiere

    uint64_t total_requests;
    uint64_t malformed_lines;
    uint64_t total_bytes;

    uint64_t status_2xx;
    uint64_t status_3xx;
    uint64_t status_4xx;
    uint64_t status_5xx;

    double elapsed_seconds;
    double requests_per_sec;

    // Reporte de memoria y fallos de página (Sección 4.2)
    long minor_faults;
    long major_faults;
    long vm_rss_kb;

    IpStat top_ips[TOP_N];
    UrlCountStat top_urls[TOP_N];
    UrlLatencyStat top_slow_urls[TOP_N];
};

#endif // COMMON_H
