#include "bounded_buffer.h"
#include <utility>

BoundedBuffer::BoundedBuffer(size_t capacity)
    : buffer_(capacity), capacity_(capacity), head_(0), tail_(0), count_(0) {
    pthread_mutex_init(&mutex_, nullptr);
    pthread_cond_init(&not_full_, nullptr);
    pthread_cond_init(&not_empty_, nullptr);
}

BoundedBuffer::~BoundedBuffer() {
    pthread_mutex_destroy(&mutex_);
    pthread_cond_destroy(&not_full_);
    pthread_cond_destroy(&not_empty_);
}

void BoundedBuffer::push(LogItem&& item) {
    pthread_mutex_lock(&mutex_);

    while (count_ == capacity_) {
        pthread_cond_wait(&not_full_, &mutex_);
    }

    buffer_[tail_] = std::move(item);
    tail_ = (tail_ + 1) % capacity_;
    count_++;

    pthread_cond_signal(&not_empty_);
    pthread_mutex_unlock(&mutex_);
}

LogItem BoundedBuffer::pop() {
    pthread_mutex_lock(&mutex_);

    while (count_ == 0) {
        pthread_cond_wait(&not_empty_, &mutex_);
    }

    LogItem item = std::move(buffer_[head_]);
    head_ = (head_ + 1) % capacity_;
    count_--;

    pthread_cond_signal(&not_full_);
    pthread_mutex_unlock(&mutex_);

    return item;
}

void BoundedBuffer::send_poison_pills(int num_consumers) {
    for (int i = 0; i < num_consumers; ++i) {
        LogItem pill;
        pill.is_poison_pill = true;
        push(std::move(pill));
    }
}
