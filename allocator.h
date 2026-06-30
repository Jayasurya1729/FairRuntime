#ifndef RESOURCE_AWARE_ALLOCATOR_H
#define RESOURCE_AWARE_ALLOCATOR_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace runtime {

struct TaskMemoryStats {
    int      task_id = -1;
    size_t   total_allocated = 0;
    size_t   peak_allocated = 0;
    size_t   active_allocations = 0;
    size_t   allocation_count = 0;
    size_t   free_count = 0;
    size_t   live_bytes = 0;
    size_t   chunk_count = 0;
    size_t   free_bytes = 0;
    size_t   largest_free_block = 0;
};

struct FragmentationStats {
    size_t usable_bytes = 0;
    size_t free_bytes = 0;
    size_t largest_free_block = 0;
    double fragmentation_ratio = 0.0;
};

class RuntimeAllocator {
public:
    static RuntimeAllocator& instance();

    void* allocate(size_t size, int task_id);
    void  deallocate(void* ptr);
    void* reallocate(void* ptr, size_t size, int task_id);

    TaskMemoryStats get_task_stats(int task_id) const;
    FragmentationStats get_fragmentation(int task_id) const;
    std::vector<TaskMemoryStats> snapshot_task_stats() const;

    size_t total_bytes_from_os() const;
    size_t total_mmap_calls() const;
    size_t total_munmap_calls() const;
    size_t total_large_allocs() const;

private:
    void* allocate_unlocked(size_t size, int task_id);
    void  deallocate_unlocked(void* ptr);
    TaskMemoryStats get_task_stats_unlocked(int task_id) const;
    RuntimeAllocator();
    RuntimeAllocator(const RuntimeAllocator&) = delete;
    RuntimeAllocator& operator=(const RuntimeAllocator&) = delete;

    struct Header {
        size_t size;
        bool   free;
        bool   large;
        int    owner_task;
        Header* next;      // free-list intrusive links
        Header* prev;      // free-list intrusive links
        Header* phys_prev; // physically preceding block within the same chunk
                            // (nullptr if this is the first block in its chunk)
    };

    struct Arena {
        int task_id = -1;
        Header* free_list = nullptr;
        size_t total_allocated = 0;
        size_t peak_allocated = 0;
        size_t live_bytes = 0;
        size_t allocation_count = 0;
        size_t free_count = 0;
        size_t chunk_count = 0;
        size_t free_bytes = 0;
        size_t largest_free_block = 0;
        std::vector<void*> chunks;
    };

    static constexpr size_t CHUNK_SIZE = 1024 * 1024;
    static constexpr size_t LARGE_THRESHOLD = 1024 * 1024;
    static constexpr size_t ALIGN = sizeof(void*) * 2;
    static constexpr size_t MIN_SPLIT_SIZE = sizeof(Header) + ALIGN;

    size_t align_up(size_t size) const;
    Arena& ensure_arena(int task_id);
    Header* mmap_chunk(Arena& arena);
    void split_block(Header* block, size_t request_size);
    void remove_from_free_list(Arena& arena, Header* block);
    void insert_into_free_list(Arena& arena, Header* block);
    void coalesce_free_segments(Arena& arena, Header* block);
    void update_arena_free_stats(Arena& arena);
    Header* find_physical_next(const Arena& arena, Header* block) const;

    mutable std::mutex mutex_;
    std::map<int, Arena> arenas_;

    size_t total_mmap_calls_ = 0;
    size_t total_munmap_calls_ = 0;
    size_t total_bytes_from_os_ = 0;
    size_t total_large_allocs_ = 0;
};

} // namespace runtime

#endif // RESOURCE_AWARE_ALLOCATOR_H