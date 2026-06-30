#include "allocator.h"
#include "scheduler.h"
#include "telemetry.h"
#include "resource_manager.h"

#include <chrono>
#include <iostream>
#include <random>
#include <vector>

using namespace runtime;

static void allocation_workload(int iterations, int allocation_range, int id) {
    Scheduler* scheduler = Scheduler::instance();
    RuntimeAllocator& allocator = RuntimeAllocator::instance();
    std::mt19937 rng(1234 + id);
    std::uniform_int_distribution<size_t> size_dist(32, allocation_range);
    std::vector<void*> live_blocks;
    live_blocks.reserve(64);

    for (int i = 0; i < iterations; ++i) {
        size_t size = size_dist(rng);
        void* ptr = allocator.allocate(size, scheduler->current_task_id());
        if (ptr) {
            scheduler->record_allocation(size);
            live_blocks.push_back(ptr);
        }

        if ((i % 4) == 0 && !live_blocks.empty()) {
            size_t idx = rng() % live_blocks.size();
            void* old_ptr = live_blocks[idx];
            live_blocks[idx] = live_blocks.back();
            live_blocks.pop_back();
            RuntimeAllocator::instance().deallocate(old_ptr);
            scheduler->record_free(size);
        }

        if ((i % 7) == 0) {
            scheduler->yield();
        }
    }

    for (void* ptr : live_blocks) {
        RuntimeAllocator::instance().deallocate(ptr);
    }
}

int main() {
    RuntimeAllocator::instance();

    PolicyConfig config;
    config.global_memory_limit = 192 * 1024 * 1024;
    config.task_memory_limit = 48 * 1024 * 1024;
    config.latency_target_ns = 8ULL * 1000ULL * 1000ULL;
    config.fairness_threshold = 0.20;
    config.allocation_rate_limit = 2 * 1024 * 1024;

    ResourceManager resource_manager(config);
    Scheduler scheduler(SchedulingPolicy::CFS, &resource_manager);

    scheduler.create_task("batch-1", []() {
        allocation_workload(240, 8192, 1);
    }, 5);

    scheduler.create_task("batch-2", []() {
        allocation_workload(240, 8192, 2);
    }, 10);

    scheduler.create_task("interactive", []() {
        allocation_workload(180, 2048, 3);
    }, -5);

    scheduler.create_task("background", []() {
        allocation_workload(260, 16384, 4);
    }, 15);

    auto start = std::chrono::high_resolution_clock::now();
    scheduler.run();
    auto end = std::chrono::high_resolution_clock::now();

    std::vector<TaskTelemetry> telemetry_snapshot = scheduler.telemetry().snapshot_task_telemetry();
    std::vector<TaskMemoryStats> memory_snapshot = RuntimeAllocator::instance().snapshot_task_stats();

    Telemetry& telemetry = scheduler.telemetry();
    telemetry.print_dashboard(scheduler.policy(), telemetry_snapshot, memory_snapshot, resource_manager);

    auto runtime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "\nTotal scheduler runtime: " << runtime_ms << " ms" << std::endl;
    std::cout << "Memory bytes from OS: " << RuntimeAllocator::instance().total_bytes_from_os() << std::endl;
    std::cout << "mmap calls: " << RuntimeAllocator::instance().total_mmap_calls() << ", munmap: " << RuntimeAllocator::instance().total_munmap_calls() << std::endl;

    return 0;
}
