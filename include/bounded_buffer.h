#ifndef BOUNDED_BUFFER_H
#define BOUNDED_BUFFER_H

#include "common.h"
#include <pthread.h>
#include <string>
#include <vector>

struct LogItem {
    std::string line;
    bool is_poison_pill = false;
};

class BoundedBuffer {
public:
    explicit BoundedBuffer(size_t capacity = BUFFER_CAPACITY);
    ~BoundedBuffer();

    BoundedBuffer(const BoundedBuffer&) = delete;
    BoundedBuffer& operator=(const BoundedBuffer&) = delete;

    // Inserta un elemento; bloquea con pthread_cond_wait si está lleno
    void push(LogItem&& item);

    // Extrae un elemento; bloquea con pthread_cond_wait si está vacío
    LogItem pop();

    // Inserta num_consumers píldoras venenosas para terminación limpia
    void send_poison_pills(int num_consumers);

private:
    std::vector<LogItem> buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    size_t count_;

    pthread_mutex_t mutex_;
    pthread_cond_t not_full_;
    pthread_cond_t not_empty_;
};

#endif // BOUNDED_BUFFER_H
