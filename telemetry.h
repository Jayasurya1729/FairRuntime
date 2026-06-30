#ifndef RESOURCE_AWARE_TELEMETRY_H
#define RESOURCE_AWARE_TELEMETRY_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "allocator.h"
#include "policy.h"

namespace runtime {
class ResourceManager;

struct TaskTelemetry {
    int      task_id = -1;
    std::string name;
    uint64_t total_run_ns = 0;
    uint64_t vruntime_ns = 0;
    size_t   allocation_bytes = 0;
    size_t   allocation_ops = 0;
    size_t   free_ops = 0;
    size_t   peak_memory_bytes = 0;
    size_t   yield_count = 0;
    int      nice = 0;
};

class Telemetry {
public:
    void register_task(int task_id, const std::string& name, int nice);
    void record_task_run(int task_id, uint64_t delta_ns, uint64_t weight);
    void record_task_yield(int task_id);
    void record_allocation(int task_id, size_t bytes);
    void record_free(int task_id, size_t bytes);
    void record_task_finish(int task_id);
    void record_policy_action(const std::string& action);

    double compute_fairness() const;
    std::vector<TaskTelemetry> snapshot_task_telemetry() const;
    std::vector<std::string> recent_policy_actions() const;

    void print_dashboard(SchedulingPolicy policy, const std::vector<TaskTelemetry>& task_snapshots,
                         const std::vector<TaskMemoryStats>& arena_snapshots,
                         const ResourceManager& resource_manager) const;

private:
    std::unordered_map<int, TaskTelemetry> tasks_;
    std::vector<std::string> policy_actions_;
};

} // namespace runtime

#endif // RESOURCE_AWARE_TELEMETRY_H
