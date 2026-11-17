#ifndef PARSER_H
#define PARSER_H

#include "common.h"
#include <string>
#include <string_view>
#include <unordered_map>

// Estructura acumulada por cada hilo parseador (evita locks por cada línea)
struct LocalStats {
    uint64_t total_requests = 0;
    uint64_t malformed_lines = 0;
    uint64_t total_bytes = 0;

    uint64_t status_2xx = 0;
    uint64_t status_3xx = 0;
    uint64_t status_4xx = 0;
    uint64_t status_5xx = 0;

    std::unordered_map<std::string, uint64_t> ip_counts;
    std::unordered_map<std::string, uint64_t> url_counts;

    struct LatencyAcc {
        uint64_t count = 0;
        double total_time = 0.0;
    };
    std::unordered_map<std::string, LatencyAcc> url_latencies;

    void merge_from(const LocalStats& other);
    void export_to_shared(WorkerMetrics& out) const;
};

// Parsea una línea cruda del log y actualiza stats. Retorna true si fue válida, false si mal formada.
bool parse_log_line(std::string_view line, LocalStats& stats);

#endif // PARSER_H
