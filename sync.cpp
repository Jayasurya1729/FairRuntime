#include "sync.h"
#include "scheduler.h"

namespace runtime {

Mutex::Mutex()
    : owner_task_id_(-1) {
}

void Mutex::lock() {
    Scheduler* scheduler = Scheduler::instance();
    if (!scheduler) {
        return;
    }

    int current = scheduler->current_task_id();
    if (current < 0) {
        return;
    }

    if (owner_task_id_ == -1) {
        owner_task_id_ = current;
        return;
    }

    if (owner_task_id_ == current) {
        return;
    }

    waiters_.push_back(current);
    scheduler->block_current_task();
}

bool Mutex::try_lock() {
    Scheduler* scheduler = Scheduler::instance();
    if (!scheduler) {
        return false;
    }

    int current = scheduler->current_task_id();
    if (current < 0) {
        return false;
    }

    if (owner_task_id_ == -1) {
        owner_task_id_ = current;
        return true;
    }
    return owner_task_id_ == current;
}

void Mutex::unlock() {
    Scheduler* scheduler = Scheduler::instance();
    if (owner_task_id_ == -1) {
        return;
    }

    if (!waiters_.empty()) {
        int next_task_id = waiters_.front();
        waiters_.pop_front();
        if (scheduler) {
            scheduler->wake_task(next_task_id);
        }
        owner_task_id_ = next_task_id;
    } else {
        owner_task_id_ = -1;
    }
}

Semaphore::Semaphore(int initial_count)
    : count_(initial_count) {
}

void Semaphore::wait() {
    Scheduler* scheduler = Scheduler::instance();
    if (!scheduler) {
        return;
    }

    int current = scheduler->current_task_id();
    if (current < 0) {
        return;
    }

    if (count_ > 0) {
        count_--;
        return;
    }

    waiters_.push_back(current);
    scheduler->block_current_task();
}

void Semaphore::signal() {
    if (!waiters_.empty()) {
        int next = waiters_.front();
        waiters_.pop_front();
        Scheduler* scheduler = Scheduler::instance();
        if (scheduler) {
            scheduler->wake_task(next);
        }
    } else {
        count_++;
    }
}

int Semaphore::value() const {
    return count_ - static_cast<int>(waiters_.size());
}

} // namespace runtime
