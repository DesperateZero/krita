/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"
#ifdef NR104_INSTALL_AUDIT
#include <dlfcn.h>
#include <cstdio>
#endif

#ifdef NR104_INSTALL_AUDIT
struct NativeInstallAudit {
    struct Counts { size_t allocations, bytes, frees; };
    void (*start)() = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "auditStart"));
    Counts (*stop)() = reinterpret_cast<Counts (*)()>(dlsym(RTLD_DEFAULT, "auditStop"));
    void (*stack)() = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "auditStack"));
    bool active = false;
    NativeInstallAudit() { if (!start || !stop || !stack) qFatal("Native install audit requires the actual allocator interposer"); }
    void begin() { active = true; start(); }
    void end() {
        if (!std::exchange(active, false)) return;
        const auto c = stop();
        if (c.allocations || c.frees) { stack(); qFatal("NATIVE_INSTALL_AUDIT allocations=%zu bytes=%zu frees=%zu", c.allocations, c.bytes, c.frees); }
    }
    ~NativeInstallAudit() { end(); }
};
#endif
KisCpuWriteBindingReservation KisNativeResultProtocol::claimNative(
    const std::shared_ptr<KisCpuResidentBinding> &binding, quint64 incarnation,
    const KisPageVersion &target)
{
    // Birth established native layout/key/provider provenance. Actual mutable
    // exclusion is still the production Binding -> Tiles3 -> TileData service.
    if (!binding || !(binding->m_key == target.key) || !target.generation.isValid()
        || incarnation == std::numeric_limits<quint64>::max()
        || !binding->tryClaimHandoff(incarnation)) return {};
    binding->finishHandoff(&target);
    KisCpuWriteBindingReservation result;
    result.m_binding = binding;
    return result;
}

bool KisNativeResultProtocol::validateNative(KisCpuResidentBinding *b, const KisReplicaHandle &expected)
{
    // Called with B held until the all-D no-fail cut. Immutable layout is not
    // projected out of this same binding merely to compare it with itself.
    return b && !b->m_retired && !b->m_storageBusy
        && b->allocationIdentityLocked() == expected.allocationIdentity()
        && b->matchesVersionLocked(expected.version);
}

struct KisNativeResultProtocol::Async {
    std::shared_ptr<Impl> owner;
    std::atomic<Body *> body{nullptr};
    KisCompletionTicket ticket;
    ~Async() { finish(false, true); }
    bool finish(bool success, bool cancelled = false) {
        auto *b = body.exchange(nullptr);
        if (!b) return false;
        owner->completions->completePrepared(ticket, success ? KisCompletionStatus::Succeeded
            : cancelled ? KisCompletionStatus::Cancelled : KisCompletionStatus::Failed);
        const auto terminal = owner->completions->verifyTerminal(ticket);
        Q_ASSERT(terminal.isValid());
        owner->finishProducer(*b);
        bool abandoned;
        { QMutexLocker lock(&owner->gate); b->asyncPending = false; b->failed = !success; abandoned = b->abandoned; }
        if (abandoned) { owner->cancelBody(b); owner->finishClosing(); }
        return true;
    }
};

thread_local KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::iteratorHead = nullptr;

void KisNativeResultProtocol::Impl::ensureEdges(size_t n)
{
    QMutexLocker growing(&growthGate);
    for (;;) {
        {
            QMutexLocker lock(&gate);
            if (edgeFree >= edgeReserved + n) { edgeReserved += n; return; }
        }
        // Charged capacity is prepared outside S. All uses are intrusive and
        // allocation-free at the final visibility cut.
        auto block = std::allocate_shared<EdgeBlock>(Alloc<EdgeBlock>(&budget));
        blocks.push_back(block);
        QMutexLocker lock(&gate);
        for (auto &e : block->edges) { e.freeNext = freeEdges; freeEdges = &e; ++edgeFree; }
    }
}

KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::checkout()
{
    {
        QMutexLocker lock(&gate);
        if (closing || defaultWriter) return nullptr;
        ++active;
        if (auto *b = freeBodies) { freeBodies = b->next; b->next = nullptr; return b; }
    }
    try {
        QMutexLocker allocation(&bodyGate);
        bodies.emplace_back(&budget);
        return &bodies.back();
    } catch (...) { QMutexLocker lock(&gate); --active; throw; }
}

void KisNativeResultProtocol::Impl::returnBody(Body *b) noexcept
{
    // Candidate destruction can release real adapter/GC storage; never do it
    // under S or the manager gate.
    Q_ASSERT(!b->clients && !b->borrows);
    b->iterator = b->raw = b->revoked = false; b->thread = nullptr; b->iteratorOwner = nullptr; b->iteratorNext = nullptr;
    b->rows.clear(); b->moves.clear(); b->defaultReaders.clear(); b->history.reset();
    if (b->defaultTile) { b->defaultTile->deref(); b->defaultTile = nullptr; }
    b->defaultPixel.clear();
    b->ready = b->accepted = b->restore = b->rawDirty = false;
    b->asyncPending = b->abandoned = b->failed = b->adapterReady = false;
    QMutexLocker lock(&gate);
    if (defaultWriter == b) defaultWriter = nullptr;
    Q_ASSERT(edgeReserved >= b->edgeBudget);
    edgeReserved -= std::exchange(b->edgeBudget, 0);
    b->next = freeBodies; freeBodies = b; --active;
}

void KisNativeResultProtocol::Impl::cancelBody(Body *b) noexcept
{
    { QMutexLocker lock(&gate); if (b->asyncPending) { b->abandoned = true; return; } }
    if (!b->accepted) {
        finishProducer(*b);
        {
            QMutexLocker lock(&gate);
            for (auto &row : b->rows) if (auto *r = std::exchange(row.idleCandidate, nullptr)) {
                r->busy = false; park(r);
            }
        }
        Resource *released = nullptr;
        {
            QMutexLocker lock(&gate);
            for (auto &row : b->rows) if (row.ownsTarget && row.target) {
                Q_ASSERT(row.target->pending); --row.target->pending;
                claimReleased(row.target, released);
                row.target = nullptr; row.ownsTarget = false;
            }
        }
        settleReleased(released);
        QMutexLocker lock(&gate);
        for (auto &row : b->rows) if (row.page && row.page->writer == b) row.page->writer = nullptr;
        COUNT(cancelled, 1);
    }
    returnBody(b);
}

void KisNativeResultProtocol::Impl::finishProducer(Body &b) noexcept
{
    for (auto &row : b.rows) {
        row.writable.unpin(); row.writable.reset(); row.pixels = nullptr;
    }
    b.ready = true;
}

KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::iteratorBody() const
{
    for (auto *b = iteratorHead; b; b = b->iteratorNext)
        if (b->iteratorOwner == this && !b->revoked) return b;
    return nullptr;
}

void KisNativeResultProtocol::Impl::detachIterator(Body *b)
{
    auto **link = &iteratorHead;
    while (*link && *link != b) link = &(*link)->iteratorNext;
    Q_ASSERT(*link == b);
    if (*link) *link = b->iteratorNext;
    b->iteratorNext = nullptr;
}

bool KisNativeResultProtocol::Impl::finishRawResult(Body *body) noexcept
{
    auto owner = std::move(body->deferredOwner);
    if (!body->rawDirty || body->failed) {
        const bool ok = !body->failed || body->revoked; owner->cancelBody(body); return ok;
    }
    owner->finishProducer(*body);
    QString failure;
    if (owner->acceptBody(*body, nullptr, &failure)) { owner->returnBody(body); return true; }
    KisStrokeJobFailureContext::reportFailure(failure.isEmpty()
        ? QStringLiteral("Native raw result acceptance failed") : failure);
    owner->cancelBody(body); return false;
}

void KisNativeResultProtocol::Impl::finishRaw(void *opaque) noexcept
{
    auto *body = static_cast<Body *>(opaque);
    Q_ASSERT(body->borrows);
    if (--body->borrows == 0 && !body->clients) body->deferredOwner->finishRawResult(body);
}

bool KisNativeResultProtocol::Impl::finishIterator(Body *b, bool success)
{
    b->failed |= !success;
    Q_ASSERT(b->clients);
    if (--b->clients) return !b->failed || b->revoked;
    detachIterator(b);
    return b->borrows ? (!b->failed || b->revoked) : finishRawResult(b);
}

bool KisNativeResultProtocol::Impl::bindRows(Body &b, const QRect &rect, QString *why, bool append)
{
    const QRect tiles(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                      QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    const quint64 total = quint64(tiles.width()) * quint64(tiles.height());
    if (!total || total > std::numeric_limits<quint32>::max()
        || !KisTileHashTable::supportsCoordinates(tiles.left(), tiles.top())
        || !KisTileHashTable::supportsCoordinates(tiles.right(), tiles.bottom())) return false;
    const size_t first = append ? b.rows.size() : 0;
    b.rows.resize(first + size_t(total)); b.moves.reserve(b.rows.size() * 2);
    Pages staged(std::less<Key>{}, pages.get_allocator());
    {
        QMutexLocker lock(&gate);
        size_t at = first;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto it = pages.find({y, x});
            if (it != pages.end()) b.rows[at].page = &it->second;
        }
    }
    size_t at = first;
    for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at)
        if (!b.rows[at].page) {
            auto it = staged.try_emplace({y, x}).first;
            it->second.key = {{1}, {x, y}};
        }
    {
        QMutexLocker lock(&gate);
        if (closing) return false;
        pages.merge(staged);
        at = first;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto &row = b.rows[at];
            if (!row.page) row.page = &pages.find({y, x})->second;
            if (row.page->writer) { error(why, "same-page writer conflict"); return false; }
            row.page->writer = &b; row.page->writerIndex = at; row.before = row.page->current;
            row.local = rect.intersected(QRect(x * 64, y * 64, 64, 64)).translated(-x * 64, -y * 64);
        }
    }
    return true;
}

bool KisNativeResultProtocol::Impl::prepareAdapter(Body &b, QString *why)
{
    if (b.adapterReady) return true;
    size_t changed = 0; QRect range;
    for (const auto &row : b.rows) if (row.changed) {
        ++changed;
        if (row.after.resource) range |= QRect(row.page->key.page.column, row.page->key.page.row, 1, 1);
    }
    if (!b.edgeBudget && changed) { ensureEdges(changed); b.edgeBudget = changed; }
    if (!range.isEmpty() && !KisNativeResultAdapter::extent(adapter).prepareTileRange(range)) {
        error(why, "adapter extent capacity refused; Ready retained"); return false;
    }
    auto &table = KisNativeResultAdapter::table(adapter);
    for (auto &row : b.rows) if (row.changed) {
        const auto update = row.after.resource ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent;
        if (!row.adapter && !(row.location && row.after.resource))
            row.adapter = table.prepareTileUpdate(row.page->key.page.column, row.page->key.page.row, update, &row.location);
    }
    for (auto &row : b.rows) if (row.changed && !row.after.resource)
        row.location = table.borrowTileForPreparedUpdate(row.page->key.page.column, row.page->key.page.row);
    if (b.defaultTile) {
        b.defaultReaders.clear();
        KisTileHashTableConstIterator it(&table);
        while (!it.isDone()) {
            auto tile = it.tile(); bool affected;
            { QMutexLocker lock(&gate); auto p = pages.find({tile->row(), tile->col()}); affected = p == pages.end() || !p->second.current.resource; }
            if (affected) b.defaultReaders.push_back(std::move(tile));
            it.next();
        }
    }
    b.adapterReady = true; return true;
}

bool KisNativeResultProtocol::Impl::prepareTargets(Body &b, const quint8 *data,
    const QRect &rect, qsizetype stride, const Faults *faults, QString *why, size_t first, size_t last)
{
    const int bpp = descriptor.format.pixelStride;
    const bool producer = !data;
    if (rect.isEmpty() || (!producer && !bounds(data, rect, stride, bpp))) { error(why, "invalid packed input"); return false; }
    if (b.rows.empty() && !bindRows(b, rect, why)) return false;
    last = std::min(last, b.rows.size());
    const auto payloadFor = [&](const Body::Row &row) -> const quint8 * {
        return producer ? nullptr : data + (qint64(row.page->key.page.row) * 64 + row.local.top() - rect.top()) * stride
            + (qint64(row.page->key.page.column) * 64 + row.local.left() - rect.left()) * bpp;
    };
    // Original rows retain original positions. Positive destination admission
    // is one actual budget group, while physical exclusion remains per backing.
    KisBackingBudgetDelta reuseChange;
    size_t reuseCandidates = 0;
    const auto transientLimit = budget.maxTransientVersionsPerPage();
    for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) {
        auto &p = *row.page;
        if (row.selectionOnly) continue;
        if (row.remove) { row.changed = bool(row.before.resource); row.after = {}; continue; }
        const auto local = row.local;
        const auto *payload = payloadFor(row);
        const auto *source = row.before.resource;
        bool same = !producer;
        if (!producer && source) {
            const auto *before = static_cast<const quint8 *>(source->binding->acquireRead(source->handle.allocationIdentity(), false));
            if (!before) { error(why, "source pin refused"); return false; }
            for (int y = 0; y < local.height() && same; ++y)
                same = std::memcmp(before + ((local.y() + y) * 64 + local.x()) * bpp,
                                   payload + y * stride, size_t(local.width() * bpp)) == 0;
            source->binding->releaseRead();
        } else if (!producer) {
            const auto *pixel = descriptor.format.defaultPixel.constData();
            for (int y = 0; y < local.height() && same; ++y)
                for (int x = 0; x < local.width() && same; ++x)
                    same = std::memcmp(payload + y * stride + x * bpp, pixel, size_t(bpp)) == 0;
        }
        if (same) continue;
        QMutexLocker lock(&gate);
        if (p.nextGeneration == std::numeric_limits<quint64>::max()) return false;
        if (p.transient >= transientLimit) {
            error(why, "page transient-result budget exhausted"); return false;
        }
        row.after.generation = p.nextGeneration++; row.changed = true;
        if (!forceFresh && p.idle && !p.idle->busy) {
            auto *idle = p.idle; p.idle = idle->nextIdle;
            idle->nextIdle = nullptr; idle->idle = false; idle->busy = true;
            row.idleCandidate = idle; ++reuseCandidates;
            delta(reuseChange, idle->charged, idle->domain, -qint64(descriptor.minimumByteSize()));
            delta(reuseChange, Class::ActivePending, idle->domain, qint64(descriptor.minimumByteSize()));
        }
    }
    if (reuseCandidates) {
        auto fee = budget.reserveChange(reuseChange, why);
        if (!fee.isValid()) return false;
        KisBackingBudgetDelta succeeded;
        for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (auto *idle = row.idleCandidate) {
            if (!(faults && faults->rejectPhysical)) row.writable = KisNativeResultProtocol::claimNative(
                idle->binding, idle->handle.allocation.generation, {row.page->key, {row.after.generation}});
            if (!row.writable.isValid()) continue;
            delta(succeeded, idle->charged, idle->domain, -qint64(descriptor.minimumByteSize()));
            delta(succeeded, Class::ActivePending, idle->domain, qint64(descriptor.minimumByteSize()));
        }
        // All unclaimed positive shares are returned before Fresh can reserve.
        fee.commit(succeeded);
        for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (auto *idle = std::exchange(row.idleCandidate, nullptr)) {
            if (row.writable.isValid()) {
                QMutexLocker lock(&gate);
                idle->handle.version = {row.page->key, {row.after.generation}}; ++idle->handle.allocation.generation;
                idle->charged = Class::ActivePending; idle->pending = 1; idle->transient = true; ++row.page->transient; idle->busy = false;
                row.target = idle; row.ownsTarget = true; COUNT(handoff, 1);
            } else {
                { QMutexLocker lock(&gate); idle->busy = false; }
                retire(idle);
            }
        }
    }
    size_t made = 0;
    for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (row.changed && !row.remove && !row.selectionOnly) {
        const auto local = row.local;
        const auto *payload = payloadFor(row);
        auto *source = row.before.resource;
        const bool reused = row.target != nullptr;
        if (!row.target) {
            row.target = fresh(*row.page, {row.page->key, {row.after.generation}}, payload, stride,
                               row.discard ? nullptr : source, local, why);
            if (!row.target) return false;
            row.ownsTarget = true;
            if (producer || local != QRect(0, 0, 64, 64)) row.writable = KisCpuWriteBindingReservation::acquire(
                row.target->binding, row.target->handle.allocationIdentity());
            if ((producer || local != QRect(0, 0, 64, 64)) && !row.writable.isValid()) return false;
        }
        auto &writable = row.writable;
        if (writable.isValid()) {
            auto *destination = static_cast<quint8 *>(writable.pinResident());
            if (!destination) destination = static_cast<quint8 *>(writable.materialize());
            if (!destination) return false;
            if (reused && !row.discard && (producer || local != QRect(0, 0, 64, 64))) {
                if (source) {
                    const void *before = source->binding->acquireRead(source->handle.allocationIdentity(), false);
                    if (!before) return false;
                    std::memcpy(destination, before, size_t(descriptor.minimumByteSize()));
                    source->binding->releaseRead();
                } else for (int i = 0; i < 4096; ++i)
                    std::memcpy(destination + i * bpp, descriptor.format.defaultPixel.constData(), size_t(bpp));
            }
            if (producer) {
                if (!row.discard && !source && !reused) for (int i = 0; i < 4096; ++i)
                    std::memcpy(destination + i * bpp, descriptor.format.defaultPixel.constData(), size_t(bpp));
                row.pixels = destination;
            } else {
                for (int y = 0; y < local.height(); ++y)
                    std::memcpy(destination + ((local.y() + y) * 64 + local.x()) * bpp,
                                payload + y * stride, size_t(local.width() * bpp));
                writable.unpin(); writable.reset();
            }
        }
        row.after.resource = row.target;
        if (faults && faults->rejectAfterTargets >= 0 && ++made == size_t(faults->rejectAfterTargets)) {
            error(why, "injected producer failure after real targets"); return false;
        }
    }
    b.ready = !producer;
    return true;
}

void KisNativeResultProtocol::Impl::buildPlan(Body &b)
{
    b.moves.clear(); b.cutEpoch = epoch + 1;
    // At most two facts per changed row. Sort/reduce the actual resources once,
    // so alias/restore share fees and physical gates instead of quadratic scans.
    const auto add = [&](Resource *r, int c, int h, int p, bool target) {
        if (r) b.moves.push_back({r, r->charged, r->charged, r->domain, r->domainRevision, c, h, p, target});
    };
    for (auto &row : b.rows) if (row.changed) {
        row.retain = retentionOwner(row.page->begin, b.cutEpoch);
        add(row.before.resource, -1, row.retain ? 1 : 0, 0, false);
        add(row.after.resource, 1, 0, row.ownsTarget ? -1 : 0, true);
    }
    std::sort(b.moves.begin(), b.moves.end(), [](const Move &a, const Move &z) {
        return std::less<Resource *>{}(a.resource, z.resource);
    });
    size_t out = 0;
    for (const auto &m : b.moves) {
        if (out && b.moves[out - 1].resource == m.resource) {
            auto &v = b.moves[out - 1]; v.current += m.current; v.retained += m.retained;
            v.pending += m.pending; v.target |= m.target;
        } else b.moves[out++] = m;
    }
    b.moves.resize(out);
    auto idleAllowance = budget.idleCapacityAllowance();
    for (auto &m : b.moves) {
        auto *r = m.resource;
        const int c = int(r->current) + m.current, h = int(r->retained) + m.retained, p = int(r->pending) + m.pending;
        Q_ASSERT(c >= 0 && h >= 0 && p >= 0);
        m.after = c ? Class::Current : h ? Class::RetainedHistory : p ? Class::ActivePending : Class::IdleCapacity;
        if (m.after == Class::IdleCapacity && m.before != Class::IdleCapacity) {
            if (idleAllowance >= descriptor.minimumByteSize()) idleAllowance -= descriptor.minimumByteSize();
            else m.after = Class::RetirementDebt; // consume the original prepaid terminal credit
        }
    }
}

void KisNativeResultProtocol::Impl::preparePlan(Body &b, KisBackingBudgetDelta &change)
{
    buildPlan(b);
    for (const auto &m : b.moves) if (m.before != m.after && m.after != Class::RetirementDebt) {
        delta(change, m.before, m.domain, -qint64(descriptor.minimumByteSize()));
        delta(change, m.after, m.domain, qint64(descriptor.minimumByteSize()));
    }
}

bool KisNativeResultProtocol::Impl::samePlan(Body &b) const
{
    if (closing || epoch == std::numeric_limits<quint64>::max()) return false;
    if (b.history && (b.history->phase == KisNativeHistory::Phase::ClosingAbort
        || b.history->phase == KisNativeHistory::Phase::Failed)) return false;
    b.cutEpoch = epoch + 1; // unrelated pages do not invalidate funded facts
    for (const auto &row : b.rows) if (row.changed) {
        if (row.page->writer != &b || row.page->current.resource != row.before.resource
            || row.page->current.generation != row.before.generation) return false;
        const bool retainedNow = retentionOwner(row.page->begin, b.cutEpoch) != nullptr;
        if (retainedNow != (row.retain != nullptr)) return false;
    }
    for (const auto &m : b.moves) {
        auto *r = m.resource;
        if (r->busy || r->charged != m.before || r->domain != m.domain || r->domainRevision != m.revision) return false;
        const int c = int(r->current) + m.current, h = int(r->retained) + m.retained, p = int(r->pending) + m.pending;
        if (c < 0 || h < 0 || p < 0) return false;
        const auto wanted = c ? Class::Current : h ? Class::RetainedHistory : p ? Class::ActivePending : Class::IdleCapacity;
        if (wanted != m.after && !(wanted == Class::IdleCapacity && m.after == Class::RetirementDebt)) return false;
    }
    return true;
}

bool KisNativeResultProtocol::Impl::acceptBody(Body &b, const Faults *faults, QString *why)
try {
    { QMutexLocker lock(&gate);
      if (b.asyncPending || !b.ready || b.accepted || b.failed
          || (b.history && (b.history->phase == KisNativeHistory::Phase::ClosingAbort
              || b.history->phase == KisNativeHistory::Phase::Failed))) return false; }
    const size_t changed = size_t(std::count_if(b.rows.begin(), b.rows.end(), [](const auto &r) { return r.changed; }));
    if (!changed && !b.defaultTile) {
        QMutexLocker lock(&gate);
        for (auto &r : b.rows) if (r.page->writer == &b) r.page->writer = nullptr;
        b.accepted = true; return true;
    }
    KisPageStoreDiagnosticTimer publishing(nullptr, KisPageStoreDiagnosticPhase::WritePublishHost, changed);
    if (!prepareAdapter(b, why)) return false;
    KisBackingBudgetDelta change;
    {
        QMutexLocker lock(&gate);
        if (epoch == std::numeric_limits<quint64>::max()) return false;
        preparePlan(b, change);
    }
    auto fee = budget.reserveChange(change, why);
    if (!fee.isValid()) return false;
    if (faults && faults->beforeAccept) faults->beforeAccept();
    if (faults && faults->rejectAdapter) { error(why, "adapter preparation refused; Ready retained"); return false; }
#ifdef NR104_INSTALL_AUDIT
    NativeInstallAudit installAudit;
#endif
    QWriteLocker adapterGate(&KisNativeResultAdapter::gate(adapter));
    {
        QMutexLocker lock(&gate);
        if (!samePlan(b)) { COUNT(retried, 1); error(why, "actual final facts changed; Ready retained"); return false; }
        // Resource order follows canonical PageSlot order. No old Value,
        // metadata index, proof projection or publication entry is traversed.
        size_t locked = 0;
        int targetIndex = 0;
        const auto unlock = qScopeGuard([&] {
            for (size_t i = locked; i; --i) if (b.moves[i - 1].target)
                b.moves[i - 1].resource->binding->m_mutex.unlock();
        });
        for (auto &move : b.moves) {
            ++locked;
            if (!move.target) continue;
            auto *r = move.resource;
            r->binding->m_mutex.lock();
            if (!KisNativeResultProtocol::validateNative(r->binding.get(), r->handle)
                || (faults && (faults->rejectAccept || faults->rejectAcceptAt == targetIndex))) {
                error(why, "actual Resource validation refused; nothing installed"); return false;
            }
            ++targetIndex;
        }
#ifdef NR104_INSTALL_AUDIT
        installAudit.begin();
#endif
        for (auto &row : b.rows) if (row.changed) {
            auto &p = *row.page;
            // Reconsume the actual owner; an equal-epoch capture/release does
            // not invalidate the already funded class plan.
            if (auto *v = retentionOwner(p.begin, b.cutEpoch)) {
                Q_ASSERT(freeEdges);
                auto *edge = freeEdges; freeEdges = edge->freeNext; --edgeFree;
                Q_ASSERT(b.edgeBudget && edgeReserved); --b.edgeBudget; --edgeReserved;
                edge->page = &p; edge->selection = row.before; edge->begin = p.begin; edge->end = b.cutEpoch;
                p.history.insert(*edge); linkEdge(edge, v);
                if (row.before.resource) ++row.before.resource->retained;
                COUNT(retainedEdges, 1);
            }
            if (row.before.resource) --row.before.resource->current;
            if (row.after.resource) ++row.after.resource->current;
            if (row.ownsTarget) --row.target->pending;
            p.current = row.after; p.begin = b.cutEpoch;
        }
        fee.commit(change);
        for (const auto &m : b.moves) {
            if (m.after == Class::RetirementDebt && m.before != m.after) {
                KisBackingBudgetDelta debt;
                delta(debt, m.before, m.domain, -qint64(descriptor.minimumByteSize()));
                delta(debt, Class::RetirementDebt, m.domain, qint64(descriptor.minimumByteSize()));
                m.resource->debt.commit(debt);
            }
            m.resource->charged = m.after;
            settleTransient(m.resource);
            if (m.after == Class::IdleCapacity && !m.resource->idle) park(m.resource);
        }
        if (currentHistory) {
            QRect changedBounds;
            for (const auto &row : b.rows) if (row.changed)
                changedBounds |= QRect(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
            currentHistory->setExtent(currentHistory->extent() | changedBounds);
        }
        epoch = b.cutEpoch; b.accepted = true;
        if (b.defaultTile) {
            std::swap(descriptor.format.defaultPixel, b.defaultPixel);
            std::swap(defaultBacking, b.defaultTile); ++defaultRevision;
        }
        COUNT(results, changed);
    }
    // Real adapter work stays inside the manager cut but outside S/B: legacy
    // Tile read takes its swap barrier before asking this bridge for selection.
    auto &table = KisNativeResultAdapter::table(adapter);
    auto &extent = KisNativeResultAdapter::extent(adapter);
    if (b.defaultTile) {
        KisNativeResultAdapter::installDefault(adapter, reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()), defaultBacking);
        for (auto &tile : b.defaultReaders) tile->invalidatePageStoreReadCache();
    }
    for (auto &row : b.rows) if (row.changed) {
        if (!row.after.resource) {
            // The exact adapter capability dies after the no-fail cut, in its
            // original row. Detach atomically with cache extraction so a raw
            // reader cannot acquire a new bridge lease in between.
            if (auto *tile = row.location) {
                QMutexLocker tileLock(&tile->m_swapBarrierLock);
                tile->m_pageStoreBridge.storeRelease(nullptr);
                if (!tile->m_lockCounter) row.detachedRead = std::move(tile->m_pageStoreLeases[KisTile::ReadLease]);
            }
            if (table.removePreparedTile(row.adapter)) extent.notifyTileRemoved(row.page->key.page.column, row.page->key.page.row);
            continue;
        }
        KisTileSP installed;
        bool created = false;
        auto *tile = row.location;
        if (!tile) { installed = table.installPreparedTile(row.adapter, created); tile = installed.data(); }
        Q_ASSERT(tile);
        {
            QMutexLocker tileLock(&tile->m_swapBarrierLock);
            if (tile->m_pageStoreBridge.loadRelaxed() != this || tile->m_pageStoreOldDataView.loadRelaxed()) {
                Q_ASSERT(!tile->m_lockCounter);
                row.detachedRead = std::move(tile->m_pageStoreLeases[KisTile::ReadLease]);
                tile->m_pageStoreOldDataView.storeRelaxed(0);
                tile->m_pageStoreBridge.storeRelease(this);
            }
            tile->m_pageStoreNativeReadReady.storeRelease(0);
        }
        if (created) extent.notifyTileAdded(row.page->key.page.column, row.page->key.page.row);
#ifdef NR104_CENSUS
        { QMutexLocker countLock(&gate); COUNT(adapterInstalled, 1); }
#endif
    }
    adapterGate.unlock();
#ifdef NR104_INSTALL_AUDIT
    installAudit.end();
    std::fprintf(stderr, "NATIVE_INSTALL_AUDIT_OK changed=%zu default=%d restore=%d\n", changed, int(b.defaultTile != nullptr), int(b.restore));
#endif
    {
        QMutexLocker lock(&gate);
        for (auto &row : b.rows) if (row.page->writer == &b) row.page->writer = nullptr;
    }
    for (const auto &m : b.moves) if (m.after == Class::RetirementDebt) retire(m.resource);
    return true;
} catch (const std::bad_alloc &) { error(why, "acceptance preparation capacity refused; Ready retained"); return false; }

KisNativeResultProtocol::Prepared::~Prepared() { if (body) owner->cancelBody(body); }
KisNativeResultProtocol::Prepared::Prepared(Prepared &&other) noexcept
    : owner(std::move(other.owner)), body(std::exchange(other.body, nullptr)) {}
KisNativeResultProtocol::Prepared &KisNativeResultProtocol::Prepared::operator=(Prepared &&other) noexcept
{
    if (this != &other) {
        if (body) owner->cancelBody(body);
        owner = std::move(other.owner); body = std::exchange(other.body, nullptr);
    }
    return *this;
}

KisNativeResultProtocol::Prepared KisNativeResultProtocol::prepareWrite(
    const quint8 *bytes, const QRect &pixels, qsizetype stride, QString *why, const Faults *faults)
try {
    if (!bytes) { error(why, "null packed input"); return {}; }
    Prepared result;
    result.owner = d; result.body = d->checkout();
    if (!result.body || !d->prepareTargets(*result.body, bytes, pixels, stride, faults, why)) return {};
    return result;
} catch (const std::bad_alloc &) { error(why, "real managed storage admission refused"); return {}; }

bool KisNativeResultProtocol::accept(Prepared &p, QString *why, const Faults *faults)
{
    if (p.owner != d || !p.body || !d->acceptBody(*p.body, faults, why)) return false;
    d->returnBody(std::exchange(p.body, nullptr)); p.owner.reset(); return true;
}

KisNativeResultProtocol::Prepared KisNativeResultProtocol::prepareProducer(const QRect &pixels, QString *why)
try {
    Prepared p; p.owner = d; p.body = d->checkout();
    if (!p.body || !d->prepareTargets(*p.body, nullptr, pixels, 0, nullptr, why)) return {};
    return p;
} catch (const std::bad_alloc &) { error(why, "producer storage refused"); return {}; }

quint8 *KisNativeResultProtocol::producerPage(Prepared &p, qint32 col, qint32 row, quint32 *stride)
{
    if (p.owner != d || !p.body || p.body->ready || p.body->failed) return nullptr;
    for (auto &r : p.body->rows) if (r.page->key.page.column == col && r.page->key.page.row == row) {
        if (stride) *stride = 64 * d->descriptor.format.pixelStride;
        return r.pixels;
    }
    return nullptr;
}

bool KisNativeResultProtocol::finishProducer(Prepared &p, bool success, QString *)
{
    if (p.owner != d || !p.body || p.body->ready || p.body->asyncPending) return false;
    if (!success) { cancel(p); return false; }
    d->finishProducer(*p.body); return true;
}

bool KisNativeResultProtocol::ProducerCompletion::finish(bool success)
{ return d && d->finish(success); }
KisCompletionTicket KisNativeResultProtocol::ProducerCompletion::ticket() const
{ return d ? d->ticket : KisCompletionTicket{}; }

KisNativeResultProtocol::ProducerCompletion KisNativeResultProtocol::deferProducer(Prepared &p, QString *why)
try {
    if (p.owner != d || !p.body || p.body->ready || p.body->asyncPending || p.body->failed) return {};
    ProducerCompletion completion;
    completion.d = std::allocate_shared<Async>(Alloc<Async>::retained(&d->budget)); completion.d->owner = d;
    {
        QMutexLocker setup(&d->asyncGate);
        if (!d->asyncSource) d->asyncSource = d->completions->registerSource(KisCompletionDomain::HostLogical);
        if (!d->asyncSource) return {};
        completion.d->ticket = d->completions->allocatePending(d->asyncSource);
        if (!completion.d->ticket.isValid()) return {};
    }
    auto cancelTicket = qScopeGuard([&] {
        if (!completion.d->body) d->completions->completePrepared(completion.d->ticket, KisCompletionStatus::Cancelled);
    });
    KisBackingBudgetDelta change;
    for (const auto &row : p.body->rows) if (row.target) {
        delta(change, row.target->charged, row.target->domain, -qint64(d->descriptor.minimumByteSize()));
        delta(change, Class::InFlight, row.target->domain, qint64(d->descriptor.minimumByteSize()));
    }
    auto fee = d->budget.reserveChange(change, why); if (!fee.isValid()) return {};
    {
        QMutexLocker lock(&d->gate);
        fee.commit(change);
        for (const auto &row : p.body->rows) if (row.target) row.target->charged = Class::InFlight;
        p.body->asyncPending = true; completion.d->body = p.body;
    }
    cancelTicket.dismiss(); return completion;
} catch (const std::bad_alloc &) { error(why, "asynchronous completion capacity refused"); return {}; }

void KisNativeResultProtocol::cancel(Prepared &p) noexcept
{ if (p.body) p.owner->cancelBody(std::exchange(p.body, nullptr)); p.owner.reset(); }

bool KisNativeResultProtocol::write(const quint8 *bytes, const QRect &pixels, qsizetype stride,
                                  QString *why, const Faults *faults)
{ auto prepared = prepareWrite(bytes, pixels, stride, why, faults); return prepared && accept(prepared, why, faults); }


KisNativeResultProtocol::Prepared KisNativeResultProtocol::prepareWrite(
    const quint8 *bytes, const QRect &rect, qsizetype stride, QString *why)
{
    return prepareWrite(bytes, rect, stride, why, nullptr);
}

bool KisNativeResultProtocol::accept(Prepared &body, QString *why)
{
    return accept(body, why, nullptr);
}

bool KisNativeResultProtocol::write(const quint8 *bytes, const QRect &rect, qsizetype stride, QString *why)
{
    return write(bytes, rect, stride, why, nullptr);
}
