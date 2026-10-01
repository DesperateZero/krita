/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_lockless_stack_test.h"
#include <QStack>
#include <QThreadPool>
#include <simpletest.h>

#include "kis_debug.h"

#include "kis_lockless_stack.h"
#include "config-limit-long-tests.h"
#include "3rdparty/lock_free_map/qsbr.h"
#include "3rdparty/lock_free_map/concurrent_map.h"
#include "tiles3/kis_tile_hash_table2.h"
// Instantiate both storage implementations in one test translation unit.
#define KisTileHashTable ChainedTileHashTable
#define KisTileHashTableIterator ChainedTileHashTableIterator
#define KisTileHashTableConstIterator ChainedTileHashTableConstIterator
#include "tiles3/kis_tile_hash_table.h"
#undef KisTileHashTableConstIterator
#undef KisTileHashTableIterator
#undef KisTileHashTable
#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <QElapsedTimer>
#include <limits>

namespace {
struct ReclaimCounter {
    std::atomic<int> calls{0};
    void invoke() { ++calls; }
    static void invokeFree(ReclaimCounter *self) { self->invoke(); }
};

struct EmbeddedReclaim {
    QSBR::Action action;
    ReclaimCounter &counter;
    int &destroyed;
    EmbeddedReclaim(ReclaimCounter &counter, int &destroyed)
        : counter(counter), destroyed(destroyed) { action.bind(&EmbeddedReclaim::destroy, this); }
    ~EmbeddedReclaim() { ++destroyed; }
    void destroy() { counter.invoke(); delete this; }
};

std::atomic<int> hashTilesAlive{0};
struct HashTestTile : KisShared {
    int x, y;
    KisSharedPtr<HashTestTile> nextTile;
    KisSharedPtr<HashTestTile> next() const { return nextTile; }
    void setNext(KisSharedPtr<HashTestTile> tile) { nextTile = std::move(tile); }
    int attached = 0, detached = 0, dead = 0;
    bool failDetach = false;
    HashTestTile(int x, int y, KisTileData *, KisMementoManager *) : x(x), y(y) { ++hashTilesAlive; }
    ~HashTestTile() { --hashTilesAlive; }
    int col() const { return x; }
    int row() const { return y; }
    void notifyAttachedToDataManager(KisMementoManager *) { ++attached; }
    void notifyDeadWithoutDetaching() { ++dead; }
    void notifyDetachedFromDataManager() {
        ++detached;
        if (failDetach) throw std::bad_alloc();
    }
};

using PreparedPointerMap = ConcurrentMap<int, quint64 *, DefaultKeyTraits<int>, DefaultValueTraits<quint64 *>, true>;
struct PreparedStorageAccount {
    size_t liveBytes = 0;
    size_t allocations = 0;
    bool reject = false;
};
template<class T>
struct PreparedStorageAllocator {
    using value_type = T;
    std::shared_ptr<PreparedStorageAccount> account;
    explicit PreparedStorageAllocator(std::shared_ptr<PreparedStorageAccount> account) : account(std::move(account)) {}
    template<class U> PreparedStorageAllocator(const PreparedStorageAllocator<U> &other) noexcept : account(other.account) {}
    T *allocate(size_t count) {
        if (account->reject) throw std::bad_alloc();
        auto *result = std::allocator<T>{}.allocate(count);
        account->liveBytes += count * sizeof(T); ++account->allocations;
        return result;
    }
    void deallocate(T *data, size_t count) noexcept {
        std::allocator<T>{}.deallocate(data, count);
        account->liveBytes -= count * sizeof(T);
    }
};
}

void KisLocklessStackTest::testPreparedRetirement()
{
    QSBR gc;
    ReclaimCounter counter;
    {
        auto cancelled = QSBR::prepare(&ReclaimCounter::invoke, &counter);
        auto moved = std::move(cancelled);
        QVERIFY(!cancelled);
    }
    QCOMPARE(counter.calls.load(), 0);
    {
        QSBR::RawPointerAccess reader(gc);
        gc.enqueuePrepared(QSBR::prepare(&ReclaimCounter::invoke, &counter));
        gc.enqueuePrepared(QSBR::prepare(&ReclaimCounter::invokeFree, &counter), true);
        gc.update();
        QCOMPARE(counter.calls.load(), 0);
    }
    gc.update();
    QCOMPARE(counter.calls.load(), 2);
    gc.flush();
    QCOMPARE(counter.calls.load(), 2);
    try {
        QSBR::RawPointerAccess reader(gc);
        throw std::bad_alloc();
    } catch (const std::bad_alloc &) {}
    QVERIFY(!gc.sanityRawPointerAccessLocked());
}

void KisLocklessStackTest::testEmbeddedRetirement()
{
    QSBR gc;
    ReclaimCounter counter;
    int destroyed = 0;
    auto first = new EmbeddedReclaim(counter, destroyed);
    auto second = new EmbeddedReclaim(counter, destroyed);
    {
        QSBR::RawPointerAccess reader(gc);
        gc.enqueueEmbedded(first->action);
        gc.enqueueEmbedded(second->action, true);
        gc.update();
        QCOMPARE(destroyed, 0);
    }
    gc.update();
    QCOMPARE(counter.calls.load(), 2);
    QCOMPARE(destroyed, 2);
    gc.flush();
    QCOMPARE(destroyed, 2);
}

void KisLocklessStackTest::testConcurrentRetirement()
{
    QSBR gc;
    ReclaimCounter counter;
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 6; ++worker) {
        workers.emplace_back([&] {
            for (int i = 0; i < 2000; ++i) {
                gc.enqueuePrepared(QSBR::prepare(&ReclaimCounter::invoke, &counter), i % 2);
                if (i % 13 == 0) gc.update();
            }
        });
    }
    for (auto &worker : workers) worker.join();
    gc.flush();
    QCOMPARE(counter.calls.load(), 12000);
}

void KisLocklessStackTest::testInsertIfAbsentAcrossMigration()
{
    ConcurrentMap<int, int> map(8);
    {
        QSBR::RawPointerAccess reader(map.getGC());
        auto stale = map.insertOrFind(1);
        QCOMPARE(stale.getValue(), 0);
        // Keep the original empty mutator alive while several migrations
        // discard its null cell. The eventual winner lives in the new table.
        for (int i = 2; i <= 2048; ++i) QCOMPARE(map.assign(i, i + 10), 0);
        QCOMPARE(map.assign(1, 42), 0);
        QCOMPARE(stale.insertIfAbsentValue(99), 42);
        QCOMPARE(map.get(1), 42);
        QCOMPARE(map.erase(1), 42);
        auto empty = map.insertOrFind(1);
        QCOMPARE(empty.insertIfAbsentValue(99), 0);
        QCOMPARE(map.get(1), 99);
        // Ordinary replacement retains its original API semantics.
        QCOMPARE(map.assign(1, 100), 99);
    }
    map.getGC().flush();
}

void KisLocklessStackTest::testConcurrentInsertIfAbsent()
{
    ConcurrentMap<int, int> map(8);
    std::atomic<int> inserted{0};
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&, worker] {
            ++ready;
            while (ready.load() != 8) std::this_thread::yield();
            QSBR::RawPointerAccess reader(map.getGC());
            for (int i = 1; i <= 1024; ++i) {
                auto entry = map.insertOrFind(i);
                if (!entry.insertIfAbsentValue(worker + 10)) ++inserted;
            }
        });
    }
    for (auto &worker : workers) worker.join();
    QCOMPARE(inserted.load(), 1024);
    {
        QSBR::RawPointerAccess reader(map.getGC());
        for (int i = 1; i <= 1024; ++i) QVERIFY(map.get(i) >= 10 && map.get(i) < 18);
    }
    map.getGC().flush();
}

void KisLocklessStackTest::testTileHashReferenceLifetime()
{
    using Table = KisTileHashTableTraits2<HashTestTile>;
    KisSharedPtr<HashTestTile> held;
    QCOMPARE(hashTilesAlive.load(), 0);
    {
        Table table(nullptr), destination(nullptr);
        bool added = false;
        held = table.getTileLazy(1, 2, added);
        QVERIFY(added);
        QCOMPARE(held->attached, 1);
        auto same = table.getTileLazy(1, 2, added);
        QVERIFY(!added);
        QCOMPARE(same.data(), held.data());
        table.addTile(held); // Self-assignment must not detach a live value.
        QCOMPARE(held->dead, 0);
        QCOMPARE(table.numTiles(), 1);
        KisSharedPtr<HashTestTile> replacement = new HashTestTile(1, 2, nullptr, nullptr);
        table.addTile(replacement);
        QCOMPARE(held->dead, 1);
        QCOMPARE(table.numTiles(), 1);
        {
            KisTileHashTableIteratorTraits2<HashTestTile> iter(&table);
            iter.moveCurrentToHashTable(&destination);
            QVERIFY(iter.isDone());
        }
        QCOMPARE(table.numTiles(), 0);
        QCOMPARE(destination.numTiles(), 1);
        destination.clear();
        QCOMPARE(replacement->detached, 2);
        QVERIFY(destination.isEmpty());
        held = table.getTileLazy(3, 4, added);
        QVERIFY(added);
    }
    QCOMPARE(held->detached, 1); // External strong reference outlives the table.
    QCOMPARE(hashTilesAlive.load(), 1);
    held.clear();
    QCOMPARE(hashTilesAlive.load(), 0);
}

void KisLocklessStackTest::testTileHashNotificationFailure()
{
    using Table = KisTileHashTableTraits2<HashTestTile>;
    {
        Table table(nullptr);
        bool added;
        auto held = table.getTileLazy(1, 1, added);
        held->failDetach = true;
        QVERIFY_EXCEPTION_THROWN(table.deleteTile(1, 1), std::bad_alloc);
        QCOMPARE(table.numTiles(), 0);
        // A leaked raw-access count would hang this ordinary GC update.
        QVERIFY(!table.getExistingTile(1, 1));
        held.clear();
        QCOMPARE(hashTilesAlive.load(), 0);
        auto next = table.getTileLazy(2, 2, added);
        QVERIFY(added);
        next->failDetach = true;
        QVERIFY_EXCEPTION_THROWN(table.clear(), std::bad_alloc);
        QVERIFY(table.isEmpty());
        QVERIFY(!table.getExistingTile(2, 2));
    }
    QCOMPARE(hashTilesAlive.load(), 0);
}

void KisLocklessStackTest::testTileHashConcurrentClear()
{
    using Table = KisTileHashTableTraits2<HashTestTile>;
    Table table(nullptr);
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&, worker] {
            for (int i = 0; i < 1000; ++i) {
                bool added;
                const int key = i % 32 + 1;
                table.getTileLazy(key, worker, added);
                if (i % 3 == 0) table.deleteTile(key, worker);
            }
        });
    }
    workers.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            try { table.clear(); } catch (const std::bad_alloc &) {}
        }
    });
    for (auto &worker : workers) worker.join();
    int counted = 0;
    {
        KisTileHashTableIteratorTraits2<HashTestTile> iter(&table);
        for (; !iter.isDone(); iter.next()) ++counted;
    }
    QCOMPARE(table.numTiles(), counted);
    table.clear();
    QCOMPARE(hashTilesAlive.load(), 0);
}

void KisLocklessStackTest::testReadEraseDuringPartialMigration()
{
    using Map = ConcurrentMap<int, int>;
    Map map(512);
    {
        QSBR::RawPointerAccess access(map.getGC());
        for (int i = 1; i <= 256; ++i) map.assign(i, i + 10);
        auto *source = map.m_root.load(Acquire);
        Map::Details::beginTableMigrationToSize(map, source, 4);
        auto *job = static_cast<Map::Details::TableMigration *>(source->jobCoordinator.loadConsume());
        bool overflowed = false;
        for (quint64 start = 0; start <= source->sizeMask; start += Map::Details::TableMigrationUnitSize) {
            if (!job->migrateRange(source, start)) { overflowed = true; break; }
        }
        QVERIFY(overflowed);
        // A paused worker has copied a prefix, then hit a too-small target.
        // Lookups and removals must neither run it nor require a new table.
        for (int i = 1; i <= 256; ++i) QCOMPARE(map.get(i), i + 10);
        int removed = 0;
        for (int i = 1; i <= 256; i += 3) {
            QCOMPARE(map.erase(i), i + 10);
            ++removed;
        }
        QCOMPARE(map.m_root.load(Acquire), source);
        QCOMPARE(source->jobCoordinator.loadConsume(), static_cast<SimpleJobCoordinator::Job *>(job));
        QCOMPARE(job->m_workerStatus.load(Acquire), quint64(0));
        QSet<int> seen;
        for (Map::Iterator iter(map); iter.isValid(); iter.next()) {
            const int key = iter.getKey();
            QVERIFY(!seen.contains(key));
            QVERIFY((key - 1) % 3 != 0);
            QCOMPARE(iter.getValue(), key + 10);
            seen.insert(key);
        }
        QCOMPARE(seen.size(), 256 - removed);
        // The regular worker can finish this same partially copied state.
        source->jobCoordinator.participate();
        QVERIFY(map.m_root.load(Acquire) != source);
        for (int i = 1; i <= 256; ++i) QCOMPARE(map.get(i), (i - 1) % 3 == 0 ? 0 : i + 10);
    }
    map.getGC().flush();
}

void KisLocklessStackTest::testMigrationOverflowCompletion()
{
    using Map = ConcurrentMap<int, int>;
    Map map(512);
    {
        QSBR::RawPointerAccess access(map.getGC());
        for (int i = 1; i <= 256; ++i) map.assign(i, i + 10);
        auto *source = map.m_root.load(Acquire);
        Map::Details::beginTableMigrationToSize(map, source, 4);
        source->jobCoordinator.participate();
        QVERIFY(map.m_root.load(Acquire) != source);
        for (int i = 1; i <= 256; ++i) QCOMPARE(map.get(i), i + 10);
        int count = 0;
        for (Map::Iterator iter(map); iter.isValid(); iter.next()) ++count;
        QCOMPARE(count, 256);
    }
    map.getGC().flush();
}

void KisLocklessStackTest::testPartialMigrationTileHashDestruction()
{
    using Table = KisTileHashTableTraits2<HashTestTile>;
    KisSharedPtr<HashTestTile> held;
    {
        Table table(nullptr);
        for (int i = 1; i <= 128; ++i) {
            bool added;
            auto tile = table.getTileLazy(i, 1, added);
            QVERIFY(added);
            if (i == 1) held = tile;
        }
        auto &map = table.m_map;
        using Details = Table::LockFreeTileMap::Details;
        QSBR::RawPointerAccess access(map.getGC());
        auto *source = map.m_root.load(Acquire);
        Details::beginTableMigrationToSize(map, source, 4);
        auto *job = static_cast<Details::TableMigration *>(source->jobCoordinator.loadConsume());
        bool overflowed = false;
        for (quint64 start = 0; start <= source->sizeMask; start += Details::TableMigrationUnitSize) {
            if (!job->migrateRange(source, start)) { overflowed = true; break; }
        }
        QVERIFY(overflowed);
        QCOMPARE(table.numTiles(), 128);
        QCOMPARE(hashTilesAlive.load(), 128);
    }
    QCOMPARE(hashTilesAlive.load(), 1);
    QCOMPARE(held->detached, 1);
    held.clear();
    QCOMPARE(hashTilesAlive.load(), 0);
}

void KisLocklessStackTest::testPrepareRedirectedEmptyCell()
{
    using Map = ConcurrentMap<int, int>;
    Map map(8);
    QSBR::RawPointerAccess access(map.getGC());
    auto *source = map.m_root.load(Acquire);
    Map::Details::beginTableMigrationToSize(map, source, 8);
    auto *job = static_cast<Map::Details::TableMigration *>(source->jobCoordinator.loadConsume());
    QVERIFY(job->migrateRange(source, 0));

    // Claiming a previously unused hash can still encounter a redirected
    // value. Preparation must resolve it before a caller changes its payload.
    auto cell = map.insertOrFind(42);
    QVERIFY(map.m_root.load(Acquire) != source);
    QVERIFY(!map.migrationInProcess());
    QCOMPARE(cell.exchangeValue(43), 0);
    QCOMPARE(map.get(42), 43);
}

void KisLocklessStackTest::testInvalidMapCapacity()
{
    using Map = ConcurrentMap<int, int>;
    for (quint64 size : {quint64(0), quint64(2), quint64(3), quint64(1) << 63,
                         std::numeric_limits<quint64>::max()}) {
        QVERIFY_EXCEPTION_THROWN(Map{size}, std::bad_alloc);
    }
    Map valid(4);
    QSBR::RawPointerAccess access(valid.getGC());
    QCOMPARE(valid.assign(1, 42), 0);
    QCOMPARE(valid.get(1), 42);
}

void KisLocklessStackTest::testCoordinatorRetryWakeup()
{
    SimpleJobCoordinator coordinator;
    struct Job : SimpleJobCoordinator::Job {
        std::atomic<int> calls{0};
        std::atomic<bool> reject{false};
        void run() override {
            ++calls;
            if (reject.load()) throw std::bad_alloc();
        }
    } job;
    std::atomic<bool> rejected{false};
    coordinator.storeRelease(&job);
    std::thread waiter([&] {
        try { coordinator.participate(); }
        catch (const std::bad_alloc &) { rejected = true; }
    });
    QElapsedTimer deadline;
    deadline.start();
    while (job.calls.load() == 0 && deadline.elapsed() < 2000) QThread::msleep(1);
    const bool entered = job.calls.load() > 0;
    job.reject = true;
    coordinator.notifyRetry(); // Same job pointer, new attempt must wake it.
    deadline.restart();
    while (!rejected.load() && deadline.elapsed() < 2000) QThread::msleep(1);
    coordinator.end();
    waiter.join();
    QVERIFY(entered);
    QVERIFY(rejected.load());
    QCOMPARE(job.calls.load(), 2);
}

void KisLocklessStackTest::testPreparedKeySurvivesMigration()
{
    quint64 first = 11, second = 22;
    PreparedPointerMap map(8);
    auto key = map.prepareKey(1);
    QVERIFY(key.belongsTo(map));
    QVERIFY(!map.getGC().sanityRawPointerAccessLocked());
    auto *original = map.m_root.load(Acquire);
    {
        QSBR::RawPointerAccess access(map.getGC());
        QVERIFY(!map.get(1));
        QVERIFY(!PreparedPointerMap::Iterator(map).isValid());
        for (int i = 2; i <= 4096; ++i) map.assign(i, &first);
        QVERIFY(map.m_root.load(Acquire) != original);
        // The token does not retain an old table or require a migration worker.
    }
    map.getGC().flush();
    {
        QSBR::RawPointerAccess access(map.getGC());
        auto result = key.insertIfAbsentValue(&second);
        QVERIFY(result.accepted); QVERIFY(!result.previous);
        QCOMPARE(map.get(1), &second);
        QCOMPARE(map.erase(1), &second);
        for (int i = 4097; i <= 8192; ++i) map.assign(i, &first);
        result = key.exchangeValue(&second);
        QVERIFY(result.accepted); QVERIFY(!result.previous);
        QCOMPARE(map.get(1), &second);
    }
    key.reset();
    map.getGC().flush();
    QSBR::RawPointerAccess access(map.getGC());
    QCOMPARE(map.get(1), &second);
    QVERIFY(!map.preparedEntry(map.locateExistingCell(DefaultKeyTraits<int>::hash(1)).value));
}

void KisLocklessStackTest::testPreparedKeyLogicalOperations()
{
    quint64 first = 11, second = 22, third = 33;
    PreparedPointerMap map;
    auto account = std::make_shared<PreparedStorageAccount>();
    PreparedStorageAllocator<std::byte> allocator(account);
    {
        QSBR::RawPointerAccess access(map.getGC());
        map.assign(1, &first);
        auto beforePromotion = map.find(1);
        auto key = map.prepareKey(1, allocator);
        auto secondKey = map.prepareKey(1, allocator);
        QCOMPARE(account->allocations, size_t(1));
        QCOMPARE(beforePromotion.exchangeValue(&second), &first);
        QCOMPARE(map.get(1), &second);
        auto result = key.insertIfAbsentValue(&third);
        QVERIFY(result.accepted); QCOMPARE(result.previous, &second);
        QCOMPARE(map.erase(1), &second);
        QVERIFY(!PreparedPointerMap::Iterator(map).isValid());
        result = secondKey.insertIfAbsentValue(&third);
        QVERIFY(result.accepted); QVERIFY(!result.previous);
        auto beforeDemotion = map.find(1);
        key.reset();
        QVERIFY(secondKey.belongsTo(map));
        secondKey.reset();
        QCOMPARE(beforeDemotion.exchangeValue(&first), &third);
        QCOMPARE(map.get(1), &first);
        // Retired storage stays charged while the old mutator can refer to it.
        map.getGC().update();
        QVERIFY(account->liveBytes > 0);
    }
    map.getGC().flush();
    QCOMPARE(account->liveBytes, size_t(0));
    account->reject = true;
    bool rejected = false;
    try { auto key = map.prepareKey(2, allocator); }
    catch (const std::bad_alloc &) { rejected = true; }
    QVERIFY(rejected);
    QVERIFY(!map.getGC().sanityRawPointerAccessLocked());
    QSBR::RawPointerAccess access(map.getGC());
    QCOMPARE(map.get(1), &first);
    QVERIFY(!map.get(2));
}

void KisLocklessStackTest::testPreparedKeyDuringPartialMigration()
{
    quint64 first = 11, second = 22;
    PreparedPointerMap map(512);
    auto key = map.prepareKey(1);
    {
        QSBR::RawPointerAccess access(map.getGC());
        for (int i = 2; i <= 256; ++i) map.assign(i, &first);
        auto *source = map.m_root.load(Acquire);
        PreparedPointerMap::Details::beginTableMigrationToSize(map, source, 4);
        auto *job = static_cast<PreparedPointerMap::Details::TableMigration *>(source->jobCoordinator.loadConsume());
        bool overflowed = false;
        for (quint64 start = 0; start <= source->sizeMask; start += PreparedPointerMap::Details::TableMigrationUnitSize) {
            if (!job->migrateRange(source, start)) { overflowed = true; break; }
        }
        QVERIFY(overflowed);
        const auto result = key.exchangeValue(&second);
        QVERIFY(result.accepted); QVERIFY(!result.previous);
        QCOMPARE(map.get(1), &second);
        key.reset();
        QCOMPARE(map.get(1), &second);
        QCOMPARE(map.m_root.load(Acquire), source);
        QCOMPARE(job->m_workerStatus.load(Acquire), quint64(0));
        source->jobCoordinator.participate();
        QCOMPARE(map.get(1), &second);
    }
    map.getGC().flush();
}

void KisLocklessStackTest::testPreparedKeyCloseAndLifetime()
{
    quint64 value = 42;
    auto account = std::make_shared<PreparedStorageAccount>();
    std::weak_ptr<PreparedStorageAccount> heldAccount = account;
    PreparedPointerMap::PreparedKey late;
    {
        PreparedPointerMap map;
        late = map.prepareKey(1, PreparedStorageAllocator<std::byte>(account));
        {
            QSBR::RawPointerAccess access(map.getGC());
            QVERIFY(late.exchangeValue(&value).accepted);
        }
        account.reset();
        QVERIFY(!heldAccount.expired());
        map.closePreparedKeys();
        QVERIFY(!late.belongsTo(map));
        QVERIFY(!late.exchangeValue(nullptr).accepted);
        QVERIFY(!map.prepareKey(2));
        QSBR::RawPointerAccess access(map.getGC());
        QCOMPARE(map.get(1), &value);
    }
    // Both the allocation and its accounting owner outlive the dead table.
    QVERIFY(!heldAccount.expired());
    QVERIFY(heldAccount.lock()->liveBytes > 0);
    QVERIFY(!late.exchangeValue(&value).accepted);
    late.reset();
    QVERIFY(heldAccount.expired());
}

void KisLocklessStackTest::testPreparedKeyConcurrentRelease()
{
    quint64 first = 11, second = 22;
    PreparedPointerMap map;
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (int worker = 1; worker <= 4; ++worker) workers.emplace_back([&, worker] {
        for (int i = 0; i < 2000; ++i) {
            auto key = map.prepareKey(worker);
            {
                QSBR::RawPointerAccess access(map.getGC());
                map.assign(worker, &first);
                if (map.erase(worker) != &first) valid = false;
                auto installed = key.insertIfAbsentValue(&second);
                if (!installed.accepted || installed.previous || map.get(worker) != &second) valid = false;
            }
            key.reset();
        }
    });
    workers.emplace_back([&] {
        for (int i = 100; i < 10000; ++i) {
            QSBR::RawPointerAccess access(map.getGC());
            map.assign(i, &first);
        }
    });
    for (auto &worker : workers) worker.join();
    QVERIFY(valid.load());
    map.getGC().flush();

    std::vector<PreparedPointerMap::PreparedKey> closing;
    for (int i = 1; i <= 256; ++i) closing.push_back(map.prepareKey(i));
    std::atomic<bool> go{false};
    workers.clear();
    for (int worker = 0; worker < 4; ++worker) workers.emplace_back([&, worker] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (size_t i = worker; i < closing.size(); i += 4) {
            closing[i].exchangeValue(&second);
            closing[i].reset();
        }
    });
    go.store(true, std::memory_order_release);
    map.closePreparedKeys();
    for (auto &worker : workers) worker.join();
    map.getGC().flush();
    QVERIFY(!map.getGC().sanityRawPointerAccessLocked());
}

void KisLocklessStackTest::testPreparedKeySameKeyCompetition()
{
    quint64 winner = 11, loser = 22;
    PreparedPointerMap map(8);
    std::atomic<bool> valid{true};
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 4; ++worker) workers.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 0; i < 2000; ++i) {
            auto key = map.prepareKey(1);
            auto shared = map.prepareKey(1);
            {
                QSBR::RawPointerAccess access(map.getGC());
                const auto result = key.insertIfAbsentValue(&winner);
                if (!result.accepted || (result.previous && result.previous != &winner)) valid = false;
                key.reset();
                const auto kept = shared.insertIfAbsentValue(&loser);
                if (!kept.accepted || kept.previous != &winner || map.get(1) != &winner) valid = false;
            }
            shared.reset();
        }
    });
    workers.emplace_back([&] {
        while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int i = 2; i < 10000; ++i) {
            {
                QSBR::RawPointerAccess access(map.getGC());
                map.assign(i, &winner);
            }
            if (i % 100 == 0) map.getGC().update();
        }
    });
    go.store(true, std::memory_order_release);
    for (auto &worker : workers) worker.join();
    QVERIFY(valid.load());
    map.getGC().flush();
    QSBR::RawPointerAccess access(map.getGC());
    QCOMPARE(map.get(1), &winner);
    QVERIFY(!map.preparedEntry(map.locateExistingCell(DefaultKeyTraits<int>::hash(1)).value));
}

void KisLocklessStackTest::testPreparedKeyMapAddressReuse()
{
    quint64 oldValue = 11, newValue = 22;
    alignas(PreparedPointerMap) std::byte storage[sizeof(PreparedPointerMap)];
    auto *oldMap = new (storage) PreparedPointerMap;
    auto oldKey = oldMap->prepareKey(1);
    QVERIFY(oldKey.exchangeValue(&oldValue).accepted);
    oldMap->~PreparedPointerMap();
    auto *newMap = new (storage) PreparedPointerMap;
    auto newKey = newMap->prepareKey(1);
    const bool rejected = !oldKey.belongsTo(*newMap) && !oldKey.exchangeValue(&oldValue).accepted;
    const bool installed = newKey.exchangeValue(&newValue).accepted;
    bool retained;
    {
        QSBR::RawPointerAccess access(newMap->getGC());
        oldKey.reset();
        retained = newMap->get(1) == &newValue;
    }
    newKey.reset();
    newMap->~PreparedPointerMap();
    QVERIFY(rejected);
    QVERIFY(installed);
    QVERIFY(retained);
}

void KisLocklessStackTest::testTileHashPreparedKeyLifecycle()
{
    using Table = KisTileHashTableTraits2<HashTestTile>;
    Table::LockFreeTileMap::PreparedKey late;
    KisSharedPtr<HashTestTile> held;
    {
        Table table(nullptr);
        late = table.m_map.prepareKey(table.calculateHash(1, 1));
        QCOMPARE(table.numTiles(), 0);
        QVERIFY(!table.getExistingTile(1, 1));
        bool added = false;
        held = table.getTileLazy(1, 1, added);
        QVERIFY(added);
        QCOMPARE(table.numTiles(), 1);
        QCOMPARE(held->attached, 1);
        QVERIFY(table.deleteTile(1, 1));
        QCOMPARE(table.numTiles(), 0);
        for (int i = 2; i <= 128; ++i) table.getTileLazy(i, 1, added);
        held = table.getTileLazy(1, 1, added);
        QVERIFY(added);
        QCOMPARE(table.numTiles(), 128);
    }
    QVERIFY(!late.exchangeValue(nullptr).accepted);
    QCOMPARE(hashTilesAlive.load(), 1);
    QCOMPARE(held->detached, 1);
    late.reset();
    held.clear();
    QCOMPARE(hashTilesAlive.load(), 0);
}


namespace {
template<class Table>
void checkPreparedTileCandidates()
{
    QCOMPARE(hashTilesAlive.load(), 0);
    typename Table::PreparedTile late;
    KisSharedPtr<HashTestTile> survivor;
    // Reuse exactly the same address, so a pointer-only identity cannot pass.
    alignas(Table) unsigned char memory[sizeof(Table)];
    auto *table = new (memory) Table(nullptr);
    {
        auto candidate = table->prepareMissingTile(1, 1);
        QVERIFY(candidate); QCOMPARE(table->numTiles(), 0);
        QVERIFY(!table->getExistingTileForPreparedUpdate(1, 1));
        auto loser = table->prepareMissingTile(1, 1);
        bool added = false;
        // Unrelated growth may migrate the actual reserved key.
        for (int i = 2; i < 1024; ++i) table->getTileLazy(i, 1, added);
        survivor = table->installPreparedTile(candidate, added);
        QVERIFY(survivor); QVERIFY(added); QCOMPARE(survivor->attached, 1);
        QVERIFY(!candidate); QVERIFY(!table->installPreparedTile(candidate, added)); QVERIFY(!added);
        const auto winner = table->installPreparedTile(loser, added);
        QCOMPARE(winner, survivor); QVERIFY(!added); QVERIFY(!loser);
        QVERIFY(!table->prepareMissingTile(1, 1));
        QCOMPARE(table->numTiles(), 1023);
        auto cancelled = table->prepareMissingTile(1025, 1);
        QVERIFY(cancelled); QCOMPARE(table->numTiles(), 1023);
        late = table->prepareMissingTile(1026, 1);
    }
    table->~Table();
    QCOMPARE(survivor->detached, 1);
    table = new (memory) Table(nullptr);
    auto newCandidate = table->prepareMissingTile(1026, 1);
    bool added = true;
    QVERIFY(!table->installPreparedTile(late, added)); QVERIFY(!added);
    auto installed = table->installPreparedTile(newCandidate, added);
    QVERIFY(installed); QVERIFY(added);
    table->~Table();
    installed.clear(); newCandidate = {}; late = {}; survivor.clear();
    QCOMPARE(hashTilesAlive.load(), 0);
}
}

void KisLocklessStackTest::testPreparedTileCandidatesLockFree()
{
    checkPreparedTileCandidates<KisTileHashTableTraits2<HashTestTile>>();
}

void KisLocklessStackTest::testPreparedTileCandidatesChained()
{
    checkPreparedTileCandidates<KisTileHashTableTraits<HashTestTile>>();
}

void KisLocklessStackTest::testOperations()
{
    KisLocklessStack<int> stack;


    for(qint32 i = 0; i < 1024; i++) {
        stack.push(i);
    }

    QCOMPARE(stack.size(), 1024);

    for(qint32 i = 1023; i >= 0; i--) {
        int value;

        bool result = stack.pop(value);
        QVERIFY(result);

        QCOMPARE(value, i);
    }

    QVERIFY(stack.isEmpty());

}

/************ BENCHMARKING INFRASTRUCTURE ************************/

#define NUM_TYPES 2

// high-concurrency
#define NUM_CYCLES 500000
#define NUM_THREADS 10

// relaxed
//#define NUM_CYCLES 100
//#define NUM_THREADS 2

// single-threaded
//#define NUM_CYCLES 10000000
//#define NUM_THREADS 1


class KisAbstractIntStack
{
public:
    virtual ~KisAbstractIntStack() {}
    virtual void push(int value) = 0;
    virtual int pop() = 0;
    virtual bool isEmpty() = 0;
    virtual void clear() = 0;
};

class KisTestingLocklessStack : public KisAbstractIntStack
{
public:
    void push(int value) override {
        m_stack.push(value);
    }

    int pop() override {
        int value  = 0;

        bool result = m_stack.pop(value);
        Q_ASSERT(result);
        Q_UNUSED(result); // for release build

        return value;
    }

    bool isEmpty() override {
        return m_stack.isEmpty();
    }

    void clear() override {
        m_stack.clear();
    }

private:
    KisLocklessStack<int> m_stack;
};

class KisTestingLegacyStack : public KisAbstractIntStack
{
public:
    void push(int value) override {
        m_mutex.lock();
        m_stack.push(value);
        m_mutex.unlock();
    }

    int pop() override {
        m_mutex.lock();
        int result = m_stack.pop();
        m_mutex.unlock();

        return result;
    }

    bool isEmpty() override {
        m_mutex.lock();
        bool result = m_stack.isEmpty();
        m_mutex.unlock();

        return result;
    }

    void clear() override {
        m_mutex.lock();
        m_stack.clear();
        m_mutex.unlock();
    }

private:
    QStack<int> m_stack;
    QMutex m_mutex;
};


class KisStressJob : public QRunnable
{
public:
    KisStressJob(KisAbstractIntStack &stack, qint32 startValue)
        : m_stack(stack), m_startValue(startValue)
    {
        m_pushSum = 0;
        m_popSum = 0;
    }

    void run() override {
        for(qint32 i = 0; i < NUM_CYCLES; i++) {
            qint32 type = i % NUM_TYPES;
            int newValue;

            switch(type) {
            case 0:
                newValue = m_startValue + i;

                m_pushSum += newValue;
                m_stack.push(newValue);
                break;
            case 1:
                m_popSum += m_stack.pop();
                break;
            }
        }
    }

    qint64 pushSum() {
        return m_pushSum;
    }

    qint64 popSum() {
        return m_popSum;
    }

private:
    KisAbstractIntStack &m_stack;
    qint32 m_startValue;
    qint64 m_pushSum;
    qint64 m_popSum;
};

void KisLocklessStackTest::runStressTest(KisAbstractIntStack &stack)
{
    QList<KisStressJob*> jobsList;
    KisStressJob *job;

    for(qint32 i = 0; i < NUM_THREADS; i++) {
        job = new KisStressJob(stack, 1);
        job->setAutoDelete(false);
        jobsList.append(job);
    }

    QThreadPool pool;
    pool.setMaxThreadCount(NUM_THREADS);

    QBENCHMARK {
        Q_FOREACH (job, jobsList) {
            pool.start(job);
        }

        pool.waitForDone();
    }

    QVERIFY(stack.isEmpty());

    qint64 totalSum = 0;

    for(qint32 i = 0; i < NUM_THREADS; i++) {
        KisStressJob *job = jobsList.takeLast();

        totalSum += job->pushSum();
        totalSum -= job->popSum();

        dbgKrita << ppVar(totalSum);

        delete job;
    }

    QCOMPARE(totalSum, (long long) 0);
}

void KisLocklessStackTest::stressTestLockless()
{
    KisTestingLocklessStack stack;
    runStressTest(stack);
}

void KisLocklessStackTest::stressTestQStack()
{
    KisTestingLegacyStack stack;
    runStressTest(stack);
}

class KisStressClearJob : public QRunnable
{
public:
    KisStressClearJob(KisLocklessStack<int> &stack, qint32 startValue)
        : m_stack(stack), m_startValue(startValue)
    {
    }

    void run() override {
        for(qint32 i = 0; i < NUM_CYCLES; i++) {
            qint32 type = i % 4;
            int newValue;

            switch(type) {
            case 0:
            case 1:
                newValue = m_startValue + i;
                m_stack.push(newValue);
                break;
            case 2:
                int tmp;
                m_stack.pop(tmp);
                break;
            case 3:
                m_stack.clear();
                break;
            }
        }
    }

private:
    KisLocklessStack<int> &m_stack;
    qint32 m_startValue;
};

void KisLocklessStackTest::stressTestClear()
{
    KisLocklessStack<int> stack;
    KisStressClearJob *job;

    QThreadPool pool;
    pool.setMaxThreadCount(NUM_THREADS);

    for(qint32 i = 0; i < NUM_THREADS; i++) {
        job = new KisStressClearJob(stack, 1);
        pool.start(job);
    }
    pool.waitForDone();

    stack.clear();
    QVERIFY(stack.isEmpty());
}

class KisStressBulkPopJob : public QRunnable
{
public:
    KisStressBulkPopJob(KisLocklessStack<int> &stack, qint64 &removedCheckSum)
        : m_stack(stack), m_removedCheckSum(removedCheckSum)
    {
    }

    void run() override {
        int value = 0;
        while (m_stack.pop(value)) {
            m_removedCheckSum += value;
        }
    }

private:
    KisLocklessStack<int> &m_stack;
    qint64 &m_removedCheckSum;
};

void KisLocklessStackTest::stressTestBulkPop()
{
    KisLocklessStack<int> stack;

#ifdef LIMIT_LONG_TESTS
    const int numThreads = 3;
    const int numObjects = 10000000;
#else
    const int numThreads = 3;
    const int numObjects = 10000000;
#endif

    QThreadPool pool;
    pool.setMaxThreadCount(numThreads);

    qint64 expectedSum = 0;
    for (int i = 0; i < numObjects; i++) {
        const int value = i % 3 + 1;
        expectedSum += value;
        stack.push(value);
    }

    QVector<qint64> partialSums(numThreads);

    for(qint32 i = 0; i < numThreads; i++) {
        KisStressBulkPopJob *job = new KisStressBulkPopJob(stack, partialSums[i]);
        pool.start(job);
    }
    pool.waitForDone();

    QVERIFY(stack.isEmpty());

    const qint64 realSum = std::accumulate(partialSums.begin(), partialSums.end(), 0);
    QCOMPARE(realSum, expectedSum);
}

SIMPLE_TEST_MAIN(KisLocklessStackTest)
