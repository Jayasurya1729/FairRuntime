#ifndef RESOURCE_AWARE_SCHEDULER_H
#define RESOURCE_AWARE_SCHEDULER_H

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <ucontext.h>

#include "allocator.h"
#include "policy.h"
#include "resource_manager.h"
#include "telemetry.h"

namespace runtime {

struct TaskControlBlock {
    int                task_id = -1;
    std::string        name;
    TaskStatus         status = TaskStatus::Ready;
    ucontext_t         context = {};
    std::function<void()> body;
    uint64_t           vruntime_ns = 0;
    uint64_t           total_run_ns = 0;
    uint64_t           last_run_ns = 0; // duration, in ns, of this task's most recently completed slice
    uint64_t           slice_start_ns = 0;
    int                nice = 0;
    uint64_t           weight = 1024;
    size_t             allocation_bytes = 0;
    size_t             allocation_ops = 0;
    size_t             free_ops = 0;
    size_t             yield_count = 0;
    bool               context_initialized = false;
    bool               throttled = false; // tracks last-logged resource-manager throttle state, to avoid re-logging every tick
    static constexpr size_t STACK_SIZE = 256 * 1024;
    std::vector<char>  stack;

    TaskControlBlock();
};

class Scheduler {
public:
    Scheduler(SchedulingPolicy policy, ResourceManager* resource_manager = nullptr);
    ~Scheduler();

    int create_task(const std::string& name, std::function<void()> body, int nice = 0);
    void run();
    void yield();

    TaskControlBlock* current_task();
    const TaskControlBlock* current_task() const;
    static Scheduler* instance();
    int current_task_id() const;

    void set_policy(SchedulingPolicy policy);
    SchedulingPolicy policy() const;

private:
    TaskControlBlock* select_next_task();
    void prepare_task_context(TaskControlBlock& task);
    void handle_task_exit();
    void update_task_runtime(TaskControlBlock& task, uint64_t delta_ns);
    uint64_t get_current_time_ns() const;
    uint64_t weight_from_nice(int nice) const;

    static void task_trampoline();

    ucontext_t scheduler_context_;
    std::vector<std::unique_ptr<TaskControlBlock>> tasks_;
    TaskControlBlock* current_task_ = nullptr;
    SchedulingPolicy policy_;
    ResourceManager* resource_manager_ = nullptr;
    Telemetry telemetry_;
    static Scheduler* instance_;

    struct TaskVruntimeComparator {
        bool operator()(const TaskControlBlock* lhs, const TaskControlBlock* rhs) const {
            if (lhs->vruntime_ns != rhs->vruntime_ns) {
                return lhs->vruntime_ns < rhs->vruntime_ns;
            }
            return lhs->task_id < rhs->task_id;
        }
    };

    std::set<TaskControlBlock*, TaskVruntimeComparator> ready_tree_;

    void insert_ready_task(TaskControlBlock* task);
    void remove_ready_task(TaskControlBlock* task);

public:
    void wake_task(int task_id);
    void block_current_task();
    void record_allocation(size_t bytes);
    void record_free(size_t bytes);
    Telemetry& telemetry();
    const Telemetry& telemetry() const;
};

} // namespace runtime

#endif // RESOURCE_AWARE_SCHEDULER_H