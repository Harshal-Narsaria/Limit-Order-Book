#pragma once

#include <cstddef>
#include <vector>

namespace lob {

// Fixed-capacity pool of T objects.
//
// All storage is allocated once in the constructor. allocate() and release()
// then just pop/push a pointer on a free-list stack: no calls into the heap
// allocator, no fragmentation, and objects live in one contiguous block, which
// is friendlier to the cache than nodes scattered around the heap.
//
// The pool hands out raw slots; the caller is responsible for initialising an
// object after allocate() and must not use it after release().
template <typename T>
class ObjectPool {
public:
    explicit ObjectPool(std::size_t capacity) : storage_(capacity) {
        free_list_.reserve(capacity);
        // Push in reverse so the first allocations come from the front of storage.
        for (std::size_t i = capacity; i > 0; --i) {
            free_list_.push_back(&storage_[i - 1]);
        }
    }

    // The pool hands out pointers into storage_, so it must not be copied or moved.
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Returns nullptr when the pool is exhausted.
    T* allocate() {
        if (free_list_.empty()) {
            return nullptr;
        }
        T* object = free_list_.back();
        free_list_.pop_back();
        return object;
    }

    // Never grows past `capacity`, so this push_back never reallocates.
    void release(T* object) { free_list_.push_back(object); }

    std::size_t capacity() const { return storage_.size(); }
    std::size_t available() const { return free_list_.size(); }

private:
    std::vector<T> storage_;
    std::vector<T*> free_list_;
};

}  // namespace lob
