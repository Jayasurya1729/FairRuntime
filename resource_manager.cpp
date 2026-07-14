#include "resource_manager.h"
#include <algorithm>
#include <sstream>

namespace runtime {

ResourceManager::ResourceManager(const PolicyConfig& config)
    : config_(config) {
}

double ResourceManager::compute_adjusted_weight(int task_id, double base_weight,
                                                size_t task_live_bytes,
                                                size_t task_allocations,
                                                double runtime_share,
                                                uint64_t last_slice_ns,
                                                std::vector<std::string>* reasons) const {
    double adjusted = base_weight;

    if (task_live_bytes > config_.task_memory_limit) {
        adjusted *= 0.65;
        if (reasons) reasons->push_back("Live memory exceeded per-task limit");
    }

    if (task_allocations > config_.allocation_rate_limit) {
        adjusted *= 0.80;
        if (reasons) reasons->push_back("Allocation rate exceeded threshold");
    }

    if (runtime_share > 1.0 + config_.fairness_threshold) {
        adjusted *= 0.75;
        if (reasons) reasons->push_back("Runtime share exceeded fairness target");
    }

    // A cooperative scheduler can't preempt mid-slice the way a timer-driven
    // CFS can, so latency_target_ns can't bound a slice as it happens. Instead,
    // once a slice finishes, we check retroactively: if the task ran longer than
    // the target before yielding, its weight is penalized for subsequent rounds.
    // This makes latency_target_ns an enforced fairness signal instead of a
    // display-only config value.
    if (last_slice_ns > config_.latency_target_ns) {
        adjusted *= 0.85;
        if (reasons) reasons->push_back("Slice duration exceeded latency target");
    }

    if (adjusted < 1.0) {
        adjusted = 1.0;
    }

    return adjusted;
}

void ResourceManager::observe(const std::vector<int>& active_task_ids) {
    std::ostringstream entry;
    entry << "Observed active tasks: ";
    for (size_t i = 0; i < active_task_ids.size(); ++i) {
        if (i) entry << ", ";
        entry << active_task_ids[i];
    }
    recent_actions_.push_back(entry.str());
    if (recent_actions_.size() > 16) {
        recent_actions_.erase(recent_actions_.begin());
    }
}

std::vector<std::string> ResourceManager::recent_actions() const {
    return recent_actions_;
}

const PolicyConfig& ResourceManager::config() const {
    return config_;
}

} // namespace runtime