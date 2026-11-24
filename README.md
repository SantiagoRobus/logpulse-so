# logpulse-so
Analizador concurrente de logs de servidor web en C++17 / POSIX para Sistemas Operativos (UD)

Motor ETL concurrente desarrollado en **C++17** sobre la **API POSIX de Linux** para el análisis de registros HTTP en modo lote (`mmap`) y en tiempo real (`inotify`), con arquitectura multiproceso (`fork`), memoria compartida (`shm_open`), semáforos, tuberías (`pipe`) e interfaz interactiva en terminal (`ncurses`).

## Integrantes del Grupo
* **David Santiago Rojas Bustamante** — `feat/parser-loggen` (`Makefile`, `common.h`, `loggen`, `parser`)
* **Yeferson Farjando Guzmán** — `feat/threads` (`BoundedBuffer`, pool de hilos, publicador, píldora venenosa)
* **[Nombre Integrante 3]** — `feat/ipc-proc` (Coordinador `main.cpp`, `fork`, `shm_open`, semáforos, `pipe`, `sigaction`)
* **[Nombre Integrante 4]** — `feat/io-ui` (Vigilancia `inotify` con rotación por inodo, comparativa `read()`, interfaz `ncurses`, CSV)

## Requisitos y Compilación
Requiere Linux con `g++` (C++17), `make` y `libncurses-dev`:
```bash
sudo apt update && sudo apt install -y build-essential libncurses-dev valgrind
make
