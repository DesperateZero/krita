/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStoreCpuAccessSession.h"
#include "KisPageStoreIteratorReadScope_p.h"

#include <QHash>

#include <optional>
#include <utility>
#include <vector>

namespace {

struct WriteRecord
{
    KisPageStoreReadPage base;
    std::optional<KisWriteLease> writeLease;
    KisCpuWriteGuard native;
};

}

class KisPageStoreCpuAccessSession::Private
{
public:
    void releaseReadPins()
    {
        cursorRecord.base.reset();
        cursorKey = {};
        reads.clear();
        readIndices.clear();
    }

    void releaseAllReads()
    {
        releaseReadPins();
        capturedReadView = {};
    }

    bool acquireBase(WriteRecord &record, const KisPageKey &key, QString *error)
    {
        if (record.base.data()) return true;
        // Preserve the control writer's before-image route. Speculatively
        // building native bindings here adds work to non-native producers.
        record.base = KisPageStoreReadPage(store, capturedReadView, key, error, priority, nativeWrites);
        return record.base.data();
    }

    void releaseCursorWrite()
    {
        if (nativeWrites) {
            cursorRecord.base.reset();
            cursorRecord.native = {};
            cursorKey = {};
            return;
        }
        auto it = writeIndices.constFind(cursorKey);
        if (it != writeIndices.constEnd()) {
            writes[it.value()].base.reset();
            // Generic leases protect unpublished bytes until finish. Do not
            // silently publish/cancel them just to evict a pointer cache.
        }
        cursorKey = {};
    }

    void cancelAllWrites()
    {
        if (!store) return;
        releaseCursorWrite();
        for (WriteRecord &record : writes) {
            record.base.reset();
            if (record.writeLease && record.writeLease->isValid()) store->cancel(std::move(*record.writeLease));
        }
        writes.clear();
        writeIndices.clear();
        cursorKey = {};
        if (nativeWrites) mutation.cancel();
        mutation = {};
        capturedReadView = {};
    }

    KisPageStore *store = nullptr;
    KisSurfaceId surface;
    KisCapturedReadView capturedReadView;
    KisPageTransaction transaction;
    KisPageMutationSession mutation;
    bool nativeWrites = false;
    KisPageWriteMode writeMode = KisPageWriteMode::PreserveContents;
    KisPagePriority priority = KisPagePriority::Normal;
    bool writable = false;
    bool active = false;
    KisCpuAccessLifetime lifetime = KisCpuAccessLifetime::OperationSpans;
    KisPageKey cursorKey;
    // Cursor reads and native writes use the same inline slot in disjoint
    // session modes; neither needs a per-page heap record or index.
    WriteRecord cursorRecord;
    // Guards are inline in the scope's contiguous cache, not one shared/heap
    // lease object per page. Returned byte spans survive vector relocation.
    std::vector<KisPageStoreReadPage> reads;
    QHash<KisPageKey, size_t> readIndices;
    std::vector<WriteRecord> writes;
    QHash<KisPageKey, size_t> writeIndices;
};

KisPageStoreCpuAccessSession::KisPageStoreCpuAccessSession()
    : d(new Private)
{
}

KisPageStoreCpuAccessSession::KisPageStoreCpuAccessSession(KisCpuAccessLifetime lifetime)
    : d(new Private)
{
    d->lifetime = lifetime;
}

KisPageStoreCpuAccessSession::~KisPageStoreCpuAccessSession()
{
    cancel();
}

bool KisPageStoreCpuAccessSession::beginRead(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageReadView &view,
    KisPagePriority priority,
    QString *error)
{
    const KisPageKey probeKey = view.kind == KisPageReadViewKind::ExactVersion
        ? view.exactVersion.key : KisPageKey{surface, {0, 0}};
    if (d->active || !store ||
        !surface.isValid() || !(probeKey.surface == surface) || !view.isValidFor(probeKey)) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU read session configuration is invalid"));
        return false;
    }
    // One capture fixes both the base root and the sealed overlay. Never
    // resolve a mutable transaction again on a later page/cache miss.
    auto captured = store->captureReadView(view, error);
    if (!captured.isValid()) return false;
    d->store = store;
    d->surface = surface;
    d->capturedReadView = std::move(captured);
    d->priority = priority;
    d->writable = false;
    d->active = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStoreCpuAccessSession::beginWrite(
    KisPageStore *store,
    KisSurfaceId surface,
    const KisPageTransaction &transaction,
    KisPageWriteMode mode,
    KisPagePriority priority,
    QString *error)
{
    if (d->active || !store || !store->isOperational() ||
        !surface.isValid() || !transaction.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU write session configuration is invalid"));
        return false;
    }
    KisPageReadView base;
    base.kind = KisPageReadViewKind::TransactionBaseEpoch;
    base.transaction = transaction.id;
    auto captured = store->captureReadView(base, error);
    if (!captured.isValid() || !(captured.epoch() == transaction.baseEpoch)) return false;
    auto mutation = store->beginMutation(transaction);
    d->store = store;
    d->surface = surface;
    d->transaction = transaction;
    d->capturedReadView = std::move(captured);
    d->nativeWrites = mutation.isActive();
    d->mutation = std::move(mutation);
    d->writeMode = mode;
    d->priority = priority;
    d->writable = true;
    d->active = true;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStoreCpuAccessSession::isActive() const
{
    return d->active;
}

bool KisPageStoreCpuAccessSession::resolveReadSurfaceState(KisSurfaceEpochState *state) const
{
    if (state) *state = {};
    if (!d->active || d->writable || !state || !d->store) return false;
    return d->capturedReadView.resolveSurfaceState(d->surface, state);
}

KisCpuPageReadSpan KisPageStoreCpuAccessSession::readPage(
    qint32 column,
    qint32 row,
    QString *error)
{
    if (!d->active || d->writable || !d->store) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU read session is not active"));
        return {};
    }
    const KisPageKey key{d->surface, {column, row}};
    if (d->lifetime == KisCpuAccessLifetime::CursorPage) {
        if (!(d->cursorKey == key) || !d->cursorRecord.base.data()) {
            d->cursorRecord.base.reset();
            d->cursorKey = {};
            d->cursorRecord.base = KisPageStoreReadPage(
                d->store, d->capturedReadView, key, error, d->priority);
            if (!d->cursorRecord.base.data()) return {};
            d->cursorKey = key;
        }
        const auto &record = d->cursorRecord.base;
        KisPageStoreDetail::setError(error, {});
        return {record.version(), record.data(), record.rowStride(), record.byteSize()};
    }
    auto found = d->readIndices.constFind(key);
    const size_t index = found == d->readIndices.constEnd() ? d->reads.size() : found.value();
    if (index == d->reads.size()) {
        KisPageStoreReadPage page(d->store, d->capturedReadView, key, error, d->priority);
        if (!page.data()) return {};
        d->reads.emplace_back(std::move(page));
        d->readIndices.insert(key, index);
    }
    const auto &record = d->reads[index];
    KisPageStoreDetail::setError(error, {});
    return {record.version(), record.data(), record.rowStride(), record.byteSize()};
}

KisCpuPageWriteSpan KisPageStoreCpuAccessSession::writePage(
    qint32 column,
    qint32 row,
    QString *error)
{
    if (!d->active || !d->writable || !d->store) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU write session is not active"));
        return {};
    }
    const KisPageKey key{d->surface, {column, row}};
    if (d->lifetime == KisCpuAccessLifetime::CursorPage && !(d->cursorKey == key))
        d->releaseCursorWrite();
    bool nativeCursor = d->nativeWrites && d->lifetime == KisCpuAccessLifetime::CursorPage;
    size_t index = d->writes.size();
    if (!nativeCursor) {
        const auto found = d->writeIndices.constFind(key);
        if (found != d->writeIndices.constEnd()) index = found.value();
    }
    const bool hasRecord = nativeCursor ? d->cursorRecord.native.isValid() : index < d->writes.size();
    if (!nativeCursor && !hasRecord) d->writes.emplace_back();
    WriteRecord *record = nativeCursor ? &d->cursorRecord : &d->writes[index];
    const auto discardNewRecord = [&] {
        if (!nativeCursor && !hasRecord) d->writes.pop_back();
    };
    if (!hasRecord) {
        if (!d->acquireBase(*record, key, error)) {
            discardNewRecord();
            return {};
        }
        if (d->nativeWrites) {
            record->native = d->mutation.beginWrite(key, d->writeMode, error);
            if (!record->native.isValid()) {
                // The canonical mutation stays active only when native
                // capability selection refused the operation before claims,
                // allocation or pixel exposure. That preflight refusal may
                // choose the generic route; a failed native operation may not.
                if (!d->mutation.isActive() || !d->mutation.cancel()) {
                    record->base.reset();
                    discardNewRecord();
                    return {};
                }
                d->mutation = {};
                d->nativeWrites = false;
                if (nativeCursor) {
                    d->writes.emplace_back(std::move(d->cursorRecord));
                    index = d->writes.size() - 1;
                    record = &d->writes[index];
                    nativeCursor = false;
                }
            }
        }
        if (!d->nativeWrites) {
            const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
            const auto request = d->store->acquireWrite(
                d->transaction, key, cpu, d->writeMode, d->priority);
            if (!request.isValid()) {
                record->base.reset();
                discardNewRecord();
                KisPageStoreDetail::setError(error, request.error.isEmpty()
                    ? QStringLiteral("CPU page write acquire failed") : request.error);
                return {};
            }
            record->writeLease.emplace(d->store->resolve(request, request.readiness));
            if (!record->writeLease->isValid()) {
                d->store->cancel(request);
                record->base.reset();
                discardNewRecord();
                KisPageStoreDetail::setError(error, QStringLiteral("CPU page write resolve failed"));
                return {};
            }
        }
        if (!nativeCursor) d->writeIndices.insert(key, index);
    } else if (!d->acquireBase(*record, key, error)) {
        return {};
    }
    if (d->lifetime == KisCpuAccessLifetime::CursorPage) d->cursorKey = key;

    KisCpuPageWriteSpan span;
    span.baseVersion = record->base.version();
    span.version = d->nativeWrites ? record->native.version() : record->writeLease->version();
    span.data = static_cast<quint8 *>(d->nativeWrites ? record->native.data() : record->writeLease->cpuData());
    span.oldData = record->base.data();
    span.rowStride = d->nativeWrites ? record->native.rowStride() : record->writeLease->rowStride();
    span.byteSize = d->nativeWrites ? record->native.byteSize() : record->writeLease->byteSize();
    KisPageStoreDetail::setError(error, {});
    return span;
}

bool KisPageStoreCpuAccessSession::finish(QString *error)
{
    if (!d->active || !d->store) {
        KisPageStoreDetail::setError(error, QStringLiteral("CPU access session is not active"));
        return false;
    }
    if (!d->writable) {
        d->releaseAllReads();
        d->active = false;
        KisPageStoreDetail::setError(error, {});
        return true;
    }

    if (d->nativeWrites) {
        d->releaseCursorWrite();
        for (WriteRecord &record : d->writes) {
            record.base.reset();
        }
        // Return guards/pins before sealing; the segment's parked writer
        // reservations still protect the pending versions. No pointer can
        // outlive its access capability.
        d->writes.clear();
        d->writeIndices.clear();
        d->cursorKey = {};
        const bool success = d->mutation.seal(error);
        d->mutation = {};
        d->capturedReadView = {};
        d->active = false;
        return success;
    }
    for (WriteRecord &record : d->writes) {
        record.base.reset();
        if (!d->store->publishHostWrite(std::move(*record.writeLease)).isValid()) {
            d->cancelAllWrites();
            d->active = false;
            KisPageStoreDetail::setError(error, QStringLiteral("CPU access session publish failed"));
            return false;
        }
    }
    d->writes.clear();
    d->writeIndices.clear();
    d->cursorKey = {};
    d->capturedReadView = {};
    d->active = false;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageStoreCpuAccessSession::cancel()
{
    if (!d->active) return;
    if (d->writable) {
        d->cancelAllWrites();
    } else {
        d->releaseAllReads();
    }
    d->active = false;
}
