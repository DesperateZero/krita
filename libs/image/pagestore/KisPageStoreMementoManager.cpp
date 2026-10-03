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

namespace {

std::atomic<quint64> s_nextHistoryCapability{1};

struct HistoryRecord
{
    explicit HistoryRecord(KisMutationStorageAllocator<KisPageKey> storage) : changedPages(storage) {}
    KisPageTransaction transaction;
    KisRetainedImageEpochSnapshot before;
    KisImageEpochSnapshotToken orphanedAfter;
    KisRetainedImageEpochSnapshot after;
    KisPageKeyStorage changedPages;
    bool requiresFullPageRestore = false;
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
    if (record.orphanedAfter.isValid()) {
        if (!store->releaseSnapshot(record.orphanedAfter))
            return false;
        record.orphanedAfter = {};
    }
    return true;
}


template<typename ReleaseSnapshot>
bool releaseHistoryRecord(HistoryRecord &record, ReleaseSnapshot releaseSnapshot)
{
    const bool shared = record.before.token.isValid() && record.before.token == record.after.token;
    if (record.before.token.isValid()) {
        if (!releaseSnapshot(record.before.token, record))
            return false;
        record.before.token = {};
        if (shared)
            record.after.token = {};
    }
    if (record.after.token.isValid()) {
        if (!releaseSnapshot(record.after.token, record))
            return false;
        record.after.token = {};
    }
    return true;
}

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
    try { prepared.try_emplace(capability, d->active.get_allocator()); }
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
    const KisPreparedPageSet prepared =
        d->store->preparedPages(it->second.transaction);
    if (!prepared.isValid()) {
        if (!d->store->abort(it->second.transaction)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "PageStore no-op history transaction abort failed"));
            return {};
        }
        it->second.transaction = {};
        it->second.after = it->second.before;
        auto record = d->active.extract(it);
        record.key() = capability;
        d->mementos.insert(std::move(record));
        KisPageStoreDetail::setError(error, {});
        return {capability};
    }
    auto &record = it->second;
    try {
        record.changedPages.clear();
        record.changedPages.reserve(size_t(prepared.proofs.size()) + size_t(prepared.removedPages.size()));
        for (const auto &proof : prepared.proofs) record.changedPages.push_back(proof.authority.version.key);
        record.changedPages.insert(record.changedPages.end(), prepared.removedPages.begin(), prepared.removedPages.end());
    } catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore history delta storage is unavailable"));
        return {};
    }
    record.requiresFullPageRestore = std::any_of(
        prepared.surfaceChanges.constBegin(), prepared.surfaceChanges.constEnd(),
        [](const KisSurfaceEpochChange &change) {
            return change.before.format.defaultPixel != change.after.format.defaultPixel;
        });
    KisRetainedImageEpochSnapshot after;
    const KisImageEpochCommitTicket committed =
        d->store->commit(it->second.transaction, prepared, &after);
    if (!committed.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore history transaction commit failed"));
        return {};
    }
    it->second.transaction = {};
    if (!after.isValid() || !(after.snapshot.epoch == committed.epoch)) {
        it->second.orphanedAfter = after.token;
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore after-root retention failed after commit"));
        return {};
    }

    record.after = after;
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
    QString *error)
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
    const KisImageEpochCommitTicket restored = it->second.requiresFullPageRestore
        ? d->store->restoreRetainedEpoch(target)
        : d->store->restoreRetainedEpochDelta(target, it->second.changedPages);
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
    const auto releaseSnapshot = [store = d->store](KisImageEpochSnapshotToken token,
                                                     const HistoryRecord &record) {
        return record.requiresFullPageRestore
            ? store->releaseSnapshot(token)
            : store->releaseSnapshotDelta(token, record.changedPages);
    };
    if (!releaseHistoryRecord(it->second, releaseSnapshot)) {
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
    const auto releaseSnapshot = [store = d->store](KisImageEpochSnapshotToken token,
                                                     const HistoryRecord &record) {
        return record.requiresFullPageRestore
            ? store->releaseSnapshot(token)
            : store->releaseSnapshotDelta(token, record.changedPages);
    };
    for (auto it = d->mementos.begin(); it != d->mementos.end();) {
        if (!releaseHistoryRecord(it->second, releaseSnapshot)) {
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
