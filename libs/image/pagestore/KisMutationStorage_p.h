/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_MUTATION_STORAGE_P_H
#define KIS_MUTATION_STORAGE_P_H

#include <kritaimage_export.h>
#include <QtGlobal>
#include <QHashFunctions>
#include <QMutex>
#include <QSharedPointer>
#include <atomic>
#include <boost/intrusive_ptr.hpp>
#include <array>
#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

class KisBackingBudgetController;
class KisMutationStorageOwner;
enum class KisBackingBudgetClass : quint8;
KRITAIMAGE_EXPORT KisMutationStorageOwner *kisMutationStorageOwner(KisBackingBudgetController *);

// Accounting lifetime only. It neither keeps a PageStore/payload alive nor
// grants mutation authority. Detached allocations keep their original parent
// child registration charged until their actual last deallocation.
class KRITAIMAGE_EXPORT KisMutationStorageOwner
{
public:
    void *allocate(size_t bytes, size_t alignment);
    void deallocate(void *data, size_t bytes, size_t alignment) noexcept;
    // Existing arena payloads use a reservation before physical allocation.
    // Their original charge follows this same accounting lifetime, without
    // charging it twice or retaining the controller itself.
    void retainLiveCharge(quint64 bytes) noexcept;
    void releaseLiveCharge(quint64 bytes) noexcept;
    void releaseLiveCharge(quint64 bytes, KisBackingBudgetClass budgetClass) noexcept;
    void ref() noexcept { m_references.fetch_add(1, std::memory_order_relaxed); }
    void deref() noexcept;
private:
    explicit KisMutationStorageOwner(KisBackingBudgetController *controller)
        : m_controller(controller), m_bytes(sizeof(KisMutationStorageOwner)) {}
    void detach(const std::shared_ptr<KisBackingBudgetController> &parent, quint64 child) noexcept;
    void stopAllocations() noexcept;
    std::atomic<quint32> m_references{0};
    QMutex m_gate;
    KisBackingBudgetController *m_controller;
    std::shared_ptr<KisBackingBudgetController> m_parent;
    quint64 m_child = 0;
    quint64 m_bytes;
    bool m_accepting = true;
    friend class KisBackingBudgetController;
    friend KisMutationStorageOwner *kisMutationStorageOwner(KisBackingBudgetController *);
};
inline void intrusive_ptr_add_ref(KisMutationStorageOwner *owner) noexcept { owner->ref(); }
inline void intrusive_ptr_release(KisMutationStorageOwner *owner) noexcept { owner->deref(); }

// Ordinary storage: the enclosing reservation/session keeps the controller
// alive. Use retained() only for real weak-control/inert/callback tails.
// Charge allocator-requested capacity (including buckets/directories), not
// live element counts. Free the allocation before returning its live charge.
KRITAIMAGE_EXPORT void *kisAllocateMutationStorage(KisBackingBudgetController *, size_t, size_t);
KRITAIMAGE_EXPORT void kisFreeMutationStorage(KisBackingBudgetController *, void *, size_t, size_t) noexcept;
KRITAIMAGE_EXPORT void *kisAllocatePageProcessStorage(size_t, size_t);
KRITAIMAGE_EXPORT void kisFreePageProcessStorage(void *, size_t, size_t) noexcept;
// Native runtime mappings are prepared under the same limit, then return the
// unused preparation capacity after the OS reports the actual mapping size.
KRITAIMAGE_EXPORT void kisReservePageProcessStorage(size_t);
KRITAIMAGE_EXPORT void kisReleasePageProcessStorage(size_t) noexcept;
KRITAIMAGE_EXPORT std::shared_ptr<std::pmr::memory_resource> kisPageProcessMemoryResource();

// Compatibility facades still returned through ordinary unique_ptr/delete.
// The allocation remembers its actual request, including its size prefix.
class KRITAIMAGE_EXPORT KisPageProcessStorageObject
{
public:
    static void *operator new(size_t bytes);
    static void operator delete(void *data) noexcept;
};

template<class T>
class KisMutationStorageAllocator
{
public:
    using value_type = T;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    explicit KisMutationStorageAllocator(KisBackingBudgetController *value = nullptr) noexcept : budget(value) {}
    template<class U>
    KisMutationStorageAllocator(const KisMutationStorageAllocator<U> &other) noexcept
        : budget(other.budget), owner(other.owner) {}

    static KisMutationStorageAllocator retained(KisBackingBudgetController *budget)
    {
        KisMutationStorageAllocator result(budget);
        result.owner.reset(kisMutationStorageOwner(budget));
        return result;
    }

    T *allocate(size_t count)
    {
        if (count > size_t(std::numeric_limits<qint64>::max()) / sizeof(T))
            throw std::bad_alloc();
        return static_cast<T *>(owner ? owner->allocate(count * sizeof(T), alignof(T))
            : kisAllocateMutationStorage(budget, count * sizeof(T), alignof(T)));
    }
    void deallocate(T *data, size_t count) noexcept
    {
        if (owner) owner->deallocate(data, count * sizeof(T), alignof(T));
        else kisFreeMutationStorage(budget, data, count * sizeof(T), alignof(T));
    }
    template<class U> bool operator==(const KisMutationStorageAllocator<U> &other) const noexcept
    { return budget == other.budget && owner == other.owner; }
    template<class U> bool operator!=(const KisMutationStorageAllocator<U> &other) const noexcept
    { return !(*this == other); }

    KisBackingBudgetController *budget = nullptr;
private:
    boost::intrusive_ptr<KisMutationStorageOwner> owner;
    template<class> friend class KisMutationStorageAllocator;
};

struct KisPageKey;
using KisPageKeyStorage = std::vector<KisPageKey, KisMutationStorageAllocator<KisPageKey>>;

// The original admission/activity owner supplies synchronization. Every bucket
// is an actual, constructed record: insert/erase never allocate nodes. Growth
// allocates and hashes outside the owner gate; capture copies only trivial
// identities under the gate, and installation checks the exact table revision.
// A Growth is storage preparation, never a queryable second authority.
template<class Key, class Value>
class KisMutationAdmissionTable
{
    struct Entry {
        Key key{};
        Value value{};
        bool occupied = false;
    };
    static_assert(std::is_trivially_copyable_v<Entry>);
    static constexpr size_t InlineBuckets = 8;
    struct DeleteArray {
        KisBackingBudgetController *budget = nullptr;
        size_t count = 0;
        void operator()(Entry *data) const noexcept
        {
            if (!data) return;
            std::destroy_n(data, count);
            KisMutationStorageAllocator<Entry>(budget).deallocate(data, count);
        }
    };
    using Array = std::unique_ptr<Entry[], DeleteArray>;
    static Array allocateArray(KisBackingBudgetController *budget, size_t count)
    {
        static_assert(std::is_nothrow_default_constructible_v<Entry>);
        Entry *data = KisMutationStorageAllocator<Entry>(budget).allocate(count);
        std::uninitialized_value_construct_n(data, count);
        return Array(data, DeleteArray{budget, count});
    }
    static size_t hash(const Key &key, size_t seed) noexcept
    {
        using ::qHash;
        return qHash(key, seed);
    }
    static size_t bucket(const Entry *entries, size_t count, const Key &key, size_t seed) noexcept
    {
        const size_t mask = count - 1;
        size_t index = hash(key, seed) & mask;
        while (entries[index].occupied && !(entries[index].key == key))
            index = (index + 1) & mask;
        return index;
    }

public:
    class Growth {
    public:
        Growth() = default;
        Growth(Growth &&) noexcept = default;
        Growth &operator=(Growth &&) noexcept = default;
        Growth(const Growth &) = delete;
        Growth &operator=(const Growth &) = delete;
        bool hasStorage() const noexcept { return snapshot || replacement; }
        void allocate()
        {
            Q_ASSERT(stage == 0 && owner && newBuckets > oldBuckets);
            snapshot = allocateArray(budget, oldBuckets);
            replacement = allocateArray(budget, newBuckets);
            stage = 1;
        }
        void build() noexcept
        {
            Q_ASSERT(stage == 2);
            std::fill_n(replacement.get(), newBuckets, Entry{});
            for (size_t i = 0; i < oldBuckets; ++i) {
                const auto &entry = snapshot[i];
                if (entry.occupied)
                    replacement[bucket(replacement.get(), newBuckets, entry.key, seed)] = entry;
            }
            stage = 3;
        }
    private:
        const KisMutationAdmissionTable *owner = nullptr;
        KisBackingBudgetController *budget = nullptr;
        quint64 revision = 0;
        size_t seed = 0;
        size_t oldBuckets = 0;
        size_t newBuckets = 0;
        quint8 stage = 0;
        Array snapshot{nullptr, DeleteArray{}};
        Array replacement{nullptr, DeleteArray{}};
        friend class KisMutationAdmissionTable;
    };

    explicit KisMutationAdmissionTable(KisBackingBudgetController *budget = nullptr) : m_budget(budget) {}
    KisMutationAdmissionTable(const KisMutationAdmissionTable &) = delete;
    KisMutationAdmissionTable &operator=(const KisMutationAdmissionTable &) = delete;
    size_t size() const noexcept { return m_size; }
    bool contains(const Key &key) const noexcept { return find(key); }
    const Value *find(const Key &key) const noexcept
    {
        const auto &entry = data()[bucket(data(), m_buckets, key, m_seed)];
        return entry.occupied ? &entry.value : nullptr;
    }
    Value *find(const Key &key) noexcept
    { return const_cast<Value *>(std::as_const(*this).find(key)); }
    bool canInsert(size_t additional) const noexcept { return additional <= m_buckets / 2 - m_size; }
    Growth planGrowth(size_t additional) const
    {
        // Saturation invalidates every outstanding older candidate. Existing
        // records can still terminate; never wrap and accept an old snapshot.
        if (!additional || canInsert(additional) || m_revision == std::numeric_limits<quint64>::max()
            || additional > std::numeric_limits<size_t>::max() - m_size)
            throw std::bad_alloc();
        Growth result;
        result.owner = this;
        result.budget = m_budget;
        result.revision = m_revision;
        result.seed = m_seed;
        result.oldBuckets = m_buckets;
        result.newBuckets = m_buckets;
        while (result.newBuckets / 2 < m_size + additional) {
            if (result.newBuckets > size_t(std::numeric_limits<qint64>::max()) / sizeof(Entry) / 2)
                throw std::bad_alloc();
            result.newBuckets *= 2;
        }
        return result;
    }
    bool capture(Growth &growth, size_t additional = 0) const noexcept
    {
        // Allocation prepared geometry, not a snapshot. Unrelated record/value
        // changes before this copy do not invalidate that storage. Capture the
        // current revision only after checking both actual buffer bounds and
        // the caller's freshly revalidated insertion requirement.
        if (growth.owner != this || (growth.stage != 1 && growth.stage != 3) || growth.oldBuckets != m_buckets
            || m_revision == std::numeric_limits<quint64>::max()
            || additional > growth.newBuckets / 2 - m_size) return false;
        std::copy_n(data(), m_buckets, growth.snapshot.get());
        growth.revision = m_revision;
        growth.stage = 2;
        return true;
    }
    bool install(Growth &growth, size_t additional = 0) noexcept
    {
        if (growth.owner != this || growth.stage != 3 || growth.revision != m_revision
            || growth.oldBuckets != m_buckets
            || additional > growth.newBuckets / 2 - m_size) return false;
        m_storage.swap(growth.replacement);
        m_buckets = growth.newBuckets;
        growth.stage = 4;
        changed();
        return true;
    }
    bool insertPrepared(const Key &key, const Value &value) noexcept
    {
        if (!canInsert(1)) return false;
        auto &entry = data()[bucket(data(), m_buckets, key, m_seed)];
        if (entry.occupied) return false;
        entry = {key, value, true};
        ++m_size;
        changed();
        return true;
    }
    bool erase(const Key &key) noexcept
    {
        Entry *entries = data();
        size_t hole = bucket(entries, m_buckets, key, m_seed);
        if (!entries[hole].occupied) return false;
        const size_t mask = m_buckets - 1;
        for (size_t next = (hole + 1) & mask; entries[next].occupied; next = (next + 1) & mask) {
            const size_t home = hash(entries[next].key, m_seed) & mask;
            if (((next - home) & mask) >= ((hole - home) & mask)) {
                entries[hole] = entries[next];
                hole = next;
            }
        }
        entries[hole] = {};
        --m_size;
        changed();
        return true;
    }
    // Every in-place value edit participates in snapshot validation, including
    // activity counter changes that leave the key set and size unchanged.
    void changed() noexcept
    {
        if (m_revision != std::numeric_limits<quint64>::max()) ++m_revision;
    }
    template<class Visitor> void forEach(Visitor &&visitor) const
    {
        for (size_t i = 0; i < m_buckets; ++i)
            if (data()[i].occupied) visitor(data()[i].key, data()[i].value);
    }

private:
    Entry *data() noexcept { return m_storage ? m_storage.get() : m_inline.data(); }
    const Entry *data() const noexcept { return m_storage ? m_storage.get() : m_inline.data(); }
    std::array<Entry, InlineBuckets> m_inline{};
    Array m_storage{nullptr, DeleteArray{}};
    KisBackingBudgetController *m_budget = nullptr;
    const size_t m_seed = QHashSeed::globalSeed();
    size_t m_buckets = InlineBuckets;
    size_t m_size = 0;
    quint64 m_revision = 1;
};

// A storage identity, meaningful only with its original storage/session owner.
// This is not a logical page generation or a writer capability.
struct KisMutationSlotHandle {
    quint32 index = std::numeric_limits<quint32>::max();
    quint64 incarnation = 0;
    bool isValid() const { return index != std::numeric_limits<quint32>::max() && incarnation; }
    friend bool operator==(KisMutationSlotHandle a, KisMutationSlotHandle b)
    { return a.index == b.index && a.incarnation == b.incarnation; }
};

// Externally synchronized stable slots. Live enumeration follows occupied
// links, not a historical high-water index. Erasure recycles slots and detaches
// empty blocks; takeReleasedBlocks() transfers their actual free outside the
// owner's gate. One empty warm block may remain, and stays charged.
// Generation is narrower only in exhaustion tests; production uses quint64.
template<class T, size_t BlockElements, class Generation = quint64>
class KisMutationStorage
{
    static_assert(BlockElements > 0);
    static_assert(std::is_unsigned_v<Generation>);
    static constexpr quint32 Invalid = std::numeric_limits<quint32>::max();
    struct Slot {
        std::optional<T> value;
        quint64 incarnation = 0;
        quint32 previous = Invalid;
        quint32 next = Invalid;
    };
    struct Block {
        std::array<Slot, BlockElements> slots;
        Block *nextReleased = nullptr;
        quint32 live = 0;
    };
    struct DeleteBlock {
        KisBackingBudgetController *budget = nullptr;
        void operator()(Block *block) const noexcept
        {
            if (!block) return;
            block->~Block();
            KisMutationStorageAllocator<Block>(budget).deallocate(block, 1);
        }
    };
    using OwnedBlock = std::unique_ptr<Block, DeleteBlock>;
    struct DirectoryEntry {
        OwnedBlock block{nullptr, DeleteBlock{}};
        quint32 nextVacant = Invalid;
    };

public:
    using Handle = KisMutationSlotHandle;
    class ReleasedBlocks {
    public:
        ReleasedBlocks() = default;
        ReleasedBlocks(ReleasedBlocks &&other) noexcept
            : m_head(std::exchange(other.m_head, nullptr)), m_budget(other.m_budget) {}
        ReleasedBlocks &operator=(ReleasedBlocks &&other) noexcept
        {
            if (this != &other) {
                reset(); m_head = std::exchange(other.m_head, nullptr); m_budget = other.m_budget;
            }
            return *this;
        }
        ReleasedBlocks(const ReleasedBlocks &) = delete;
        ReleasedBlocks &operator=(const ReleasedBlocks &) = delete;
        ~ReleasedBlocks() { reset(); }
        bool isEmpty() const { return !m_head; }
        void reset() noexcept
        {
            while (m_head) {
                Block *block = m_head; m_head = block->nextReleased;
                DeleteBlock{m_budget}(block);
            }
        }
    private:
        Block *m_head = nullptr;
        KisBackingBudgetController *m_budget = nullptr;
        friend class KisMutationStorage;
    };

    explicit KisMutationStorage(KisBackingBudgetController *budget = nullptr)
        : m_blocks(KisMutationStorageAllocator<DirectoryEntry>(budget)) {}
    KisMutationStorage(KisMutationStorage &&other) noexcept
        : m_blocks(std::move(other.m_blocks)), m_released(std::move(other.m_released))
        , m_size(std::exchange(other.m_size, 0)), m_attached(std::exchange(other.m_attached, 0))
        , m_freeHead(std::exchange(other.m_freeHead, Invalid)), m_freeTail(std::exchange(other.m_freeTail, Invalid))
        , m_liveHead(std::exchange(other.m_liveHead, Invalid)), m_liveTail(std::exchange(other.m_liveTail, Invalid))
        , m_vacant(std::exchange(other.m_vacant, Invalid))
        , m_nextIncarnation(std::exchange(other.m_nextIncarnation, 0)) {}
    KisMutationStorage &operator=(KisMutationStorage &&other) noexcept
    {
        if (this != &other) {
            m_released = std::move(other.m_released);
            m_blocks = std::move(other.m_blocks);
            m_size = std::exchange(other.m_size, 0); m_attached = std::exchange(other.m_attached, 0);
            m_freeHead = std::exchange(other.m_freeHead, Invalid); m_freeTail = std::exchange(other.m_freeTail, Invalid);
            m_liveHead = std::exchange(other.m_liveHead, Invalid); m_liveTail = std::exchange(other.m_liveTail, Invalid);
            m_vacant = std::exchange(other.m_vacant, Invalid);
            m_nextIncarnation = std::exchange(other.m_nextIncarnation, 0);
        }
        return *this;
    }
    KisMutationStorage(const KisMutationStorage &) = delete;
    KisMutationStorage &operator=(const KisMutationStorage &) = delete;

    void reserve(size_t count)
    {
        if (count > Invalid - 1u) throw std::bad_alloc();
        while (count > m_attached * BlockElements) {
            auto *budget = m_blocks.get_allocator().budget;
            const bool append = m_vacant == Invalid;
            const size_t directory = append ? m_blocks.size() : m_vacant;
            if (directory >= (Invalid - 1u) / BlockElements) throw std::bad_alloc();
            if (append && m_blocks.size() == m_blocks.capacity())
                m_blocks.reserve(m_blocks.capacity() ? 2 * m_blocks.capacity() : size_t(1));
            KisMutationStorageAllocator<Block> allocator(budget);
            Block *raw = allocator.allocate(1);
            try { ::new (static_cast<void *>(raw)) Block(); }
            catch (...) { allocator.deallocate(raw, 1); throw; }
            OwnedBlock prepared(raw, DeleteBlock{budget});
            if (append) m_blocks.emplace_back();
            else m_vacant = m_blocks[directory].nextVacant;
            auto &entry = m_blocks[directory]; entry.block = std::move(prepared); entry.nextVacant = Invalid;
            ++m_attached;
            for (size_t i = 0; i < BlockElements; ++i)
                appendLink(quint32(directory * BlockElements + i), m_freeHead, m_freeTail);
        }
    }
    Handle prepareSlot()
    {
        if (!m_nextIncarnation) throw std::bad_alloc();
        reserve(m_size + 1);
        Q_ASSERT(m_freeHead != Invalid);
        return {m_freeHead, quint64(m_nextIncarnation)};
    }
    template<class... Args> T *emplacePrepared(Handle handle, Args &&...args)
    {
        if (handle.index != m_freeHead || !handle.isValid() || handle.incarnation != m_nextIncarnation)
            return nullptr;
        Slot *slot = slotAt(handle.index);
        Q_ASSERT(slot && !slot->value && !slot->incarnation);
        slot->value.emplace(std::forward<Args>(args)...);
        removeLink(handle.index, m_freeHead, m_freeTail);
        slot->incarnation = handle.incarnation;
        m_nextIncarnation = m_nextIncarnation == std::numeric_limits<Generation>::max()
            ? 0 : Generation(m_nextIncarnation + 1);
        appendLink(handle.index, m_liveHead, m_liveTail);
        ++m_size; ++m_blocks[handle.index / BlockElements].block->live;
        return &*slot->value;
    }
    template<class... Args> T &emplace_back(Args &&...args)
    {
        auto *value = emplacePrepared(prepareSlot(), std::forward<Args>(args)...);
        Q_ASSERT(value); return *value;
    }
    size_t size() const { return m_size; }
    Handle handleAt(size_t index) const
    {
        const auto *slot = slotAt(index);
        return slot && slot->value && slot->incarnation ? Handle{quint32(index), slot->incarnation} : Handle{};
    }
    Handle first() const { return handleAt(m_liveHead); }
    Handle next(Handle current) const
    {
        const auto *slot = slotAt(current.index);
        return at(current) ? handleAt(slot->next) : Handle{};
    }
    T *at(Handle handle) { return const_cast<T *>(std::as_const(*this).at(handle)); }
    const T *at(Handle handle) const
    {
        const auto *slot = slotAt(handle.index);
        return handle.isValid() && slot && slot->incarnation == handle.incarnation && slot->value ? &*slot->value : nullptr;
    }
    T *at(size_t index) { return at(handleAt(index)); }
    const T *at(size_t index) const { return at(handleAt(index)); }
    T &operator[](size_t index) { Q_ASSERT(at(index)); return *at(index); }
    const T &operator[](size_t index) const { Q_ASSERT(at(index)); return *at(index); }
    bool erase(Handle handle)
    {
        if (!at(handle)) return false;
        auto *slot = slotAt(handle.index);
        removeLink(handle.index, m_liveHead, m_liveTail);
        slot->incarnation = 0; slot->value.reset(); --m_size;
        auto &block = m_blocks[handle.index / BlockElements].block;
        --block->live;
        // Prefer a just-released slot while keeping untouched reserves ordered.
        prependFree(handle.index);
        if (!block->live && m_attached > 1) detachEmptyBlock(handle.index / BlockElements);
        return true;
    }
    bool erase(size_t index) { return erase(handleAt(index)); }
    ReleasedBlocks takeReleasedBlocks() noexcept { return std::move(m_released); }

private:
    Slot *slotAt(size_t index)
    { return const_cast<Slot *>(std::as_const(*this).slotAt(index)); }
    const Slot *slotAt(size_t index) const
    {
        if (index == Invalid || index / BlockElements >= m_blocks.size()) return nullptr;
        const auto &block = m_blocks[index / BlockElements].block;
        return block ? &block->slots[index % BlockElements] : nullptr;
    }
    void appendLink(quint32 index, quint32 &head, quint32 &tail)
    {
        auto &slot = *slotAt(index); slot.previous = tail; slot.next = Invalid;
        if (tail != Invalid) slotAt(tail)->next = index; else head = index;
        tail = index;
    }
    void removeLink(quint32 index, quint32 &head, quint32 &tail)
    {
        auto &slot = *slotAt(index);
        if (slot.previous != Invalid) slotAt(slot.previous)->next = slot.next; else head = slot.next;
        if (slot.next != Invalid) slotAt(slot.next)->previous = slot.previous; else tail = slot.previous;
        slot.previous = slot.next = Invalid;
    }
    void prependFree(quint32 index)
    {
        auto &slot = *slotAt(index); slot.previous = Invalid; slot.next = m_freeHead;
        if (m_freeHead != Invalid) slotAt(m_freeHead)->previous = index; else m_freeTail = index;
        m_freeHead = index;
    }
    void detachEmptyBlock(size_t directory)
    {
        auto &entry = m_blocks[directory]; Q_ASSERT(entry.block && !entry.block->live);
        for (size_t i = 0; i < BlockElements; ++i)
            removeLink(quint32(directory * BlockElements + i), m_freeHead, m_freeTail);
        Block *block = entry.block.release();
        block->nextReleased = m_released.m_head; m_released.m_head = block;
        m_released.m_budget = m_blocks.get_allocator().budget;
        entry.nextVacant = m_vacant; m_vacant = quint32(directory); --m_attached;
    }

    std::vector<DirectoryEntry, KisMutationStorageAllocator<DirectoryEntry>> m_blocks;
    ReleasedBlocks m_released;
    size_t m_size = 0;
    size_t m_attached = 0;
    quint32 m_freeHead = Invalid, m_freeTail = Invalid;
    quint32 m_liveHead = Invalid, m_liveTail = Invalid;
    quint32 m_vacant = Invalid;
    Generation m_nextIncarnation = 1;
};

#endif
