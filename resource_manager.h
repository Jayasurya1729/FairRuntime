#ifndef RESOURCE_AWARE_RESOURCE_MANAGER_H
#define RESOURCE_AWARE_RESOURCE_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace runtime {

struct PolicyConfig {
    size_t global_memory_limit = 256 * 1024 * 1024;
    size_t task_memory_limit = 64 * 1024 * 1024;
    uint64_t latency_target_ns = 10ULL * 1000ULL * 1000ULL;
    double fairness_threshold = 0.25;
    size_t allocation_rate_limit = 4 * 1024 * 1024;
};

class ResourceManager {
public:
    explicit ResourceManager(const PolicyConfig& config);
    double compute_adjusted_weight(int task_id, double base_weight,
                                   size_t task_live_bytes, size_t task_allocations,
                                   double runtime_share,
                                   std::vector<std::string>* reasons = nullptr) const;
    void observe(const std::vector<int>& active_task_ids);
    std::vector<std::string> recent_actions() const;
    const PolicyConfig& config() const;

private:
    PolicyConfig config_;
    std::vector<std::string> recent_actions_;
};

} // namespace runtime

#endif // RESOURCE_AWARE_RESOURCE_MANAGER_H