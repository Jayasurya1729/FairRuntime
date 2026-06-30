#include "telemetry.h"
#include "resource_manager.h"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <cmath>

namespace runtime {

void Telemetry::register_task(int task_id, const std::string& name, int nice) {
    TaskTelemetry telemetry;
    telemetry.task_id = task_id;
    telemetry.name = name;
    telemetry.nice = nice;
    tasks_[task_id] = telemetry;
}

void Telemetry::record_task_run(int task_id, uint64_t delta_ns, uint64_t weight) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    it->second.total_run_ns += delta_ns;
    it->second.vruntime_ns += (delta_ns * 1024ULL) / std::max<uint64_t>(1, weight);
}

void Telemetry::record_task_yield(int task_id) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    it->second.yield_count += 1;
}

void Telemetry::record_allocation(int task_id, size_t bytes) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    it->second.allocation_bytes += bytes;
    it->second.allocation_ops += 1;
    if (it->second.allocation_bytes > it->second.peak_memory_bytes) {
        it->second.peak_memory_bytes = it->second.allocation_bytes;
    }
}

void Telemetry::record_free(int task_id, size_t bytes) {
    auto it = tasks_.find(task_id);
    if (it == tasks_.end()) return;
    it->second.free_ops += 1;
}

void Telemetry::record_task_finish(int task_id) {
    // Intentionally left blank for now, but available for future hooks.
}

void Telemetry::record_policy_action(const std::string& action) {
    policy_actions_.push_back(action);
    if (policy_actions_.size() > 32) {
        policy_actions_.erase(policy_actions_.begin());
    }
}

// Upgraded to calculate research-grade Jain's Fairness Index
double Telemetry::compute_fairness() const {
    if (tasks_.empty()) {
        return 1.0;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    for (const auto& entry : tasks_) {
        double run_time = static_cast<double>(entry.second.total_run_ns);
        sum += run_time;
        sum_sq += (run_time * run_time);
    }
    
    if (sum_sq == 0.0) {
        return 1.0;
    }
    return (sum * sum) / (tasks_.size() * sum_sq);
}

std::vector<TaskTelemetry> Telemetry::snapshot_task_telemetry() const {
    std::vector<TaskTelemetry> snapshot;
    snapshot.reserve(tasks_.size());
    for (const auto& entry : tasks_) {
        snapshot.push_back(entry.second);
    }
    return snapshot;
}

std::vector<std::string> Telemetry::recent_policy_actions() const {
    return policy_actions_;
}

void Telemetry::print_dashboard(SchedulingPolicy policy,
                               const std::vector<TaskTelemetry>& task_snapshots,
                               const std::vector<TaskMemoryStats>& arena_snapshots,
                               const ResourceManager& resource_manager) const {
    std::cout << "\n========================================================\n";
    std::cout << "             RESOURCE-AWARE RUNTIME DASHBOARD           \n";
    std::cout << "========================================================\n\n";

    // -----------------------------------------------------------------
    // ## SCHEDULER STATISTICS
    // -----------------------------------------------------------------
    std::cout << "## SCHEDULER STATISTICS\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Scheduling Policy    : CFS\n";
    std::cout << "  Scheduler Mode       : Virtual Runtime Scheduling\n";
    std::cout << "  Latency Target       : " << resource_manager.config().latency_target_ns << " ns\n";
    std::cout << "  Jain's Fairness Index: " << std::fixed << std::setprecision(6) << compute_fairness() << "\n";
    std::cout << "--------------------------------------------------------\n";

    std::cout << std::left << std::setw(6) << "ID" << std::setw(18) << "Task Name"
              << std::setw(14) << "Run (ms)" << std::setw(12) << "VRun (ms)"
              << std::setw(10) << "Yields" << std::setw(10) << "Nice"
              << std::endl;
    std::cout << "--------------------------------------------------------\n";
    for (const auto& t : task_snapshots) {
        std::cout << std::left << std::setw(6) << t.task_id
                  << std::setw(18) << t.name
                  << std::setw(14) << (t.total_run_ns / 1000000.0)
                  << std::setw(12) << (t.vruntime_ns / 1000000.0)
                  << std::setw(10) << t.yield_count
                  << std::setw(10) << t.nice
                  << std::endl;
    }
    std::cout << "--------------------------------------------------------\n\n";

    // -----------------------------------------------------------------
    // ## MEMORY SUBSYSTEM
    // -----------------------------------------------------------------
    std::cout << "## MEMORY SUBSYSTEM\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Global Memory Limit  : " << resource_manager.config().global_memory_limit << " bytes\n";
    std::cout << "  Task Memory Limit    : " << resource_manager.config().task_memory_limit << " bytes\n";
    std::cout << "--------------------------------------------------------\n";

    // Re-mapped names to clear all confusion and added inline fragmentation computation
    std::cout << std::left << std::setw(6) << "Task" 
              << std::setw(18) << "Total Allocated"
              << std::setw(18) << "Peak Live Memory" 
              << std::setw(20) << "Current Live Memory" 
              << std::setw(14) << "Fragmentation" << std::endl;
    std::cout << "--------------------------------------------------------\n";
    for (const auto& s : arena_snapshots) {
        double frag_ratio = 0.0;
        if (s.free_bytes > 0) {
            frag_ratio = 1.0 - (static_cast<double>(s.largest_free_block) / s.free_bytes);
            if (frag_ratio < 0.0) frag_ratio = 0.0;
        }

        std::cout << std::left << std::setw(6) << s.task_id
                  << std::setw(18) << s.total_allocated
                  << std::setw(18) << s.peak_allocated
                  << std::setw(20) << s.live_bytes
                  << std::fixed << std::setprecision(2) << (frag_ratio * 100.0) << "%\n";
    }
    std::cout << "--------------------------------------------------------\n\n";

    // -----------------------------------------------------------------
    // ## RESOURCE MANAGER ACTIONS
    // -----------------------------------------------------------------
    std::cout << "## RESOURCE MANAGER ACTIONS\n";
    std::cout << "--------------------------------------------------------\n";
    if (policy_actions_.empty()) {
        std::cout << "  [System Status: Nominal. No throttling limits triggered.]\n";
    } else {
        for (const auto& action : policy_actions_) {
            std::cout << action << "\n";
        }
    }
    std::cout << "========================================================\n";
}

} // namespace runtime