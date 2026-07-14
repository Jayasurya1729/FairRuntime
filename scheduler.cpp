#include "scheduler.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iostream>
#include <sstream>

namespace runtime {

Scheduler* Scheduler::instance_ = nullptr;

TaskControlBlock::TaskControlBlock() {
    stack.resize(STACK_SIZE);
}

Scheduler::Scheduler(SchedulingPolicy policy, ResourceManager* resource_manager)
    : policy_(SchedulingPolicy::CFS), resource_manager_(resource_manager) {
    (void)policy;
    instance_ = this;
}

Scheduler::~Scheduler() {
    instance_ = nullptr;
}

int Scheduler::create_task(const std::string& name, std::function<void()> body, int nice) {
    auto task = std::make_unique<TaskControlBlock>();
    int id = static_cast<int>(tasks_.size());
    task->task_id = id;
    task->name = name;
    task->body = std::move(body);
    task->nice = nice;
    task->weight = weight_from_nice(nice);
    task->status = TaskStatus::Ready;
    task->context_initialized = false;
    task->allocation_bytes = 0;
    task->allocation_ops = 0;
    task->free_ops = 0;
    task->yield_count = 0;
    task->vruntime_ns = 0;
    task->total_run_ns = 0;
    task->last_run_ns = 0;
    task->slice_start_ns = 0;
    tasks_.push_back(std::move(task));

    TaskControlBlock* added_task = tasks_.back().get();
    insert_ready_task(added_task);
    telemetry_.register_task(id, name, nice);
    return id;
}

void Scheduler::set_policy(SchedulingPolicy policy) {
    (void)policy;
    policy_ = SchedulingPolicy::CFS;
}

SchedulingPolicy Scheduler::policy() const {
    return policy_;
}

TaskControlBlock* Scheduler::current_task() {
    return current_task_;
}

const TaskControlBlock* Scheduler::current_task() const {
    return current_task_;
}

int Scheduler::current_task_id() const {
    return current_task_ ? current_task_->task_id : -1;
}

Scheduler* Scheduler::instance() {
    return instance_;
}

uint64_t Scheduler::get_current_time_ns() const {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(high_resolution_clock::now().time_since_epoch()).count();
}

uint64_t Scheduler::weight_from_nice(int nice) const {
    static const uint64_t weights[40] = {
        88761, 71755, 56483, 46273, 36291, 29154, 23254, 18705, 14949, 11916,
        9548,  7620,  6100,  4904,  3906,  3121,  2501,  1991,  1586,  1277,
        1024,  820,   655,   526,   423,   335,   272,   215,   172,   137,
        110,   87,    70,    56,    45,    36,    29,    23,    18,    15
    };

    int index = nice + 20;
    if (index < 0) index = 0;
    if (index > 39) index = 39;
    return weights[index];
}

void Scheduler::insert_ready_task(TaskControlBlock* task)
{
    if (!task)
        return;

    if (task->status != TaskStatus::Ready)
        return;

    ready_tree_.insert(task);
}

void Scheduler::remove_ready_task(TaskControlBlock* task)
{
    if (!task)
        return;

    ready_tree_.erase(task);
}

TaskControlBlock* Scheduler::select_next_task() {
    if (ready_tree_.empty()) {
        return nullptr;
    }

    if (resource_manager_) {
        std::vector<int> active_ids;
        for (auto* task : ready_tree_) {
            active_ids.push_back(task->task_id);
        }
        resource_manager_->observe(active_ids);
    }

    // Compute the average total run time across ready tasks so each task's
    // runtime_share (its run time relative to that average) can be passed to
    // the resource manager's fairness-threshold check. Without this, the
    // fairness penalty branch in compute_adjusted_weight() could never fire.
    double average_run_ns = 0.0;
    if (!ready_tree_.empty()) {
        uint64_t total_run_ns_sum = 0;
        for (auto* task : ready_tree_) {
            total_run_ns_sum += task->total_run_ns;
        }
        average_run_ns = static_cast<double>(total_run_ns_sum) / static_cast<double>(ready_tree_.size());
    }

    TaskControlBlock* selected = nullptr;
    double best_score = 0.0;
    for (auto* task : ready_tree_) {
        double base_weight = static_cast<double>(task->weight);
        double effective_weight = base_weight;
        if (resource_manager_) {
            double runtime_share = 1.0;
            if (average_run_ns > 0.0) {
                runtime_share = static_cast<double>(task->total_run_ns) / average_run_ns;
            }

            std::vector<std::string> reasons;
            effective_weight = resource_manager_->compute_adjusted_weight(
                task->task_id, base_weight, task->allocation_bytes,
                task->allocation_ops, runtime_share, task->last_run_ns, &reasons);

            bool is_throttled_now = effective_weight < base_weight;
            if (is_throttled_now != task->throttled) {
                std::ostringstream action;
                if (is_throttled_now) {
                    action << "Throttled task " << task->task_id << " (" << task->name << ")\n"
                           << "  Reason : ";
                    for (size_t i = 0; i < reasons.size(); ++i) {
                        if (i) action << ", ";
                        action << reasons[i];
                    }
                    action << "\n  Action : Weight reduced (" << base_weight
                           << " -> " << effective_weight << ")";
                } else {
                    action << "Recovered task " << task->task_id << " (" << task->name << ")\n"
                           << "  Action : Weight restored (" << effective_weight
                           << " -> " << base_weight << ")";
                }
                telemetry_.record_policy_action(action.str());
                task->throttled = is_throttled_now;
            }
        }
        if (effective_weight < 1.0) {
            effective_weight = 1.0;
        }

        double score = static_cast<double>(task->vruntime_ns) / effective_weight;
        if (!selected || score < best_score ||
            (score == best_score && task->task_id < selected->task_id)) {
            selected = task;
            best_score = score;
        }
    }

    return selected;
}

void Scheduler::prepare_task_context(TaskControlBlock& task) {
    getcontext(&task.context);
    task.stack.assign(TaskControlBlock::STACK_SIZE, 0);
    task.context.uc_stack.ss_sp = task.stack.data();
    task.context.uc_stack.ss_size = task.stack.size();
    task.context.uc_link = &scheduler_context_;
    makecontext(&task.context, (void (*)())task_trampoline, 0);
    task.context_initialized = true;
}

void Scheduler::run() {
    if (!instance_) instance_ = this;
    getcontext(&scheduler_context_);

    while (true) {
        TaskControlBlock* next_task = select_next_task();
        if (!next_task) {
            break;
        }

        current_task_ = next_task;
        remove_ready_task(current_task_);
        current_task_->status = TaskStatus::Running;
        current_task_->slice_start_ns = get_current_time_ns();

        if (!current_task_->context_initialized) {
            prepare_task_context(*current_task_);
        }

        swapcontext(&scheduler_context_, &current_task_->context);

        if (current_task_ && current_task_->status == TaskStatus::Finished) {
            current_task_ = nullptr;
        }
    }
}

void Scheduler::block_current_task() {
    if (!current_task_) {
        return;
    }

    TaskControlBlock* task = current_task_;
    uint64_t now_ns = get_current_time_ns();
    uint64_t delta_ns = now_ns - task->slice_start_ns;
    update_task_runtime(*task, delta_ns);
    task->status = TaskStatus::Blocked;
    current_task_ = nullptr;
    swapcontext(&task->context, &scheduler_context_);
}

void Scheduler::wake_task(int task_id) {
    for (auto& task_ptr : tasks_) {
        if (task_ptr->task_id == task_id) {
            TaskControlBlock* task = task_ptr.get();
            if (task->status == TaskStatus::Blocked) {
                task->status = TaskStatus::Ready;
                insert_ready_task(task);
            }
            break;
        }
    }
}

void Scheduler::yield() {
    if (!current_task_) {
        return;
    }

    uint64_t now_ns = get_current_time_ns();
    uint64_t delta_ns = now_ns - current_task_->slice_start_ns;
    update_task_runtime(*current_task_, delta_ns);
    current_task_->yield_count++;
    telemetry_.record_task_yield(current_task_->task_id);
    current_task_->status = TaskStatus::Ready;
    insert_ready_task(current_task_);
    swapcontext(&current_task_->context, &scheduler_context_);
}

void Scheduler::update_task_runtime(TaskControlBlock& task, uint64_t delta_ns) {
    task.total_run_ns += delta_ns;
    task.last_run_ns = delta_ns;
    task.vruntime_ns += (delta_ns * 1024ULL) / std::max<uint64_t>(1, task.weight);
    telemetry_.record_task_run(task.task_id, delta_ns,task.weight);
}

void Scheduler::handle_task_exit() {
    if (!current_task_) {
        setcontext(&scheduler_context_);
        return;
    }

    uint64_t now_ns = get_current_time_ns();
    uint64_t delta_ns = now_ns - current_task_->slice_start_ns;
    update_task_runtime(*current_task_, delta_ns);
    current_task_->status = TaskStatus::Finished;
    telemetry_.record_task_finish(current_task_->task_id);
    current_task_ = nullptr;
    setcontext(&scheduler_context_);
}

void Scheduler::task_trampoline() {
    if (!instance_ || !instance_->current_task_) {
        return;
    }

    TaskControlBlock* task = instance_->current_task_;
    task->body();
    instance_->handle_task_exit();
}

void Scheduler::record_allocation(size_t bytes) {
    if (current_task_) {
        current_task_->allocation_bytes += bytes;
        current_task_->allocation_ops += 1;
        telemetry_.record_allocation(current_task_->task_id, bytes);
    }
}

void Scheduler::record_free(size_t bytes) {
    if (current_task_) {
        current_task_->free_ops += 1;
        telemetry_.record_free(current_task_->task_id, bytes);
    }
}


Telemetry& Scheduler::telemetry() {
    return telemetry_;
}

const Telemetry& Scheduler::telemetry() const {
    return telemetry_;
}

} // namespace runtime