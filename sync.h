#ifndef RESOURCE_AWARE_SYNC_H
#define RESOURCE_AWARE_SYNC_H

#include <deque>

namespace runtime {

class Mutex {
public:
    Mutex();
    void lock();
    bool try_lock();
    void unlock();

private:
    int owner_task_id_;
    std::deque<int> waiters_;
};

class Semaphore {
public:
    explicit Semaphore(int initial_count = 0);
    void wait();
    void signal();
    int value() const;

private:
    int count_;
    std::deque<int> waiters_;
};

} // namespace runtime

#endif // RESOURCE_AWARE_SYNC_H
