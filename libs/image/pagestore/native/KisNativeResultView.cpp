/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"

void KisNativeResultProtocol::Impl::releaseView(View *v) noexcept
{
    Resource *released = nullptr;
    Views::node_type releasedView;
    {
        QMutexLocker lock(&gate);
        if (v->explicitItems) {
            for (const auto &item : v->items) if (auto *r = item.selection.resource) {
                Q_ASSERT(r->retained); --r->retained; claimReleased(r, released);
            }
        } else {
            releasedView = views.extract({v->epoch, v->serial}); v->registered = false;
            while (v->first) {
                auto *e = v->first; unlinkEdge(e);
                if (auto *next = retentionOwner(e->begin, e->end)) { linkEdge(e, next); continue; }
                if (auto *r = e->selection.resource) { --r->retained; claimReleased(r, released); }
                e->page->history.erase(e->page->history.iterator_to(*e));
                e->freeNext = freeEdges; freeEdges = e; ++edgeFree;
            }
        }
    }
    settleReleased(released); finishClosing();
}

bool KisNativeResultProtocol::Impl::readSelection(Selection selected, quint8 *out,
    const QRect &local, qsizetype stride, const KisPageByteArray &defaultPixel, QString *why) const
{
    const int bpp = descriptor.format.pixelStride;
    if (!selected.resource) {
        for (int y = 0; y < local.height(); ++y) for (int x = 0; x < local.width(); ++x)
            std::memcpy(out + y * stride + x * bpp, defaultPixel.constData(), size_t(bpp));
        return true;
    }
    auto *r = selected.resource;
    const auto *data = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
    if (!data) { error(why, "real historical/current read pin refused"); return false; }
    for (int y = 0; y < local.height(); ++y)
        std::memcpy(out + y * stride, data + ((local.y() + y) * 64 + local.x()) * bpp, size_t(local.width() * bpp));
    r->binding->releaseRead();
    return true;
}

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::capture(QString *why)
try {
    Snapshot s;
    s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    Impl::Views staged(std::less<std::pair<quint64, quint64>>{}, d->views.get_allocator());
    staged.emplace(std::pair<quint64, quint64>{0, 0}, s.d.get());
    auto node = staged.extract(staged.begin());
    QMutexLocker lock(&d->gate);
    if (d->closing) return {};
    if (d->viewSerial == std::numeric_limits<quint64>::max()) return {};
    s.d->epoch = d->epoch; s.d->serial = ++d->viewSerial; s.d->defaultRevision = d->defaultRevision;
    s.d->defaultPixel = d->descriptor.format.defaultPixel;
    s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref();
    node.key() = {s.d->epoch, s.d->serial}; d->views.insert(std::move(node));
    s.d->registered = true; return s;
} catch (const std::bad_alloc &) { error(why, "snapshot capacity refused"); return {}; }

bool KisNativeResultProtocol::restore(const Snapshot &s, QString *why)
try {
    if (!s.d || s.d->owner != d || s.d->explicitItems) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; b.restore = true;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate);
        if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &page = item.second; auto &row = b.rows[n++]; row.page = &page;
            if (page.writer) return false;
            page.writer = &b; row.before = page.current; row.after = d->viewSelection(*s.d, item.first);
            row.target = row.after.resource;
            row.changed = row.before.resource != row.after.resource || row.before.generation != row.after.generation;
        }
    }
    auto &table = KisNativeResultAdapter::table(d->adapter);
    for (auto &row : b.rows) if (row.changed) {
        row.adapter = table.prepareTileUpdate(row.page->key.page.column, row.page->key.page.row,
            row.after.resource ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent, &row.location);
    }
    if (s.d->defaultPixel != d->descriptor.format.defaultPixel &&
        !d->prepareDefault(b, s.d->defaultPixel, why, s.d->defaultBacking)) return false;
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "restore preparation refused"); return false; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::captureRange(const QRect &rect, QString *why)
try {
    if (rect.isEmpty()) return {};
    const QRect tiles(QPoint(floorTile(rect.left()), floorTile(rect.top())), QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    Snapshot s; s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    s.d->range = rect; s.d->items.reserve(size_t(tiles.width()) * size_t(tiles.height()));
    QMutexLocker lock(&d->gate);
    if (d->closing) return {};
    s.d->defaultPixel = d->descriptor.format.defaultPixel; s.d->defaultRevision = d->defaultRevision;
    s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref();
    for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x) {
        auto it = d->pages.find({y, x});
        const auto selected = it != d->pages.end() ? it->second.current : Impl::Selection{};
        s.d->items.push_back({{y, x}, selected});
        if (selected.resource) ++selected.resource->retained;
    }
    s.d->explicitItems = true;
    return s;
} catch (const std::bad_alloc &) { error(why, "range capability capacity refused"); return {}; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::capturePrepared(const Prepared &p, QString *why)
try {
    if (p.owner != d || !p.body || !p.body->ready || p.body->failed || p.body->asyncPending) return {};
    Snapshot s; s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    s.d->items.reserve(p.body->rows.size());
    { QMutexLocker lock(&d->gate);
      s.d->defaultPixel = d->descriptor.format.defaultPixel; s.d->defaultRevision = d->defaultRevision;
      s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref(); }
    Vec<Impl::Move> claims{Alloc<Impl::Move>(&d->budget)};
    claims.reserve(p.body->rows.size());
    auto undo = qScopeGuard([&] {
        QMutexLocker lock(&d->gate);
        for (const auto &m : claims) m.resource->busy = false;
    });
    KisBackingBudgetDelta change;
    {
        QMutexLocker lock(&d->gate);
        for (const auto &row : p.body->rows) {
            auto selected = row.changed ? row.after : row.before;
            auto *r = selected.resource;
            if (r && r->busy) return {};
            if (r && r->charged == Class::ActivePending) {
                r->busy = true;
                claims.push_back({r, r->charged, Class::RetainedHistory, r->domain, r->domainRevision});
                delta(change, Class::ActivePending, r->domain, -qint64(d->descriptor.minimumByteSize()));
                delta(change, Class::RetainedHistory, r->domain, qint64(d->descriptor.minimumByteSize()));
            }
            s.d->items.push_back({{row.page->key.page.row, row.page->key.page.column}, selected});
            s.d->range |= QRect(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
        }
    }
    auto fee = d->budget.reserveChange(change, why);
    if (!fee.isValid()) return {};
    {
        QMutexLocker lock(&d->gate);
        fee.commit(change);
        for (const auto &m : claims) m.resource->charged = Class::RetainedHistory;
        for (const auto &item : s.d->items) if (auto *r = item.selection.resource) ++r->retained;
    }
    s.d->explicitItems = true;
    return s;
} catch (const std::bad_alloc &) { error(why, "private capability capacity refused"); return {}; }

bool KisNativeResultProtocol::read(const Snapshot &s, quint8 *bytes, const QRect &pixels,
                                  qsizetype stride, QString *why) const
{ return s.d && s.d->owner == d && s.read(bytes, pixels, stride, why); }

bool KisNativeResultProtocol::Snapshot::read(quint8 *bytes, const QRect &pixels,
                                            qsizetype stride, QString *why) const
{
    if (!d || !bounds(bytes, pixels, stride, d->owner->descriptor.format.pixelStride)
        || (d->explicitItems && !d->range.contains(pixels))) return false;
    const int bpp = d->owner->descriptor.format.pixelStride;
    for (int y = floorTile(pixels.top()); y <= floorTile(pixels.bottom()); ++y)
        for (int x = floorTile(pixels.left()); x <= floorTile(pixels.right()); ++x) {
            Impl::Selection selected;
            { QMutexLocker lock(&d->owner->gate); selected = d->owner->viewSelection(*d, {y, x}); }
            const QRect hit = pixels.intersected(QRect(x * 64, y * 64, 64, 64));
            if (!d->owner->readSelection(selected, bytes + qint64(hit.y() - pixels.y()) * stride + (hit.x() - pixels.x()) * bpp,
                                  hit.translated(-x * 64, -y * 64), stride, d->defaultPixel, why)) return false;
        }
    return true;
}

bool KisNativeResultProtocol::readCurrent(quint8 *bytes, const QRect &pixels, qsizetype stride, QString *why) const
{
    if (!bounds(bytes, pixels, stride, d->descriptor.format.pixelStride)) return false;
    // Same manager read protection as the public packed-read contract. A raw
    // Tile may still pin an older selection, so its cached pixels cannot be
    // the authority for this new current read. Consume PageSlot directly.
    QReadLocker adapterGate(&KisNativeResultAdapter::gate(d->adapter));
    const auto bpp = d->descriptor.format.pixelStride;
    auto *live = d->iteratorBody(Impl::IteratorAccess::Borrow);
    auto releaseLive = qScopeGuard([&] { if (live) Impl::finishRaw(live); });
    for (int y = floorTile(pixels.top()); y <= floorTile(pixels.bottom()); ++y)
        for (int x = floorTile(pixels.left()); x <= floorTile(pixels.right()); ++x) {
            Impl::Selection selected; const quint8 *pending = nullptr;
            { QMutexLocker lock(&d->gate); if (d->closing) return false;
              auto it = d->pages.find({y, x}); if (it != d->pages.end()) {
                  selected = it->second.current;
                  if (live && it->second.writer == live) pending = live->rows[it->second.writerIndex].pixels;
              } }

            const QRect hit = pixels.intersected(QRect(x * 64, y * 64, 64, 64));
            if (pending) {
                for (int row = 0; row < hit.height(); ++row)
                    std::memcpy(bytes + qint64(hit.y() - pixels.y() + row) * stride + (hit.x() - pixels.x()) * bpp,
                        pending + ((hit.y() - y * 64 + row) * 64 + hit.x() - x * 64) * bpp, hit.width() * bpp);
                continue;
            }
            if (!d->readSelection(selected, bytes + qint64(hit.y() - pixels.y()) * stride + (hit.x() - pixels.x()) * bpp,
                hit.translated(-x * 64, -y * 64), stride, d->descriptor.format.defaultPixel, why)) return false;
        }
    return true;
}

KisMementoSP KisNativeResultProtocol::beginHistory(QString *why)
try {
    QMutexLocker historySequence(&d->historyGate);
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) return d->currentHistory;
        if (d->closing || d->active
            || d->historySequence == std::numeric_limits<quint64>::max()) return {};
    }
    auto history = std::allocate_shared<KisNativeHistory>(Alloc<KisNativeHistory>::retained(&d->budget));
    history->before = capture(why); if (!history->before) return {};
    KisMementoSP result = KisMemento::createPageStoreMemento();
    result->saveOldDefaultPixel(reinterpret_cast<const quint8 *>(history->before.d->defaultPixel.constData()),
                               d->descriptor.format.pixelStride);
    // The original history gate sequences public memento calls. Capacity
    // precedes S; weak entries never extend pixel/Selection lifetimes.
    d->history.reserve(d->history.size() + 1);
    {
        QMutexLocker lock(&d->gate);
        if (d->closing || d->active || d->currentHistory) return {};
        history->sequence = ++d->historySequence;
        d->history.push_back(history);
        result->m_nativeHistory = std::move(history); d->currentHistory = result;
    }
    return result;
} catch (const std::bad_alloc &) { error(why, "history admission refused"); return {}; }

bool KisNativeResultProtocol::commitHistory(QString *why)
try {
    QMutexLocker historySequence(&d->historyGate);
    KisMementoSP current;
    {
        QMutexLocker lock(&d->gate); current = d->currentHistory;
        if (!current) return !d->active;
        auto &phase = current->m_nativeHistory->phase;
        if (phase == KisNativeHistory::Phase::Failed || phase == KisNativeHistory::Phase::ClosingAbort) return false;
        phase = KisNativeHistory::Phase::ClosingCommit;
        if (d->active) return false;
    }
    auto after = capture(why); if (!after) return false;
    current->saveNewDefaultPixel(reinterpret_cast<const quint8 *>(after.d->defaultPixel.constData()),
                                d->descriptor.format.pixelStride);
    {
        QMutexLocker lock(&d->gate);
        if (d->active || current != d->currentHistory) return false;
        current->m_nativeHistory->after = std::move(after);
        current->m_nativeHistory->phase = KisNativeHistory::Phase::Sealed;
        d->currentHistory = {};
    }
    return true;
} catch (const std::bad_alloc &) { error(why, "history finalization refused"); return false; }

bool KisNativeResultProtocol::abortHistory(const KisMementoSP &current, QString *why)
{
    QMutexLocker historySequence(&d->historyGate);
    { QMutexLocker lock(&d->gate);
      if (!current || current != d->currentHistory || !current->m_nativeHistory) return false;
      current->m_nativeHistory->phase = KisNativeHistory::Phase::ClosingAbort;
      if (d->active) return false; }
    if (!current->m_nativeHistory || !restore(current->m_nativeHistory->before, why)) return false;
    { QMutexLocker lock(&d->gate); d->currentHistory = {}; }
    auto mutableCurrent = current; mutableCurrent->m_nativeHistory.reset();
    return true;
}

bool KisNativeResultProtocol::restoreHistory(const KisMementoSP &memento, bool before, QString *why)
{
    QMutexLocker historySequence(&d->historyGate);
    if (!memento || !memento->m_nativeHistory || !memento->m_nativeHistory->before.d
        || memento->m_nativeHistory->before.d->owner != d || !commitHistory(why)) return false;
    return restore(before ? memento->m_nativeHistory->before : memento->m_nativeHistory->after, why);
}

bool KisNativeResultProtocol::hasHistory() const
{ QMutexLocker lock(&d->gate); return bool(d->currentHistory); }

void KisNativeResultProtocol::purgeHistory(const KisMementoSP &oldest)
{
    QMutexLocker historySequence(&d->historyGate);
    if (!oldest || !oldest->m_nativeHistory || !oldest->m_nativeHistory->before.d
        || oldest->m_nativeHistory->before.d->owner != d) return;
    const auto through = oldest->m_nativeHistory->sequence;
    bool includesCurrent;
    { QMutexLocker lock(&d->gate); includesCurrent = d->currentHistory && d->currentHistory->m_nativeHistory->sequence <= through; }
    if (includesCurrent && !commitHistory(nullptr)) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("native history purge cannot close active or failed work"));
        return;
    }
    // Release actual historical consumers, outside the selection gate. No
    // resource/page scan or replacement publication is needed for a purge.
    auto end = std::remove_if(d->history.begin(), d->history.end(), [&](auto &weak) {
        auto entry = weak.lock();
        if (!entry) return true;
        if (entry->sequence > through) return false;
        if (d->currentHistory && d->currentHistory->m_nativeHistory == entry) return false;
        entry->before = {}; entry->after = {}; return true;
    });
    d->history.erase(end, d->history.end());
}

KisTileSP KisNativeResultProtocol::oldTile(qint32 col, qint32 row, bool *present, QString *why) const
try {
    Snapshot view;
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) view = d->currentHistory->m_nativeHistory->before;
    }
    if (!view) return {};
    Impl::Selection selected;
    { QMutexLocker lock(&d->gate); selected = d->viewSelection(*view.d, {row, col}); }
    if (present) *present = selected.resource != nullptr;
    if (auto *r = selected.resource) {
        if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return {};
        auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
        // The actual public oldData tile owns a COW reference before the
        // physical pin ends. Future handoff observes that real foreign owner.
        return new KisTile(col, row, r->tile, nullptr);
    }
    // The captured immutable default already owns its real backing.
    // An oldData wrapper consumes that fact instead of recreating storage.
    return new KisTile(col, row, view.d->defaultBacking, nullptr);
} catch (const std::bad_alloc &) { error(why, "oldData public view refused"); return {}; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::captureOldData(QString *why)
{
    { QMutexLocker lock(&d->gate); if (d->currentHistory) return d->currentHistory->m_nativeHistory->before; }
    return capture(why);
}

KisNativeResultProtocol::ReadAccess::~ReadAccess()
{
    if (binding && pixels) binding->releaseRead();
    if (constant) { if (pixels) constant->unblockSwapping(); constant->deref(); }
}
KisNativeResultProtocol::ReadAccess::ReadAccess(ReadAccess &&other) noexcept
{ *this = std::move(other); }
KisNativeResultProtocol::ReadAccess &KisNativeResultProtocol::ReadAccess::operator=(ReadAccess &&other) noexcept
{
    if (this == &other) return *this;
    if (binding && pixels) binding->releaseRead();
    if (constant) { if (pixels) constant->unblockSwapping(); constant->deref(); }
    owner = std::move(other.owner); binding = std::move(other.binding);
    constant = std::exchange(other.constant, nullptr); pixels = std::exchange(other.pixels, nullptr);
    stride = std::exchange(other.stride, 0); selected = other.selected;
    return *this;
}

KisPageVersion KisNativeResultProtocol::Snapshot::versionAt(qint32 col, qint32 row) const
{
    if (!d) return {};
    QMutexLocker lock(&d->owner->gate);
    const auto selection = d->owner->viewSelection(*d, {row, col});
    return {{{1}, {col, row}}, {selection.resource ? selection.generation : d->defaultRevision},
            selection.resource ? 0 : d->defaultRevision};
}

KisNativeResultProtocol::ReadAccess KisNativeResultProtocol::Snapshot::readPage(
    qint32 col, qint32 row, QString *why) const
{
    ReadAccess result;
    if (!d || !KisTileHashTable::supportsCoordinates(col, row)
        || (d->explicitItems && !d->range.contains(QRect(col * 64, row * 64, 64, 64)))) return result;
    Impl::Selection selection;
    { QMutexLocker lock(&d->owner->gate); selection = d->owner->viewSelection(*d, {row, col}); }
    result.owner = d->owner;
    result.stride = 64 * d->owner->descriptor.format.pixelStride;
    result.selected = {{{1}, {col, row}}, {selection.resource ? selection.generation : d->defaultRevision},
                       selection.resource ? 0 : d->defaultRevision};
    if (auto *r = selection.resource) {
        result.binding = r->binding;
        result.pixels = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
    } else {
        result.constant = d->defaultBacking;
        if (result.constant) {
            result.constant->ref();
            if (result.constant->blockSwapping()) result.pixels = result.constant->data();
        }
    }
    if (!result.pixels) error(why, "native selected read pin refused");
    return result;
}

bool KisNativeResultProtocol::beginHistoryMutation(const KisMementoSP &memento, QString *why)
{
    QMutexLocker lock(&d->gate);
    if (!memento || memento != d->currentHistory || d->active || d->closing) {
        error(why, "native history mutation owner unavailable"); return false;
    }
    auto &phase = memento->m_nativeHistory->phase;
    if (phase != KisNativeHistory::Phase::None && phase != KisNativeHistory::Phase::Open) return false;
    phase = KisNativeHistory::Phase::Open; return true;
}

bool KisNativeResultProtocol::checkpointHistoryMutation(const KisMementoSP &memento, QString *why)
{
    QMutexLocker lock(&d->gate);
    if (!memento || memento != d->currentHistory || memento->m_nativeHistory->phase != KisNativeHistory::Phase::Open || d->active || d->closing) {
        error(why, "native history checkpoint owner unavailable"); return false;
    }
    // Each successful original result already crossed the single all-D cut.
    // Checkpoint consumes that fact; it neither republishes nor reprojects it.
    return true;
}

