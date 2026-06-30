#include "allocator.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <sys/mman.h>

namespace runtime {

RuntimeAllocator& RuntimeAllocator::instance() {
    static RuntimeAllocator allocator;
    return allocator;
}

RuntimeAllocator::RuntimeAllocator() = default;

size_t RuntimeAllocator::align_up(size_t size) const {
    return (size + ALIGN - 1) & ~(ALIGN - 1);
}

RuntimeAllocator::Arena& RuntimeAllocator::ensure_arena(int task_id) {
    auto it = arenas_.find(task_id);
    if (it != arenas_.end()) {
        return it->second;
    }

    Arena arena;
    arena.task_id = task_id;
    arenas_.emplace(task_id, std::move(arena));
    return arenas_.at(task_id);
}

// Returns the header of the block physically adjacent (immediately after)
// `block` within the same chunk, or nullptr if `block` is the last block
// in its chunk. Used to maintain phys_prev links and to find a forward
// coalescing candidate.
RuntimeAllocator::Header* RuntimeAllocator::find_physical_next(const Arena& arena, Header* block) const {
    char* block_addr = reinterpret_cast<char*>(block);
    char* chunk_end = nullptr;
    for (void* chunk : arena.chunks) {
        char* chunk_start = static_cast<char*>(chunk);
        if (block_addr >= chunk_start && block_addr < chunk_start + CHUNK_SIZE) {
            chunk_end = chunk_start + CHUNK_SIZE;
            break;
        }
    }

    if (!chunk_end) {
        return nullptr;
    }

    char* payload = block_addr + sizeof(Header);
    char* next_addr = payload + block->size;
    if (next_addr >= chunk_end) {
        return nullptr;
    }

    return reinterpret_cast<Header*>(next_addr);
}

RuntimeAllocator::Header* RuntimeAllocator::mmap_chunk(Arena& arena) {
    void* raw = mmap(nullptr, CHUNK_SIZE, PROT_READ | PROT_WRITE,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (raw == MAP_FAILED) {
        return nullptr;
    }

    total_mmap_calls_++;
    total_bytes_from_os_ += CHUNK_SIZE;
    arena.chunk_count++;
    arena.chunks.push_back(raw);

    Header* block = static_cast<Header*>(raw);
    block->size = CHUNK_SIZE - sizeof(Header);
    block->free = true;
    block->large = false;
    block->owner_task = arena.task_id;
    block->next = nullptr;
    block->prev = nullptr;
    block->phys_prev = nullptr; // first (and currently only) block in this chunk

    insert_into_free_list(arena, block);
    update_arena_free_stats(arena);
    return block;
}

void RuntimeAllocator::split_block(Header* block, size_t request_size) {
    size_t aligned = align_up(request_size);
    if (block->size < aligned + MIN_SPLIT_SIZE) {
        return;
    }

    Arena& arena = arenas_.at(block->owner_task);

    // Capture whatever physically follows `block` BEFORE we shrink it,
    // since shrinking changes the address find_physical_next would compute.
    Header* old_next = find_physical_next(arena, block);

    Header* remainder = reinterpret_cast<Header*>(reinterpret_cast<char*>(block + 1) + aligned);
    remainder->size = block->size - aligned - sizeof(Header);
    remainder->free = true;
    remainder->large = false;
    remainder->owner_task = block->owner_task;
    remainder->next = nullptr;
    remainder->prev = nullptr;
    remainder->phys_prev = block;

    block->size = aligned;

    // Whatever used to follow `block` now follows `remainder` instead.
    if (old_next) {
        old_next->phys_prev = remainder;
    }

    insert_into_free_list(arena, remainder);
}

void RuntimeAllocator::remove_from_free_list(Arena& arena, Header* block) {
    if (block->prev) {
        block->prev->next = block->next;
    } else {
        arena.free_list = block->next;
    }
    if (block->next) {
        block->next->prev = block->prev;
    }
    block->next = nullptr;
    block->prev = nullptr;
}

void RuntimeAllocator::insert_into_free_list(Arena& arena, Header* block) {
    block->free = true;
    block->next = arena.free_list;
    block->prev = nullptr;
    if (arena.free_list) {
        arena.free_list->prev = block;
    }
    arena.free_list = block;
}

// Bidirectional coalescing: merges `block` with its physical successor if
// free, AND with its physical predecessor if free. This keeps free space
// consolidated into the largest possible contiguous spans instead of only
// ever merging forward, which previously left many freeable runs split.
void RuntimeAllocator::coalesce_free_segments(Arena& arena, Header* block) {
    // --- Forward coalesce: merge `block` with the block right after it ---
    Header* next = find_physical_next(arena, block);
    if (next && next->free && !next->large && next->owner_task == block->owner_task) {
        remove_from_free_list(arena, next);
        remove_from_free_list(arena, block);
        block->size += sizeof(Header) + next->size;

        // The block that used to follow `next` now follows `block` (same
        // address as before, just larger), so fix its phys_prev.
        Header* after = find_physical_next(arena, block);
        if (after) {
            after->phys_prev = block;
        }

        insert_into_free_list(arena, block);
    }

    // --- Backward coalesce: merge `block` into its physical predecessor ---
    Header* prev = block->phys_prev;
    if (prev && prev->free && !prev->large && prev->owner_task == block->owner_task) {
        remove_from_free_list(arena, prev);
        remove_from_free_list(arena, block);
        prev->size += sizeof(Header) + block->size;

        // Whatever now follows the merged block (prev) needs its phys_prev
        // updated to point at prev instead of the consumed `block`.
        Header* after = find_physical_next(arena, prev);
        if (after) {
            after->phys_prev = prev;
        }

        insert_into_free_list(arena, prev);
    }
}

void RuntimeAllocator::update_arena_free_stats(Arena& arena) {
    arena.free_bytes = 0;
    arena.largest_free_block = 0;
    for (Header* current = arena.free_list; current; current = current->next) {
        arena.free_bytes += current->size;
        if (current->size > arena.largest_free_block) {
            arena.largest_free_block = current->size;
        }
    }
}

// =========================================================================
// UNLOCKED INTERNAL CORE IMPLEMENTATIONS (Prevents Reentrancy Deadlocks)
// =========================================================================

void* RuntimeAllocator::allocate_unlocked(size_t size, int task_id) {
    if (size == 0) {
        return nullptr;
    }

    size = align_up(size);

    if (size > LARGE_THRESHOLD) {
        size_t total = sizeof(Header) + size;
        void* raw = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (raw == MAP_FAILED) {
            return nullptr;
        }

        total_mmap_calls_++;
        total_bytes_from_os_ += total;
        total_large_allocs_++;

        Header* header = static_cast<Header*>(raw);
        header->size = size;
        header->free = false;
        header->large = true;
        header->owner_task = task_id;
        header->next = nullptr;
        header->prev = nullptr;
        header->phys_prev = nullptr; // large blocks live outside chunk bookkeeping
        return static_cast<void*>(header + 1);
    }

    Arena& arena = ensure_arena(task_id);
    arena.allocation_count++;

    Header* candidate = arena.free_list;
    while (candidate) {
        if (candidate->size >= size) {
            remove_from_free_list(arena, candidate);
            split_block(candidate, size);
            candidate->free = false;
            size_t allocated_size = candidate->size;
            arena.total_allocated += allocated_size;
            arena.live_bytes += allocated_size;
            if (arena.live_bytes > arena.peak_allocated) {
                arena.peak_allocated = arena.live_bytes;
            }
            update_arena_free_stats(arena);
            return static_cast<void*>(candidate + 1);
        }
        candidate = candidate->next;
    }

    Header* chunk = mmap_chunk(arena);
    if (!chunk) {
        return nullptr;
    }

    remove_from_free_list(arena, chunk);
    split_block(chunk, size);
    chunk->free = false;
    size_t allocated_size = chunk->size;
    arena.total_allocated += allocated_size;
    arena.live_bytes += allocated_size;
    if (arena.live_bytes > arena.peak_allocated) {
        arena.peak_allocated = arena.live_bytes;
    }
    update_arena_free_stats(arena);
    return static_cast<void*>(chunk + 1);
}

void RuntimeAllocator::deallocate_unlocked(void* ptr) {
    if (!ptr) {
        return;
    }

    Header* header = static_cast<Header*>(ptr) - 1;
    if (header->large) {
        size_t total = sizeof(Header) + header->size;
        munmap(header, total);
        total_munmap_calls_++;
        return;
    }

    Arena& arena = ensure_arena(header->owner_task);
    assert(!header->free && "Double free detected");
    if (arena.live_bytes < header->size) {
        std::cerr << "Allocator underflow detected: task=" << arena.task_id
                  << " live_bytes=" << arena.live_bytes
                  << " block_size=" << header->size
                  << " ptr=" << ptr << "\n";
    }

    header->free = true;
    arena.free_count++;
    arena.live_bytes -= header->size;
    insert_into_free_list(arena, header);
    coalesce_free_segments(arena, header);
    update_arena_free_stats(arena);
}

TaskMemoryStats RuntimeAllocator::get_task_stats_unlocked(int task_id) const {
    TaskMemoryStats stats;
    auto it = arenas_.find(task_id);
    if (it != arenas_.end()) {
        const Arena& arena = it->second;
        stats.task_id = arena.task_id;
        stats.total_allocated = arena.total_allocated;
        stats.peak_allocated = arena.peak_allocated;
        stats.live_bytes = arena.live_bytes;
        stats.active_allocations = arena.allocation_count - arena.free_count;
        stats.allocation_count = arena.allocation_count;
        stats.free_count = arena.free_count;
        stats.chunk_count = arena.chunk_count;
        stats.free_bytes = arena.free_bytes;
        stats.largest_free_block = arena.largest_free_block;
    }
    return stats;
}

// =========================================================================
// PUBLIC API INTERFACES (Handles Concurrency Locking Safely)
// =========================================================================

void* RuntimeAllocator::allocate(size_t size, int task_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    return allocate_unlocked(size, task_id);
}

void RuntimeAllocator::deallocate(void* ptr) {
    std::lock_guard<std::mutex> guard(mutex_);
    deallocate_unlocked(ptr);
}

void* RuntimeAllocator::reallocate(void* ptr, size_t size, int task_id) {
    if (!ptr) {
        std::lock_guard<std::mutex> guard(mutex_);
        return allocate_unlocked(size, task_id);
    }
    if (size == 0) {
        std::lock_guard<std::mutex> guard(mutex_);
        deallocate_unlocked(ptr);
        return nullptr;
    }

    std::lock_guard<std::mutex> guard(mutex_);

    Header* header = static_cast<Header*>(ptr) - 1;
    bool is_large_block = header->large;

    if (!is_large_block) {
        if (header->size >= size) {
            if (header->size >= size + MIN_SPLIT_SIZE) {
                split_block(header, size);
            }
            return ptr;
        }
    }

    void* new_ptr = allocate_unlocked(size, task_id);
    if (!new_ptr) {
        return nullptr;
    }

    size_t copy_size = header->size;
    if (is_large_block) {
        copy_size = header->size;
    }
    copy_size = copy_size < size ? copy_size : size;
    std::memcpy(new_ptr, ptr, copy_size);
    deallocate_unlocked(ptr);
    return new_ptr;
}

TaskMemoryStats RuntimeAllocator::get_task_stats(int task_id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return get_task_stats_unlocked(task_id);
}

FragmentationStats RuntimeAllocator::get_fragmentation(int task_id) const {
    std::lock_guard<std::mutex> guard(mutex_);
    FragmentationStats stats;
    auto it = arenas_.find(task_id);
    if (it != arenas_.end()) {
        const Arena& arena = it->second;
        stats.free_bytes = arena.free_bytes;
        stats.largest_free_block = arena.largest_free_block;
        stats.usable_bytes = arena.total_allocated + arena.free_bytes;
        if (stats.usable_bytes > 0) {
            stats.fragmentation_ratio = 1.0 - static_cast<double>(stats.largest_free_block) / stats.free_bytes;
            if (stats.fragmentation_ratio < 0.0) {
                stats.fragmentation_ratio = 0.0;
            }
        }
    }
    return stats;
}

std::vector<TaskMemoryStats> RuntimeAllocator::snapshot_task_stats() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<TaskMemoryStats> snapshots;
    snapshots.reserve(arenas_.size());
    for (const auto& entry : arenas_) {
        snapshots.push_back(get_task_stats_unlocked(entry.first));
    }
    return snapshots;
}

size_t RuntimeAllocator::total_bytes_from_os() const {
    return total_bytes_from_os_;
}

size_t RuntimeAllocator::total_mmap_calls() const {
    return total_mmap_calls_;
}

size_t RuntimeAllocator::total_munmap_calls() const {
    return total_munmap_calls_;
}

size_t RuntimeAllocator::total_large_allocs() const {
    return total_large_allocs_;
}

} // namespace runtime