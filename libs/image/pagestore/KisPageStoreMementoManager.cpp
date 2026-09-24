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
#include <atomic>

namespace {

std::atomic<quint64> s_nextHistoryCapability{1};

struct ActiveHistoryTransaction
{
    KisPageTransaction transaction;
    KisRetainedImageEpochSnapshot before;
    KisImageEpochSnapshotToken orphanedAfter;
};

quint64 allocateHistoryCapability(QString *error, const QString &failure)
{
    const quint64 capability = KisPageStoreDetail::allocateMonotonicId<quint64>(&s_nextHistoryCapability);
    if (!capability) KisPageStoreDetail::setError(error, failure);
    return capability;
}

bool releaseActiveHistory(KisPageStore *store, ActiveHistoryTransaction &record)
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

struct HistoryRecord
{
    KisRetainedImageEpochSnapshot before;
    KisRetainedImageEpochSnapshot after;
    QVector<KisPageKey> changedPages;
    bool requiresFullPageRestore = false;
};

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
    auto find(const KisPageStoreHistoryTransaction &transaction, QString *error)
    {
        auto it = active.find(transaction.capability);
        if (!store || !transaction.isValid() || it == active.end() ||
            !(it->transaction == transaction.transaction)) {
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
    KisPageStore *store = nullptr;
    QHash<quint64, ActiveHistoryTransaction> active;
    QHash<quint64, HistoryRecord> mementos;
};

KisPageStoreMementoManager::KisPageStoreMementoManager()
    : d(new Private)
{
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
    d->active.insert(capability, {transaction, before, {}});
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
        d->store->preparedPages(it->transaction);
    if (!prepared.isValid()) {
        if (!d->store->abort(it->transaction)) {
            KisPageStoreDetail::setError(error, QStringLiteral(
                "PageStore no-op history transaction abort failed"));
            return {};
        }
        const KisRetainedImageEpochSnapshot retained = it->before;
        d->active.erase(it);
        d->mementos.insert(capability,
                            {retained, retained, {}, false});
        KisPageStoreDetail::setError(error, {});
        return {capability};
    }
    KisRetainedImageEpochSnapshot after;
    const KisImageEpochCommitTicket committed =
        d->store->commit(it->transaction, prepared, &after);
    if (!committed.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore history transaction commit failed"));
        return {};
    }
    it->transaction = {};
    if (!after.isValid() || !(after.snapshot.epoch == committed.epoch)) {
        it->orphanedAfter = after.token;
        KisPageStoreDetail::setError(error, QStringLiteral(
            "PageStore after-root retention failed after commit"));
        return {};
    }

    QVector<KisPageKey> changedPages;
    changedPages.reserve(prepared.proofs.size() + prepared.removedPages.size());
    for (const KisPreparedPageProof &proof : prepared.proofs) {
        changedPages.append(proof.authority.version.key);
    }
    changedPages += prepared.removedPages;
    const bool requiresFullPageRestore = std::any_of(
        prepared.surfaceChanges.constBegin(),
        prepared.surfaceChanges.constEnd(),
        [](const KisSurfaceEpochChange &change) {
            return change.before.format.defaultPixel !=
                   change.after.format.defaultPixel;
        });

    const KisRetainedImageEpochSnapshot before = it->before;
    d->active.erase(it);
    d->mementos.insert(capability,
                        {before, after, changedPages, requiresFullPageRestore});
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
    if (!releaseActiveHistory(d->store, it.value())) {
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
    if (!it->before.token.isValid() || !it->after.token.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("PageStore memento is being purged"));
        return {};
    }
    const KisRetainedImageEpochSnapshot &target =
        before ? it->before : it->after;
    const KisImageEpochCommitTicket restored = it->requiresFullPageRestore
        ? d->store->restoreRetainedEpoch(target)
        : d->store->restoreRetainedEpochDelta(target, it->changedPages);
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
    if (!releaseHistoryRecord(it.value(), releaseSnapshot)) {
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
        if (!releaseActiveHistory(d->store, it.value())) {
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
        if (!releaseHistoryRecord(it.value(), releaseSnapshot)) {
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
