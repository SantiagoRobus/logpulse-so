#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <cstring>
#include <ctime>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static volatile sig_atomic_t keep_running = 1;

void handle_sigint(int) {
    keep_running = 0;
}

std::string get_current_timestamp() {
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    char buf[64];
    strftime(buf, sizeof(buf), "%d/%b/%Y:%H:%M:%S %z", &tm_info);
    return std::string(buf);
}

int main(int argc, char* argv[]) {
    std::string output_file = "data/access.log";
    uint64_t max_lines = 0;     // 0 = infinito (modo en vivo)
    int rate_per_sec = 0;       // 0 = máxima velocidad posible
    bool enable_rotate = false;
    uint64_t rotate_every = 5000; // Rota cada 5000 líneas si --rotar está activo

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "-n" && i + 1 < argc) {
            max_lines = std::stoull(argv[++i]);
        } else if (arg == "-r" && i + 1 < argc) {
            rate_per_sec = std::stoi(argv[++i]);
        } else if (arg == "--rotar") {
            enable_rotate = true;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Uso: " << argv[0] << " [-o archivo] [-n lineas] [-r tasa_seg] [--rotar]\n";
            return 0;
        }
    }

    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    const std::vector<std::string> ips = {
        "192.168.10.24", "10.0.0.15", "172.16.5.8", "192.168.1.101",
        "200.118.45.12", "181.54.22.9", "190.24.110.3", "8.8.8.8",
        "45.230.12.88", "186.155.10.4", "192.168.10.50", "10.10.2.99"
    };
    const std::vector<std::string> methods = {"GET", "GET", "GET", "POST", "PUT", "DELETE"};
    const std::vector<std::string> urls = {
        "/api/notas", "/index.html", "/login", "/api/usuarios",
        "/productos/detalle", "/assets/app.js", "/assets/style.css",
        "/api/pagos", "/reportes/mensual", "/health", "/api/cursos", "/perfil"
    };
    const std::vector<std::string> queries = {
        "", "?id=42", "?page=1&limit=20", "?user=admin", "?sort=asc", "?token=xyz"
    };
    const std::vector<int> statuses = {200, 200, 200, 200, 201, 301, 302, 400, 401, 403, 404, 500, 503};
    const std::vector<std::string> agents = {
        "Mozilla/5.0", "curl/8.5.0", "PostmanRuntime/7.36.0", "Chrome/128.0.0.0"
    };

    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist_ip(0, ips.size() - 1);
    std::uniform_int_distribution<size_t> dist_method(0, methods.size() - 1);
    std::uniform_int_distribution<size_t> dist_url(0, urls.size() - 1);
    std::uniform_int_distribution<size_t> dist_query(0, queries.size() - 1);
    std::uniform_int_distribution<size_t> dist_status(0, statuses.size() - 1);
    std::uniform_int_distribution<size_t> dist_agent(0, agents.size() - 1);
    std::uniform_int_distribution<int> dist_bytes(128, 65536);
    std::uniform_real_distribution<double> dist_time(0.005, 1.850);
    std::uniform_int_distribution<int> dist_anomaly(1, 100);

    int fd = open(output_file.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        perror("Error abriendo archivo de salida");
        return 1;
    }

    uint64_t generated = 0;
    uint64_t since_rotate = 0;
    std::string cached_ts = get_current_timestamp();
    std::string batch_buffer;
    batch_buffer.reserve(65536);

    while (keep_running && (max_lines == 0 || generated < max_lines)) {
        if (generated % 1000 == 0) {
            cached_ts = get_current_timestamp();
        }

        int anomaly = dist_anomaly(rng);
        char line[512];

        if (anomaly <= 2) {
            // 2% líneas mal formadas para probar robustez
            snprintf(line, sizeof(line), "CORRUPT_LOG_ENTRY_MISSING_FIELDS_%lu\n", generated);
        } else if (anomaly <= 5) {
            // 3% líneas válidas sin tiempo de respuesta final (Sección 10)
            snprintf(line, sizeof(line), "%s - - [%s] \"%s %s%s HTTP/1.1\" %d %d \"%s\"\n",
                     ips[dist_ip(rng)].c_str(),
                     cached_ts.c_str(),
                     methods[dist_method(rng)].c_str(),
                     urls[dist_url(rng)].c_str(),
                     queries[dist_query(rng)].c_str(),
                     statuses[dist_status(rng)],
                     dist_bytes(rng),
                     agents[dist_agent(rng)].c_str());
        } else {
            // 95% líneas completas estándar
            snprintf(line, sizeof(line), "%s - - [%s] \"%s %s%s HTTP/1.1\" %d %d \"-\" \"%s\" %.3f\n",
                     ips[dist_ip(rng)].c_str(),
                     cached_ts.c_str(),
                     methods[dist_method(rng)].c_str(),
                     urls[dist_url(rng)].c_str(),
                     queries[dist_query(rng)].c_str(),
                     statuses[dist_status(rng)],
                     dist_bytes(rng),
                     agents[dist_agent(rng)].c_str(),
                     dist_time(rng));
        }

        batch_buffer.append(line);
        generated++;
        since_rotate++;

        // Escribir en disco en bloques si es rápido, o línea por línea si hay tasa limitada
        if (rate_per_sec > 0 || batch_buffer.size() >= 32768 || (max_lines > 0 && generated == max_lines)) {
            ssize_t written = write(fd, batch_buffer.data(), batch_buffer.size());
            if (written < 0) {
                perror("Error escribiendo en log");
                break;
            }
            batch_buffer.clear();
        }

        // Rotación de log (cambio de inodo)
        if (enable_rotate && since_rotate >= rotate_every) {
            close(fd);
            std::string rotated_name = output_file + ".1";
            rename(output_file.c_str(), rotated_name.c_str());
            fd = open(output_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("Error recreando archivo tras rotación");
                return 1;
            }
            since_rotate = 0;
        }

        if (rate_per_sec > 0) {
            usleep(1000000 / rate_per_sec);
        }
    }

    if (!batch_buffer.empty() && fd >= 0) {
        ssize_t ignored = write(fd, batch_buffer.data(), batch_buffer.size());
        (void)ignored;
    }
    if (fd >= 0) close(fd);

    std::cout << "loggen finalizado. Total de líneas generadas: " << generated << "\n";
    return 0;
}
