#include "parser.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdlib>

bool parse_log_line(std::string_view line, LocalStats& stats) {
    // Eliminar salto de línea final si existe
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return true;

    // 1. Extraer IP (hasta el primer espacio)
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos || sp1 == 0 || sp1 >= MAX_IP_LEN) {
        stats.malformed_lines++;
        return false;
    }
    std::string_view ip = line.substr(0, sp1);

    // 2. Buscar bloque de fecha entre corchetes [ ... ]
    size_t bracket_open = line.find('[', sp1);
    size_t bracket_close = line.find(']', bracket_open == std::string_view::npos ? 0 : bracket_open);
    if (bracket_open == std::string_view::npos || bracket_close == std::string_view::npos) {
        stats.malformed_lines++;
        return false;
    }

    // 3. Buscar petición entre comillas "METODO /url?query HTTP/1.1"
    size_t q1 = line.find('"', bracket_close);
    size_t q2 = (q1 != std::string_view::npos) ? line.find('"', q1 + 1) : std::string_view::npos;
    if (q1 == std::string_view::npos || q2 == std::string_view::npos) {
        stats.malformed_lines++;
        return false;
    }

    std::string_view request = line.substr(q1 + 1, q2 - q1 - 1);
    size_t req_sp1 = request.find(' ');
    if (req_sp1 == std::string_view::npos) {
        stats.malformed_lines++;
        return false;
    }
    std::string_view rest_req = request.substr(req_sp1 + 1);
    size_t req_sp2 = rest_req.find(' ');
    std::string_view full_url = (req_sp2 == std::string_view::npos) ? rest_req : rest_req.substr(0, req_sp2);

    if (full_url.empty()) {
        stats.malformed_lines++;
        return false;
    }

    // Limpiar cadena de consulta (?...) según Sección 10
    size_t qmark = full_url.find('?');
    std::string_view clean_url = (qmark == std::string_view::npos) ? full_url : full_url.substr(0, qmark);

    // 4. Extraer código de estado y bytes después de q2
    size_t pos = q2 + 1;
    while (pos < line.size() && line[pos] == ' ') pos++;
    size_t sp_status = line.find(' ', pos);
    if (sp_status == std::string_view::npos) {
        stats.malformed_lines++;
        return false;
    }

    int status_code = 0;
    for (size_t i = pos; i < sp_status; ++i) {
        if (line[i] < '0' || line[i] > '9') {
            stats.malformed_lines++;
            return false;
        }
        status_code = status_code * 10 + (line[i] - '0');
    }

    pos = sp_status + 1;
    while (pos < line.size() && line[pos] == ' ') pos++;
    size_t sp_bytes = line.find(' ', pos);
    std::string_view bytes_sv = (sp_bytes == std::string_view::npos) ? line.substr(pos) : line.substr(pos, sp_bytes - pos);

    uint64_t bytes = 0;
    if (!bytes_sv.empty() && bytes_sv != "-") {
        for (char c : bytes_sv) {
            if (c < '0' || c > '9') {
                stats.malformed_lines++;
                return false;
            }
            bytes = bytes * 10 + (c - '0');
        }
    }

    // 5. Extraer tiempo de respuesta opcional al final de la línea
    bool has_resp_time = false;
    double resp_time = 0.0;
    if (sp_bytes != std::string_view::npos) {
        size_t last_sp = line.rfind(' ');
        if (last_sp != std::string_view::npos && last_sp > sp_bytes) {
            std::string_view last_tok = line.substr(last_sp + 1);
            // Si no termina en comilla, debe ser el número flotante del tiempo de respuesta
            if (!last_tok.empty() && last_tok.back() != '"') {
                char buf[64];
                if (last_tok.size() < sizeof(buf)) {
                    std::memcpy(buf, last_tok.data(), last_tok.size());
                    buf[last_tok.size()] = '\0';
                    char* endptr = nullptr;
                    double val = std::strtod(buf, &endptr);
                    if (endptr != buf && *endptr == '\0' && val >= 0.0) {
                        has_resp_time = true;
                        resp_time = val;
                    }
                }
            }
        }
    }

    // 6. Actualizar métricas locales
    stats.total_requests++;
    stats.total_bytes += bytes;

    if (status_code >= 200 && status_code < 300) stats.status_2xx++;
    else if (status_code >= 300 && status_code < 400) stats.status_3xx++;
    else if (status_code >= 400 && status_code < 500) stats.status_4xx++;
    else if (status_code >= 500 && status_code < 600) stats.status_5xx++;

    std::string ip_str(ip);
    std::string url_str(clean_url);

    stats.ip_counts[ip_str]++;
    stats.url_counts[url_str]++;

    if (has_resp_time) {
        auto& acc = stats.url_latencies[url_str];
        acc.count++;
        acc.total_time += resp_time;
    }

    return true;
}

void LocalStats::merge_from(const LocalStats& other) {
    total_requests += other.total_requests;
    malformed_lines += other.malformed_lines;
    total_bytes += other.total_bytes;
    status_2xx += other.status_2xx;
    status_3xx += other.status_3xx;
    status_4xx += other.status_4xx;
    status_5xx += other.status_5xx;

    for (const auto& [ip, cnt] : other.ip_counts) {
        ip_counts[ip] += cnt;
    }
    for (const auto& [url, cnt] : other.url_counts) {
        url_counts[url] += cnt;
    }
    for (const auto& [url, acc] : other.url_latencies) {
        auto& dst = url_latencies[url];
        dst.count += acc.count;
        dst.total_time += acc.total_time;
    }
}

void LocalStats::export_to_shared(WorkerMetrics& out) const {
    out.total_requests = total_requests;
    out.malformed_lines = malformed_lines;
    out.total_bytes = total_bytes;
    out.status_2xx = status_2xx;
    out.status_3xx = status_3xx;
    out.status_4xx = status_4xx;
    out.status_5xx = status_5xx;

    // Top 10 IPs
    std::vector<std::pair<std::string, uint64_t>> ips(ip_counts.begin(), ip_counts.end());
    std::partial_sort(ips.begin(), ips.begin() + std::min(TOP_N, ips.size()), ips.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    std::memset(out.top_ips, 0, sizeof(out.top_ips));
    for (size_t i = 0; i < std::min(TOP_N, ips.size()); ++i) {
        std::strncpy(out.top_ips[i].ip, ips[i].first.c_str(), MAX_IP_LEN - 1);
        out.top_ips[i].count = ips[i].second;
    }

    // Top 10 URLs más solicitadas
    std::vector<std::pair<std::string, uint64_t>> urls(url_counts.begin(), url_counts.end());
    std::partial_sort(urls.begin(), urls.begin() + std::min(TOP_N, urls.size()), urls.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
    std::memset(out.top_urls, 0, sizeof(out.top_urls));
    for (size_t i = 0; i < std::min(TOP_N, urls.size()); ++i) {
        std::strncpy(out.top_urls[i].url, urls[i].first.c_str(), MAX_URL_LEN - 1);
        out.top_urls[i].count = urls[i].second;
    }

    // Top 10 URLs con mayor tiempo de respuesta promedio
    struct SlowItem {
        std::string url;
        uint64_t count;
        double total;
        double avg;
    };
    std::vector<SlowItem> slow;
    slow.reserve(url_latencies.size());
    for (const auto& [url, acc] : url_latencies) {
        if (acc.count > 0) {
            slow.push_back({url, acc.count, acc.total_time, acc.total_time / acc.count});
        }
    }
    std::partial_sort(slow.begin(), slow.begin() + std::min(TOP_N, slow.size()), slow.end(),
                      [](const auto& a, const auto& b) { return a.avg > b.avg; });
    std::memset(out.top_slow_urls, 0, sizeof(out.top_slow_urls));
    for (size_t i = 0; i < std::min(TOP_N, slow.size()); ++i) {
        std::strncpy(out.top_slow_urls[i].url, slow[i].url.c_str(), MAX_URL_LEN - 1);
        out.top_slow_urls[i].count = slow[i].count;
        out.top_slow_urls[i].total_time = slow[i].total;
        out.top_slow_urls[i].avg_time = slow[i].avg;
    }
}
