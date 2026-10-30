/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStoreMementoManager.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>
#include <map>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

#include "tiles3/kis_tile_data.h"
#include "tiles3/kis_memento.h"

namespace {

struct alignas(std::max_align_t) MementoStorageHeader
{
    bool processStorage = false;
};

static_assert(alignof(KisMemento) <= alignof(std::max_align_t));

void *allocateMementoStorage(std::size_t bytes, bool processStorage)
{
    if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(MementoStorageHeader))
        throw std::bad_alloc();
    const std::size_t total = sizeof(MementoStorageHeader) + bytes;
    void *raw = processStorage
        ? kisAllocatePageProcessStorage(total, alignof(std::max_align_t))
        : ::operator new(total);
    ::new (raw) MementoStorageHeader{processStorage};
    return static_cast<std::byte *>(raw) + sizeof(MementoStorageHeader);
}

MementoStorageHeader *mementoStorageHeader(void *data) noexcept
{
    return reinterpret_cast<MementoStorageHeader *>(
        static_cast<std::byte *>(data) - sizeof(MementoStorageHeader));
}

std::atomic<quint64> s_nextHistoryCapability{1};

struct HistoryRecord
{
    KisPageTransaction transaction;
    KisRetainedImageEpochSnapshot before;
    KisRetainedImageEpochSnapshot after;
};

quint64 allocateHistoryCapability(QString *error, const QString &failure)
{
    const quint64 capability = KisPageStoreDetail::allocateMonotonicId<quint64>(&s_nextHistoryCapability);
    if (!capability) KisPageStoreDetail::setError(error, failure);
    return capability;
}

bool releaseActiveHistory(KisPageStore *store, HistoryRecord &record)
{
    if (record.transaction.isValid()) {
        if (!store->abort(record.transaction)) return false;
        record.transaction = {};
    }
    if (record.before.token.isValid()) {
        if (!store->releaseSnapshot(record.before.token))
            return false;
        record.before.token = {};
    }
    return true;
}


bool releaseHistoryRecord(KisPageStore *store, HistoryRecord &record)
{
    const bool shared = record.before.token.isValid() && record.before.token == record.after.token;
    if (record.before.token.isValid()) {
        if (!store->releaseSnapshot(record.before.token))
            return false;
        record.before.token = {};
        if (shared)
            record.after.token = {};
    }
    if (record.after.token.isValid()) {
        if (!store->releaseSnapshot(record.after.token))
            return false;
        record.after.token = {};
    }
    return true;
}

}

void *KisMemento::operator new(std::size_t bytes)
{
    return allocateMementoStorage(bytes, false);
}

void KisMemento::operator delete(void *data) noexcept
{
    if (!data) return;
    auto *header = mementoStorageHeader(data);
    if (header->processStorage)
        kisFreePageProcessStorage(header, 0, alignof(std::max_align_t));
    else
        ::operator delete(header);
}

void KisMemento::operator delete(void *data, std::size_t) noexcept
{
    KisMemento::operator delete(data);
}

KisMemento *KisMemento::createPageStoreMemento()
{
    void *storage = allocateMementoStorage(sizeof(KisMemento), true);
    return ::new (storage) KisMemento(nullptr, true);
}

KisMemento::~KisMemento()
{
    if (m_pageStoreStorage) {
        kisFreePageProcessStorage(m_oldDefaultPixel, 0, alignof(quint8));
        kisFreePageProcessStorage(m_newDefaultPixel, 0, alignof(quint8));
    } else {
        delete[] m_oldDefaultPixel;
        delete[] m_newDefaultPixel;
    }
}

void KisMemento::saveDefaultPixel(quint8 *&stored, const quint8 *pixel, quint32 pixelSize)
{
    // Prepare before releasing the previous value, including an aliased input
    // or a commit retry. Both undo endpoints use the same storage policy.
    quint8 *replacement = m_pageStoreStorage
        ? static_cast<quint8 *>(kisAllocatePageProcessStorage(pixelSize, alignof(quint8)))
        : new quint8[pixelSize];
    memcpy(replacement, pixel, pixelSize);
    if (m_pageStoreStorage) kisFreePageProcessStorage(stored, 0, alignof(quint8));
    else delete[] stored;
    stored = replacement;
}

void KisMemento::saveOldDefaultPixel(const quint8 *pixel, quint32 pixelSize)
{
    saveDefaultPixel(m_oldDefaultPixel, pixel, pixelSize);
}

void KisMemento::saveNewDefaultPixel(const quint8 *pixel, quint32 pixelSize)
{
    saveDefaultPixel(m_newDefaultPixel, pixel, pixelSize);
}

class KisPageStoreMementoManager::Private
{
public:
    using Storage = KisMutationStorageAllocator<Private>;
    using Records = std::map<quint64, HistoryRecord, std::less<quint64>,
        KisMutationStorageAllocator<std::pair<const quint64, HistoryRecord>>>;
    explicit Private(const Storage &allocator) : storage(allocator) {}
    auto find(const KisPageStoreHistoryTransaction &transaction, QString *error)
    {
        auto it = active.find(transaction.capability);
        if (!store || !transaction.isValid() || it == active.end() ||
            !(it->second.transaction == transaction.transaction)) {
            KisPageStoreDetail::setError(error, QStringLiteral("PageStore history transaction capability is invalid"));
            return active.end();
        }
        return it;
    }
    auto find(const KisPageStoreMemento &memento, QString *error)
    {
        auto it = mementos.find(memento.capability);
        if (!store || !memento.isValid() || it == mementos.end()) {
            KisPageStoreDetail::setError(error, QStringLiteral("PageStore memento is invalid"));
            return mementos.end();
        }
        return it;
    }

    mutable QMutex mutex;
    Storage storage;
    KisPageStore *store = nullptr;
    Records active;
    Records mementos;
};

KisPageStoreMementoManager::KisPageStoreMementoManager(
    const KisMutationStorageAllocator<KisPageStoreMementoManager> &storage)
{
    Private::Storage allocator(storage);
    auto *raw = allocator.allocate(1);
    try { std::allocator_traits<Private::Storage>::construct(allocator, raw, allocator); }
    catch (...) { allocator.deallocate(raw, 1); throw; }
    d.reset(raw);
}

void KisPageStoreMementoManager::PrivateReleaser::cleanup(Private *owner)
{
    if (!owner) return;
    auto storage = owner->storage;
    std::destroy_at(owner);
    storage.deallocate(owner, 1);
}

KisPageStoreMementoManager::~KisPageStoreMementoManager()
{
    close();
}

bool KisPageStoreMementoManager::configure(KisPageStore *store,
                                           QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (d->store || !store || !store->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore memento manager configuration is invalid"));
        return false;
    }
    const auto storage = store->storageAllocator();
    d->active = Private::Records(storage);
    d->mementos = Private::Records(storage);
    d->store = store;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageStoreHistoryTransaction KisPageStoreMementoManager::begin(
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->store || !d->store->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore memento manager is not operational"));
        return {};
    }
    const quint64 capability = allocateHistoryCapability(error, QStringLiteral("PageStore history capability allocation failed"));
    if (!capability) return {};
    Private::Records prepared(d->active.get_allocator());
    try { prepared.try_emplace(capability); }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore history record storage is unavailable"));
        return {};
    }
    const KisRetainedImageEpochSnapshot before =
        d->store->captureRetainedEpochRoot();
    if (!before.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore before-root retention failed"));
        return {};
    }
    const KisPageTransaction transaction =
        d->store->beginTransaction(before.snapshot.epoch);
    if (!transaction.isValid()) {
        d->store->releaseSnapshot(before.token);
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore history transaction allocation failed"));
        return {};
    }
    auto &record = prepared.begin()->second;
    record.transaction = transaction;
    record.before = before;
    d->active.insert(prepared.extract(prepared.begin()));
    KisPageStoreDetail::setError(error, {});
    return {capability, transaction};
}

KisPageStoreMemento KisPageStoreMementoManager::commit(
    const KisPageStoreHistoryTransaction &transaction,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    auto it = d->find(transaction, error);
    if (it == d->active.end()) return {};
    const quint64 capability = allocateHistoryCapability(error, QStringLiteral("PageStore memento capability allocation failed"));
    if (!capability) return {};
    auto &record = it->second;
    KisRetainedImageEpochSnapshot after;
    if (!d->store->commitPreparedTransaction(record.transaction, &after)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore history transaction commit failed"));
        return {};
    }
    it->second.transaction = {};
    // A commit requesting retention admits the complete output before root
    // installation. Acceptance cannot leave this history capability orphaned.
    // A successful empty transaction was aborted and shares the before token.
    record.after = after.isValid() ? after : record.before;
    auto accepted = d->active.extract(it);
    accepted.key() = capability;
    d->mementos.insert(std::move(accepted));
    KisPageStoreDetail::setError(error, {});
    return {capability};
}

bool KisPageStoreMementoManager::abort(
    const KisPageStoreHistoryTransaction &transaction,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    auto it = d->find(transaction, error);
    if (it == d->active.end()) return false;
    if (!releaseActiveHistory(d->store, it->second)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore history transaction cleanup failed"));
        return false;
    }
    d->active.erase(it);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisImageEpochCommitTicket KisPageStoreMementoManager::rollback(
    const KisPageStoreMemento &memento,
    QString *error)
{
    return restore(memento, true, error);
}

KisImageEpochCommitTicket KisPageStoreMementoManager::rollforward(
    const KisPageStoreMemento &memento,
    QString *error)
{
    return restore(memento, false, error);
}

KisImageEpochCommitTicket KisPageStoreMementoManager::restore(
    const KisPageStoreMemento &memento,
    bool before,
    QString *error,
    const std::function<bool(const KisImageEpochRootSnapshot &, const KisPageRestorationChanges &)> &prepareAdapter,
    const std::function<void(const KisPageRestorationChanges &)> &completeAdapter)
{
    QMutexLocker locker(&d->mutex);
    auto it = d->find(memento, error);
    if (it == d->mementos.end()) return {};
    if (!it->second.before.token.isValid() || !it->second.after.token.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore memento is being purged"));
        return {};
    }
    const KisRetainedImageEpochSnapshot &target =
        before ? it->second.before : it->second.after;
    const KisImageEpochCommitTicket restored = d->store->restoreRetainedEpoch(target, prepareAdapter, completeAdapter);
    if (!restored.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore memento restore failed"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return restored;
}

bool KisPageStoreMementoManager::purge(const KisPageStoreMemento &memento,
                                       QString *error)
{
    QMutexLocker locker(&d->mutex);
    auto it = d->find(memento, error);
    if (it == d->mementos.end()) return false;
    if (!releaseHistoryRecord(d->store, it->second)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore memento retention release failed"));
        return false;
    }
    d->mementos.erase(it);
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStoreMementoManager::close(QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->store) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    for (auto it = d->active.begin(); it != d->active.end();) {
        if (!releaseActiveHistory(d->store, it->second)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "PageStore memento manager transaction drain failed"));
            return false;
        }
        it = d->active.erase(it);
    }
    for (auto it = d->mementos.begin(); it != d->mementos.end();) {
        if (!releaseHistoryRecord(d->store, it->second)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "PageStore memento manager retention drain failed"));
            return false;
        }
        it = d->mementos.erase(it);
    }
    d->store = nullptr;
    KisPageStoreDetail::setError(error, {});
    return true;
}
