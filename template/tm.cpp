/**
 * @file   tm.cpp
 * @author [...]
 *
 * @section LICENSE
 *
 * [...]
 *
 * @section DESCRIPTION
 *
 * Implementation of your own transaction manager.
 * You can completely rewrite this file (and create more files) as you wish.
 * Only the interface (i.e. exported symbols and semantic) must be preserved.
 **/

// Requested features
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#ifdef __STDC_NO_ATOMICS__
#error Current C11 compiler does not support atomic operations
#endif

// External headers

// Internal headers
#include <tm.hpp>

#include "macros.h"
#include <atomic>
#include <assert.h>
#include <cstdlib>
#include <vector>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>

struct Region
{

    size_t align;
    size_t size;

    std::atomic<uint64_t> clock{0};

    std::list<Segment> segments;
    std::shared_mutex segments_lock;

    std::mutex transaction_start_lock;
    std::multiset<uint64_t> transaction_starts;
    std::map<uint64_t, uint64_t> memory_to_free;

    Region(size_t size, size_t align)
    {
        this->align = align;
        this->size = size;
        this->segments.push_back(Segment(size, align));
    }

    uint64_t increment_clock()
    {
        return clock.fetch_add(1) + 1;
    }

    uint64_t get_clock()
    {
        return clock.load();
    }
};

struct Segment
{
    std::vector<Word> words;
    void *mem;
    size_t size;
    size_t align;

    Segment(size_t size, size_t align)
    {
        assert(size % align == 0);

        words = std::vector<Word>(size / align);
        this->size = size;
        this->align = align;

        mem = malloc(size);
        if (mem == NULL)
            throw std::bad_alloc();
        memset(mem, 0, size);
    }
    ~Segment()
    {
        free(mem);
    }

    bool is_in(const void *address, int size)
    {
        uint64_t segment_start = reinterpret_cast<uint64_t>(mem);
        uint64_t address_start = reinterpret_cast<uint64_t>(address);

        return (address_start >= segment_start) && (address_start < segment_start + this->size) && (address_start + size <= segment_start + this->size);
    }

    void *read_unsafe(const void *address)
    {
        uint64_t segment_start = reinterpret_cast<uint64_t>(mem);
        uint64_t address_start = reinterpret_cast<uint64_t>(address);

        return (void *)(reinterpret_cast<uint64_t>(mem) + (address_start - segment_start));
    }

    Word *get_word(uint64_t address)
    {
        size_t offset = (address - (uint64_t)mem) / align;

        assert(offset >= 0);
        assert(offset * align < size);

        return &words[offset];
    }
};

struct Word
{
    std::atomic<uint64_t> write_status{0};

    Word()
    {
    }

    uint64_t write_version()
    {
        return write_status.load() >> 1;
    }

    uint64_t is_locked()
    {
        return write_status.load() & 1;
    }
};

struct Transaction
{
    Region *region;
    uint64_t rv;
    std::list<Segment> segments;
    std::unordered_map<uint64_t, std::pair<void *, Segment *>> dirty_memory;
    std::unordered_set<uint64_t> read_memory;
    std::unordered_set<uint64_t> freed_memory;

    Transaction(Region *region)
    {
        this->region = region;

        region->transaction_start_lock.lock();
        this->rv = region->get_clock();
        region->transaction_starts.insert(this->rv);
        region->transaction_start_lock.unlock();
    }
};

/** Create (i.e. allocate + init) a new shared memory region, with one first non-free-able allocated segment of the requested size and alignment.
 * @param size  Size of the first shared segment of memory to allocate (in bytes), must be a positive multiple of the alignment
 * @param align Alignment (in bytes, must be a power of 2) that the shared memory region must support
 * @return Opaque shared memory region handle, 'invalid_shared' on failure
 **/
shared_t tm_create(size_t size, size_t align)
{
    try
    {
        return new Region(size, align);
    }
    catch (...)
    {
        return invalid_shared;
    }
}

/** Destroy (i.e. clean-up + free) a given shared memory region.
 * @param shared Shared memory region to destroy, with no running transaction
 **/
void tm_destroy(shared_t unused(shared))
{
    // TODO: tm_destroy(shared_t)
}

/** [thread-safe] Return the start address of the first allocated segment in the shared memory region.
 * @param shared Shared memory region to query
 * @return Start address of the first allocated segment
 **/
void *tm_start(shared_t shared)
{
    return static_cast<Region *>(shared)->segments.front().mem;
}

/** [thread-safe] Return the size (in bytes) of the first allocated segment of the shared memory region.
 * @param shared Shared memory region to query
 * @return First allocated segment size
 **/
size_t tm_size(shared_t shared)
{
    return static_cast<Region *>(shared)->segments.front().size;
}

/** [thread-safe] Return the alignment (in bytes) of the memory accesses on the given shared memory region.
 * @param shared Shared memory region to query
 * @return Alignment used globally
 **/
size_t tm_align(shared_t shared)
{
    return static_cast<Region *>(shared)->align;
}

/** [thread-safe] Begin a new transaction on the given shared memory region.
 * @param shared Shared memory region to start a transaction on
 * @param is_ro  Whether the transaction is read-only
 * @return Opaque transaction ID, 'invalid_tx' on failure
 **/
tx_t tm_begin(shared_t shared, bool unused(is_ro))
{
    try
    {
        return reinterpret_cast<tx_t>(new Transaction(static_cast<Region *>(shared)));
    }
    catch (...)
    {
        return invalid_tx;
    }
}

/** [thread-safe] End the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to end
 * @return Whether the whole transaction committed
 **/
bool tm_end(shared_t shared, tx_t tx)
{
    Region *region = static_cast<Region *>(shared);
    Transaction *transaction = reinterpret_cast<Transaction *>(tx);

    for (auto dirty_word : transaction->dirty_memory)
    {
    }

    return true;
}

/** [thread-safe] Read operation in the given transaction, source in the shared region and target in a private region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in the shared region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in a private region)
 * @return Whether the whole transaction can continue
 **/
bool tm_read(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = static_cast<Region *>(shared);
    Transaction *transaction = reinterpret_cast<Transaction *>(tx);

    for (auto &transaction_segment : transaction->segments)
    {
        if (transaction_segment.is_in(source, size))
        {
            memcpy(target, source, size);
            return true;
        }
    }

    Segment *source_segment = nullptr;
    std::shared_lock<std::shared_mutex> segments_lock(region->segments_lock);
    for (auto &segment : region->segments)
    {
        if (segment.is_in(source, size))
        {
            source_segment = &segment;
            break;
        }
    }
    segments_lock.unlock();
    if (source_segment == nullptr)
        return false;

    uint64_t source_position = reinterpret_cast<uint64_t>(source);
    size_t align = region->align;
    for (size_t offset = 0; offset < size; offset += align)
    {
        if (transaction->dirty_memory.find(source_position + offset) != transaction->dirty_memory.end())
            memcpy((void *)((uint64_t)(target) + offset), transaction->dirty_memory[source_position + offset].first, align);
        else
        {
            Word *current_word = source_segment->get_word(source_position + offset);

            uint64_t lock_value = current_word->write_status.load();
            if (lock_value & 1 || (lock_value >> 1) > transaction->rv)
            {
                return false;
            }
            memcpy((void *)((uint64_t)(target) + offset), (void *)((uint64_t)(source) + offset), align);
            lock_value = current_word->write_status.load();
            if (lock_value & 1 || (lock_value >> 1) > transaction->rv)
            {
                return false;
            }
            transaction->read_memory.insert(source_position + offset);
        }
    }
    return true;
}

/** [thread-safe] Write operation in the given transaction, source in a private region and target in the shared region.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param source Source start address (in a private region)
 * @param size   Length to copy (in bytes), must be a positive multiple of the alignment
 * @param target Target start address (in the shared region)
 * @return Whether the whole transaction can continue
 **/
bool tm_write(shared_t shared, tx_t tx, void const *source, size_t size, void *target)
{
    Region *region = static_cast<Region *>(shared);
    Transaction *transaction = reinterpret_cast<Transaction *>(tx);

    for (auto &transaction_segment : transaction->segments)
    {
        if (transaction_segment.is_in(target, size))
        {
            memcpy(target, source, size);
            return true;
        }
    }

    Segment *source_segment = nullptr;
    std::shared_lock<std::shared_mutex> segments_lock(region->segments_lock);
    for (auto &segment : region->segments)
    {
        if (segment.is_in(source, size))
        {
            source_segment = &segment;
            break;
        }
    }
    segments_lock.unlock();
    if (source_segment == nullptr)
        return false;

    uint64_t source_position = reinterpret_cast<uint64_t>(source);
    uint64_t target_position = reinterpret_cast<uint64_t>(target);
    size_t align = region->align;
    for (size_t offset = 0; offset < size; offset += align)
    {
        if (transaction->dirty_memory.find(target_position + offset) != transaction->dirty_memory.end())
        {
            memcpy(transaction->dirty_memory[target_position + offset].first, (void *)(source_position + offset), region->align);
            continue;
        }

        void *mem = malloc(region->align);
        if (mem == nullptr)
            return false;
        memcpy(mem, (void *)(source_position + offset), region->align);
        transaction->dirty_memory[target_position + offset] = {mem, source_segment};
    }

    return true;
}

/** [thread-safe] Memory allocation in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param size   Allocation requested size (in bytes), must be a positive multiple of the alignment
 * @param target Pointer in private memory receiving the address of the first byte of the newly allocated, aligned segment
 * @return Whether the whole transaction can continue (success/nomem), or not (abort_alloc)
 **/
Alloc tm_alloc(shared_t shared, tx_t tx, size_t size, void **(target))
{
    Region *region = static_cast<Region *>(shared);
    Transaction *transaction = reinterpret_cast<Transaction *>(tx);

    try
    {
        std::unique_lock<std::shared_mutex> segments_lock(region->segments_lock);
        Segment &new_segment = transaction->segments.emplace_back(size, region->align);
        *target = new_segment.mem;
    }
    catch (const std::bad_alloc &)
    {
        return Alloc::nomem;
    }

    return Alloc::success;
}

/** [thread-safe] Memory freeing in the given transaction.
 * @param shared Shared memory region associated with the transaction
 * @param tx     Transaction to use
 * @param target Address of the first byte of the previously allocated segment to deallocate
 * @return Whether the whole transaction can continue
 **/
bool tm_free(shared_t shared, tx_t tx, void *target)
{
    Region *region = static_cast<Region *>(shared);
    Transaction *transaction = reinterpret_cast<Transaction *>(tx);

    for (auto it = transaction->segments.begin(); it != transaction->segments.end(); ++it)
    {
        if (it->mem == target)
        {
            transaction->segments.erase(it);
            return true;
        }
    }

    transaction->freed_memory.insert((uint64_t)target);
    return true;
}
