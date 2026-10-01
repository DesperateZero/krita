/*------------------------------------------------------------------------
  Junction: Concurrent data structures in C++
  Copyright (c) 2016 Jeff Preshing
  Distributed under the Simplified BSD License.
  Original location: https://github.com/preshing/junction
  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the LICENSE file for more information.
------------------------------------------------------------------------*/

#ifndef CONCURRENTMAP_H
#define CONCURRENTMAP_H

#include "leapfrog.h"
#include "qsbr.h"
#include <QtCore/qyieldcpu.h>
#include <atomic>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

template<bool Enabled>
struct ConcurrentMapPreparationState {};

template<>
struct ConcurrentMapPreparationState<true> {
    std::atomic_flag preparationGate = ATOMIC_FLAG_INIT;
    std::atomic<bool> preparationsClosed{false};
    void *preparedHead = nullptr;
};

template <typename K, typename V, class KT = DefaultKeyTraits<K>, class VT = DefaultValueTraits<V>,
          bool PreparedKeys = false>
class ConcurrentMap : private ConcurrentMapPreparationState<PreparedKeys>
{
public:
    typedef K Key;
    typedef V Value;
    typedef KT KeyTraits;
    typedef VT ValueTraits;
    typedef quint32 Hash;
    typedef Leapfrog<ConcurrentMap> Details;

private:
    friend class KisLocklessStackTest;
    Atomic<typename Details::Table*> m_root;
    QSBR m_gc;

    // Only opted-in, aligned pointer maps use a tagged temporary value. The
    // normal cell layout and all non-opted-in map representations stay intact.
    static_assert(!PreparedKeys || std::is_pointer_v<Value>);
    class PreparationGuard {
    public:
        explicit PreparationGuard(std::atomic_flag &gate) noexcept : m_gate(gate)
        { while (m_gate.test_and_set(std::memory_order_acquire)) qYieldCpu(); }
        ~PreparationGuard() { m_gate.clear(std::memory_order_release); }
        PreparationGuard(const PreparationGuard &) = delete;
    private:
        std::atomic_flag &m_gate;
    };

    struct alignas(8) PreparedEntry {
        std::atomic_flag gate = ATOMIC_FLAG_INIT;
        std::atomic<unsigned> references{2}; // map/grace record and first token
        unsigned pins = 1; // under gate; this is storage retention, not a writer
        ConcurrentMap *owner;
        const Hash hash;
        Atomic<Value> value{Value(ValueTraits::NullValue)};
        PreparedEntry *previous = nullptr;
        PreparedEntry *next = nullptr;
        bool listed = false; // protected by the map's preparation list gate
        QSBR::Action reclamation;
        void (*dispose)(PreparedEntry *) noexcept;

        PreparedEntry(ConcurrentMap *owner, Hash hash,
                      void (*dispose)(PreparedEntry *) noexcept)
            : owner(owner), hash(hash), dispose(dispose)
        { reclamation.bind(&PreparedEntry::releaseReference, this); }

        void retainReference() noexcept
        { references.fetch_add(1, std::memory_order_relaxed); }
        void releaseReference() noexcept
        {
            if (references.fetch_sub(1, std::memory_order_acq_rel) == 1) dispose(this);
        }
    };

    template<class Allocator>
    struct AllocatedPreparedEntry : PreparedEntry {
        using Storage = typename std::allocator_traits<Allocator>::template rebind_alloc<AllocatedPreparedEntry>;
        Storage allocator;
        AllocatedPreparedEntry(ConcurrentMap *owner, Hash hash, const Storage &allocator)
            : PreparedEntry(owner, hash, &AllocatedPreparedEntry::destroy), allocator(allocator) {}
        static void destroy(PreparedEntry *base) noexcept
        {
            auto *entry = static_cast<AllocatedPreparedEntry *>(base);
            Storage allocator(entry->allocator);
            std::allocator_traits<Storage>::destroy(allocator, entry);
            std::allocator_traits<Storage>::deallocate(allocator, entry, 1);
        }
    };

    static PreparedEntry *preparedEntry(Value value) noexcept
    {
        if constexpr (PreparedKeys) {
            const quintptr bits = reinterpret_cast<quintptr>(value);
            if ((bits & 3) == 2) return reinterpret_cast<PreparedEntry *>(bits & ~quintptr(3));
        }
        return nullptr;
    }
    static Value encodedEntry(PreparedEntry *entry) noexcept
    { return reinterpret_cast<Value>(reinterpret_cast<quintptr>(entry) | quintptr(2)); }
    static Value logicalValue(Value value) noexcept
    {
        if (auto *entry = preparedEntry(value)) return entry->value.load(Acquire);
        return value;
    }

    void listPreparedEntry(PreparedEntry *entry) noexcept
    {
        PreparationGuard lock(this->preparationGate);
        entry->next = static_cast<PreparedEntry *>(this->preparedHead);
        if (entry->next) entry->next->previous = entry;
        this->preparedHead = entry;
        entry->listed = true;
    }
    void unlistPreparedEntry(PreparedEntry *entry) noexcept
    {
        PreparationGuard lock(this->preparationGate);
        if (!entry->listed) return;
        if (entry->previous) entry->previous->next = entry->next;
        else this->preparedHead = entry->next;
        if (entry->next) entry->next->previous = entry->previous;
        entry->listed = false;
        entry->previous = entry->next = nullptr;
    }

    // Caller holds entry->gate. Migrations only copy the encoded address;
    // ordinary updates of this key take that same gate. A failed source CAS
    // in migrateRange copies the new plain value before publishing Redirect.
    void detachPreparedEntry(PreparedEntry *entry) noexcept
    {
        QSBR::RawPointerAccess access(m_gc);
        for (;;) {
            const auto location = locateExistingCell(entry->hash);
            Q_ASSERT(location.cell && location.value == encodedEntry(entry));
            Value expected = encodedEntry(entry);
            if (location.cell->value.compareExchangeStrong(expected, entry->value.load(Relaxed), ConsumeRelease)) break;
        }
        entry->owner = nullptr;
        unlistPreparedEntry(entry);
        m_gc.enqueueEmbedded(entry->reclamation);
    }

    struct LocatedCell {
        typename Details::Table *table;
        typename Details::Cell *cell;
        Value value;
    };

    static LocatedCell locateInTable(typename Details::Table *table, Hash hash)
    {
        auto *cell = Details::find(hash, table);
        return {table, cell, cell ? cell->value.load(Acquire) : Value(ValueTraits::NullValue)};
    }

    // Reads and removals never need to allocate a successor table. A Redirect
    // publishes the copied destination value with release ordering. During
    // overflow, each immutable source precedes the next partial destination;
    // the first non-redirected cell is the authoritative location of this key.
    LocatedCell locateExistingCell(Hash hash)
    {
        for (;;) {
            auto *root = m_root.load(Consume);
            auto result = locateInTable(root, hash);
            if (result.value != Value(ValueTraits::Redirect)) return result;
            auto *job = root->jobCoordinator.loadConsume();
            if (quintptr(job) <= 1) continue; // The root has just advanced.
            auto *migration = static_cast<typename Details::TableMigration *>(job);
            for (quint64 i = 1; i < migration->m_numSources; ++i) {
                result = locateInTable(migration->getSources()[i].table, hash);
                if (result.cell && result.value != Value(ValueTraits::Redirect)) return result;
            }
            result = locateInTable(migration->m_destination, hash);
            if (result.value != Value(ValueTraits::Redirect)) return result;
            // That destination has itself been redirected into a successor.
            // Reload the root/job; no helping, waiting, or storage preparation.
        }
    }

public:
    ConcurrentMap(quint64 capacity = Details::InitialSize) : m_root(Details::Table::create(capacity))
    {
    }

    ~ConcurrentMap()
    {
        closePreparedKeys();
        typename Details::Table* table = m_root.loadNonatomic();
        auto *job = table->jobCoordinator.loadConsume();
        if (quintptr(job) > 1) {
            // Quiescent destruction must also own a rejected, unfinished
            // migration. It cannot allocate merely to finish that migration.
            static_cast<typename Details::TableMigration *>(job)->discardUnpublished();
        } else {
            table->destroy();
        }
        m_gc.flush();
    }

    // Normal map callers/migration workers must already be quiescent. Tokens
    // may still exist or be finishing: detach their records before destroying
    // table storage, and let their own references keep closed records alive.
    // This only walks outstanding preparations, never every key in the map.
    void closePreparedKeys() noexcept
    {
        if constexpr (PreparedKeys) {
            this->preparationsClosed.store(true, std::memory_order_release);
            for (;;) {
                PreparedEntry *entry;
                {
                    PreparationGuard lock(this->preparationGate);
                    entry = static_cast<PreparedEntry *>(this->preparedHead);
                    if (!entry) break;
                    entry->retainReference();
                    this->preparedHead = entry->next;
                    if (entry->next) entry->next->previous = nullptr;
                    entry->previous = entry->next = nullptr;
                    entry->listed = false;
                }
                {
                    PreparationGuard lock(entry->gate);
                    if (entry->owner == this) detachPreparedEntry(entry);
                }
                entry->releaseReference();
            }
            // A concurrent final token release can have unlisted its record
            // just before enqueueing it. Its raw access covers that handoff.
            while (m_gc.sanityRawPointerAccessLocked()) qYieldCpu();
        }
    }

    class PreparedKey {
    public:
        struct Result {
            bool accepted = false;
            Value previous = Value(ValueTraits::NullValue);
        };
        PreparedKey() = default;
        PreparedKey(const PreparedKey &) = delete;
        PreparedKey &operator=(const PreparedKey &) = delete;
        PreparedKey(PreparedKey &&other) noexcept : m_entry(std::exchange(other.m_entry, nullptr)) {}
        PreparedKey &operator=(PreparedKey &&other) noexcept
        {
            if (this != &other) { reset(); m_entry = std::exchange(other.m_entry, nullptr); }
            return *this;
        }
        ~PreparedKey() { reset(); }
        explicit operator bool() const noexcept { return m_entry; }

        // Storage capability only. Pointer results require the caller's usual
        // payload/raw protection. The token itself pins neither a tile nor a
        // logical page version, and holds no raw/table lock across a kernel.
        Result exchangeValue(Value desired) noexcept { return replace(desired, false); }
        Result insertIfAbsentValue(Value desired) noexcept { return replace(desired, true); }
        bool belongsTo(const ConcurrentMap &map) const noexcept
        {
            if (!m_entry) return false;
            PreparationGuard lock(m_entry->gate);
            return m_entry->owner == &map;
        }
        void reset() noexcept
        {
            PreparedEntry *entry = std::exchange(m_entry, nullptr);
            if (!entry) return;
            {
                PreparationGuard lock(entry->gate);
                Q_ASSERT(entry->pins);
                if (--entry->pins == 0 && entry->owner) entry->owner->detachPreparedEntry(entry);
            }
            entry->releaseReference();
        }
    private:
        friend class ConcurrentMap;
        explicit PreparedKey(PreparedEntry *entry) noexcept : m_entry(entry) {}
        Result replace(Value desired, bool onlyIfAbsent) noexcept
        {
            if (!m_entry) return {};
            PreparationGuard lock(m_entry->gate);
            if (!m_entry->owner) return {};
            Q_ASSERT(!preparedEntry(desired) && desired != Value(ValueTraits::Redirect));
            const Value old = m_entry->value.load(Relaxed);
            if (!onlyIfAbsent || old == Value(ValueTraits::NullValue)) m_entry->value.store(desired, Release);
            return {true, old};
        }
        PreparedEntry *m_entry = nullptr;
    };

    // Allocation belongs to preparation. A stateful allocator is retained in
    // the real record through cancellation, map close and the QSBR callback;
    // it must keep its allocation/charge owner alive until deallocate().
    template<class Allocator = std::allocator<std::byte>>
    PreparedKey prepareKey(Key key, const Allocator &allocator = {})
    {
        static_assert(PreparedKeys, "Prepared keys require the aligned pointer representation");
        static_assert(alignof(std::remove_pointer_t<Value>) >= 4);
        if (this->preparationsClosed.load(std::memory_order_acquire)) return {};
        using Entry = AllocatedPreparedEntry<Allocator>;
        using Storage = typename Entry::Storage;
        const Hash hash = KeyTraits::hash(key);
        std::unique_ptr<PreparedEntry, void(*)(PreparedEntry *)> candidate(nullptr,
            [](PreparedEntry *entry) { entry->dispose(entry); });
        for (;;) {
            {
                QSBR::RawPointerAccess access(m_gc);
                const auto location = locateExistingCell(hash);
                if (auto *entry = preparedEntry(location.value)) {
                    PreparationGuard lock(entry->gate);
                    if (entry->owner != this) continue;
                    if (entry->references.load(std::memory_order_relaxed) >=
                        std::numeric_limits<unsigned>::max() / 2) throw std::bad_alloc();
                    ++entry->pins;
                    entry->retainReference();
                    return PreparedKey(entry);
                }
            }
            if (!candidate) {
                Storage storage(allocator);
                Entry *entry = std::allocator_traits<Storage>::allocate(storage, 1);
                try { std::allocator_traits<Storage>::construct(storage, entry, this, hash, storage); }
                catch (...) { std::allocator_traits<Storage>::deallocate(storage, entry, 1); throw; }
                candidate.reset(entry);
            }
            QSBR::RawPointerAccess access(m_gc);
            auto cell = insertOrFind(key);
            if (preparedEntry(cell.m_storedValue)) continue;
            candidate->value.storeNonatomic(logicalValue(cell.m_storedValue));
            Value expected = cell.m_storedValue;
            if (!cell.m_cell->value.compareExchangeStrong(expected, encodedEntry(candidate.get()), ConsumeRelease)) continue;
            listPreparedEntry(candidate.get());
            return PreparedKey(candidate.release());
        }
    }

    QSBR &getGC()
    {
        return m_gc;
    }

    bool migrationInProcess()
    {
        return quint64(m_root.loadNonatomic()->jobCoordinator.loadConsume()) > 1;
    }

    // publishTableMigration() is called by exactly one thread from Details::TableMigration::run()
    // after all the threads participating in the migration have completed their work.
    void publishTableMigration(typename Details::TableMigration* migration)
    {
        m_root.store(migration->m_destination, Release);
        // Caller will GC the TableMigration and the source table.
    }

    // A Mutator represents a known cell in the hash table.
    // It's meant for manipulations within a temporary function scope.
    // Obviously you must not call QSBR::Update while holding a Mutator.
    // Any operation that modifies the table (exchangeValue, eraseValue)
    // may be forced to follow a redirected cell, which changes the Mutator itself.
    // Note that even if the Mutator was constructed from an existing cell,
    // exchangeValue() can still trigger a resize if the existing cell was previously marked deleted,
    // or if another thread deletes the key between the two steps.
    class Mutator
    {
    private:
        friend class ConcurrentMap;

        ConcurrentMap& m_map;
        typename Details::Table* m_table;
        typename Details::Cell* m_cell;
        Value m_value;
        Value m_storedValue = Value(ValueTraits::NullValue);

        void readStorage(Value stored) noexcept
        {
            m_storedValue = stored;
            m_value = logicalValue(stored);
        }

        enum ReplaceResult { Replaced, Conflict, Redirected };
        ReplaceResult compareValue(Value desired) noexcept
        {
            if constexpr (!PreparedKeys) {
                if (m_cell->value.compareExchangeStrong(m_value, desired, ConsumeRelease)) return Replaced;
                return m_value == Value(ValueTraits::Redirect) ? Redirected : Conflict;
            } else {
                for (;;) {
                    if (auto *entry = preparedEntry(m_storedValue)) {
                        {
                            PreparationGuard lock(entry->gate);
                            if (entry->owner == &m_map) {
                                if (entry->value.compareExchangeStrong(m_value, desired, ConsumeRelease)) return Replaced;
                                return Conflict;
                            }
                        }
                        // Last-pin release/map close restored an ordinary
                        // cell. Keep the logical expected value: changing the
                        // storage representation is not a competing write.
                        const auto location = m_map.locateExistingCell(m_cell->hash.load(Relaxed));
                        if (!location.cell) return Redirected;
                        m_table = location.table;
                        m_cell = location.cell;
                        m_storedValue = location.value;
                        continue;
                    }
                    Value expected = m_value;
                    if (m_cell->value.compareExchangeStrong(expected, desired, ConsumeRelease)) {
                        m_storedValue = desired;
                        return Replaced;
                    }
                    m_storedValue = expected;
                    if (preparedEntry(expected)) continue; // newly prepared, same logical value
                    m_value = expected;
                    return expected == Value(ValueTraits::Redirect) ? Redirected : Conflict;
                }
            }
        }

        // Constructor: Find existing cell
        Mutator(ConcurrentMap& map, Key key, bool) : m_map(map)
        {
            const auto location = map.locateExistingCell(KeyTraits::hash(key));
            m_table = location.table;
            m_cell = location.cell;
            readStorage(location.value);
        }

        // Constructor: Insert or find cell
        Mutator(ConcurrentMap& map, Key key) : m_map(map), m_value(Value(ValueTraits::NullValue))
        {
            Hash hash = KeyTraits::hash(key);
            for (;;) {
                m_table = m_map.m_root.load(Consume);
                quint64 overflowIdx;
                switch (Details::insertOrFind(hash, m_table, m_cell, overflowIdx)) { // Modifies m_cell
                case Details::InsertResult_InsertedNew: {
                    // A migration may already have redirected this unused
                    // cell before we claimed its hash. Resolve that existing
                    // migration now, before returning a prepared cell.
                    const Value stored = m_cell->value.load(Acquire);
                    if (stored == Value(ValueTraits::Redirect)) break;
                    if constexpr (PreparedKeys) readStorage(stored);
                    return;
                }
                case Details::InsertResult_AlreadyFound: {
                    // The hash was already found in the table.
                    Value value = m_cell->value.load(Acquire);
                    if (value == Value(ValueTraits::Redirect)) {
                        // We've encountered a Redirect value.
                        break; // Help finish the migration.
                    }
                    // Found an existing value
                    readStorage(value);
                    return;
                }
                case Details::InsertResult_Overflow: {
                    // Unlike ConcurrentMap_Linear, we don't need to keep track of & pass a "mustDouble" flag.
                    // Passing overflowIdx is sufficient to prevent an infinite loop here.
                    // It defines the start of the range of cells to check while estimating total cells in use.
                    // After the first migration, deleted keys are purged, so if we hit this line during the
                    // second loop iteration, every cell in the range will be in use, thus the estimate will be 100%.
                    // (Concurrent deletes could result in further iterations, but it will eventually settle.)
                    Details::beginTableMigration(m_map, m_table, overflowIdx);
                    break;
                }
                }
                // A migration has been started (either by us, or another thread). Participate until it's complete.
                m_table->jobCoordinator.participate();
                // Try again using the latest root.
            }
        }

    public:
        Value getValue() const
        {
            // Return previously loaded value. Don't load it again.
            return Value(m_value);
        }

        Value exchangeValue(Value desired)
        {
            return exchangeValueImpl(desired, false);
        }

        // Return NullValue on installation, otherwise the existing winner.
        // Unlike exchangeValue, this never replaces a non-null value, even
        // when migration redirects us after the original empty-cell lookup.
        Value insertIfAbsentValue(Value desired)
        {
            return exchangeValueImpl(desired, true);
        }

    private:
        Value exchangeValueImpl(Value desired, bool onlyIfAbsent)
        {
            for (;;) {
                if (onlyIfAbsent && m_value != Value(ValueTraits::NullValue)) {
                    return m_value;
                }
                Value oldValue = m_value;
                const ReplaceResult replacement = compareValue(desired);
                if (replacement == Replaced) {
                    // Exchange was successful. Return previous value.
                    Value result = m_value;
                    m_value = desired; // Leave the mutator in a valid state
                    return result;
                }
                // The CAS failed and m_value has been updated with the latest value.
                if (replacement != Redirected) {
                    if (onlyIfAbsent) {
                        // A racing erase may leave null again; retry the CAS.
                        if (m_value == Value(ValueTraits::NullValue)) continue;
                        return m_value;
                    }
                    if (oldValue == Value(ValueTraits::NullValue) && m_value != Value(ValueTraits::NullValue)) {
                        // racing write inserted new value
                    }
                    // There was a racing write (or erase) to this cell.
                    // Pretend we exchanged with ourselves, and just let the racing write win.
                    return desired;
                }

                // We've encountered a Redirect value. Help finish the migration.
                Hash hash = m_cell->hash.load(Relaxed);
                for (;;) {
                    // Help complete the migration.
                    m_table->jobCoordinator.participate();
                    // Try again in the new table.
                    m_table = m_map.m_root.load(Consume);
                    m_value = Value(ValueTraits::NullValue);
                    m_storedValue = m_value;
                    quint64 overflowIdx;

                    switch (Details::insertOrFind(hash, m_table, m_cell, overflowIdx)) { // Modifies m_cell
                    case Details::InsertResult_AlreadyFound:
                        readStorage(m_cell->value.load(Acquire));
                        if (m_storedValue == Value(ValueTraits::Redirect)) {
                            break;
                        }
                        goto breakOuter;
                    case Details::InsertResult_InsertedNew:
                        if constexpr (PreparedKeys) {
                            const Value stored = m_cell->value.load(Acquire);
                            if (stored == Value(ValueTraits::Redirect)) break;
                            readStorage(stored);
                        }
                        goto breakOuter;
                    case Details::InsertResult_Overflow:
                        Details::beginTableMigration(m_map, m_table, overflowIdx);
                        break;
                    }
                    // We were redirected... again
                }
            breakOuter:;
                // Try again in the new table.
            }
        }

    public:
        void assignValue(Value desired)
        {
            exchangeValue(desired);
        }

        Value eraseValue()
        {
            for (;;) {
                if (m_value == Value(ValueTraits::NullValue)) {
                    return Value(m_value);
                }

                const ReplaceResult replacement = compareValue(Value(ValueTraits::NullValue));
                if (replacement == Replaced) {
                    // Exchange was successful and a non-NULL value was erased and returned by reference in m_value.
                    Value result = m_value;
                    m_value = Value(ValueTraits::NullValue); // Leave the mutator in a valid state
                    return result;
                }

                // The CAS failed and m_value has been updated with the latest value.
                if (replacement != Redirected) {
                    // There was a racing write (or erase) to this cell.
                    // Pretend we erased nothing, and just let the racing write win.
                    return Value(ValueTraits::NullValue);
                }

                const auto location = m_map.locateExistingCell(m_cell->hash.load(Relaxed));
                m_table = location.table;
                m_cell = location.cell;
                readStorage(location.value);
            }
        }
    };

    Mutator insertOrFind(Key key)
    {
        return Mutator(*this, key);
    }

    Mutator find(Key key)
    {
        return Mutator(*this, key, false);
    }

    // Callers hold QSBR raw access until converting the result to ownership.
    Value get(Key key)
    {
        return logicalValue(locateExistingCell(KeyTraits::hash(key)).value);
    }

    Value assign(Key key, Value desired)
    {
        Mutator iter(*this, key);
        return iter.exchangeValue(desired);
    }

    Value exchange(Key key, Value desired)
    {
        Mutator iter(*this, key);
        return iter.exchangeValue(desired);
    }

    Value erase(Key key)
    {
        Mutator iter(*this, key, false);
        return iter.eraseValue();
    }

    // The easiest way to implement an Iterator is to prevent all Redirects.
    // The currrent Iterator does that by forbidding concurrent inserts.
    // To make it work with concurrent inserts, we'd need a way to block TableMigrations.
    class Iterator
    {
    private:
        typename Details::Table* m_table = nullptr;
        typename Details::TableMigration* m_migration = nullptr;
        quint64 m_source = 0;
        quint64 m_idx = 0;
        Key m_hash;
        Value m_value;

        bool advanceTable()
        {
            if (!m_migration) return false;
            ++m_source;
            if (m_source < m_migration->m_numSources) {
                m_table = m_migration->getSources()[m_source].table;
            } else if (m_source == m_migration->m_numSources) {
                m_table = m_migration->m_destination;
            } else {
                return false;
            }
            m_idx = quint64(-1);
            return true;
        }

    public:
        Iterator() = default;
        Iterator(ConcurrentMap& map) { setMap(map); }

        void setMap(ConcurrentMap& map)
        {
            // The caller excludes inserts/migration workers. A rejected job
            // can leave live values split between sources and destination;
            // enumerate them without allocating to complete the migration.
            m_table = map.m_root.load(Consume);
            auto *job = m_table->jobCoordinator.loadConsume();
            m_migration = quintptr(job) > 1
                ? static_cast<typename Details::TableMigration *>(job) : nullptr;
            m_source = 0;
            m_idx = quint64(-1);
            next();
        }

        void next()
        {
            do {
                while (++m_idx <= m_table->sizeMask) {
                    typename Details::CellGroup* group = m_table->getCellGroups() + (m_idx >> 2);
                    typename Details::Cell* cell = group->cells + (m_idx & 3);
                    m_hash = cell->hash.load(Relaxed);
                    if (m_hash != KeyTraits::NullHash) {
                        m_value = logicalValue(cell->value.load(Acquire));
                        if (m_value != Value(ValueTraits::NullValue) && m_value != Value(ValueTraits::Redirect)) return;
                    }
                }
            } while (advanceTable());
            m_hash = KeyTraits::NullHash;
            m_value = Value(ValueTraits::NullValue);
        }

        bool isValid() const
        {
#ifdef SANITY_CHECK
            KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_value != Value(ValueTraits::Redirect), false);
#endif
            return m_value != Value(ValueTraits::NullValue);
        }

        Key getKey() const
        {
            // Since we've forbidden concurrent inserts (for now), nonatomic would suffice here, but let's plan ahead:
            return KeyTraits::dehash(m_hash);
        }

        Value getValue() const
        {
            return m_value;
        }
    };
};

#endif // CONCURRENTMAP_LEAPFROG_H
