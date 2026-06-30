#ifndef RESOURCE_AWARE_POLICY_H
#define RESOURCE_AWARE_POLICY_H

namespace runtime {

enum class SchedulingPolicy {
    CFS,
};

enum class TaskStatus {
    Ready,
    Running,
    Finished,
};

} // namespace runtime

#endif // RESOURCE_AWARE_POLICY_H
