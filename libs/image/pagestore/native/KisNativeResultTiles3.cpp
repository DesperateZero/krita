/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"

struct KisNativeResultProtocol::Impl::ReadLease final : KisTilePageStoreLease {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<KisCpuResidentBinding> binding;
        KisReplicaHandle expected;
        KisTileData *tile = nullptr;
        bool pinned = false;
        ~ReadLease() override { finish(); }
        KisTileData *tileData() const override { return pinned ? tile : nullptr; }
        bool writable() const override { return false; }
        void markDirty() override {}
        bool finish(TileLeaseRelease * = nullptr) override {
            if (std::exchange(pinned, false)) binding->releaseRead();
            return true;
        }
        bool reusableReadCache() const override { return true; }
        bool readPinned() const override { return pinned; }
        ReadPinResult repinRead() override {
            if (pinned) return ReadPinResult::Ready;
            KisCpuResidentReadStatus status;
            pinned = binding->acquireRead(expected.allocationIdentity(), false, &status) != nullptr;
            if (status == KisCpuResidentReadStatus::Retired) return ReadPinResult::Retired;
            return pinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
        }
        static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(ReadLease)); }
        static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(ReadLease), alignof(ReadLease)); }
    };
struct KisNativeResultProtocol::Impl::DefaultLease final : KisTilePageStoreLease {
    std::shared_ptr<Impl> owner;
    KisTileData *tile = nullptr;
    quint64 revision = 0;
    bool pinned = false;
    ~DefaultLease() override { finish(); if (tile) tile->deref(); }
    KisTileData *tileData() const override { return pinned ? tile : nullptr; }
    bool writable() const override { return false; }
    void markDirty() override {}
    bool finish(TileLeaseRelease * = nullptr) override { if (std::exchange(pinned, false)) tile->unblockSwapping(); return true; }
    bool reusableReadCache() const override { return true; }
    bool readPinned() const override { return pinned; }
    ReadPinResult repinRead() override {
        { QMutexLocker lock(&owner->gate); if (revision != owner->defaultRevision) return ReadPinResult::Stale; }
        if (!pinned) pinned = tile->blockSwapping();
        return pinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
    }
    static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(DefaultLease)); }
    static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(DefaultLease), alignof(DefaultLease)); }
};

struct KisNativeResultProtocol::Impl::WriteLease final : KisTilePageStoreLease {
    std::shared_ptr<Impl> owner;
    Body *body = nullptr;
    KisTileData *tile = nullptr;
    bool writes = true, fixedPinned = false;
    ~WriteLease() override { finish(); }
    KisTileData *tileData() const override { return body || fixedPinned ? tile : nullptr; }
    bool writable() const override { return writes && body && !body->failed; }
    void markDirty() override { if (body && writes) body->rawDirty = true; }
    bool reusableReadCache() const override { return !body; }
    bool readPinned() const override { return fixedPinned; }
    ReadPinResult repinRead() override {
        // A public raw Tile already owns this exact COW backing. The original
        // lease keeps its bridge alive, without rebuilding a read capability.
        if (!fixedPinned) fixedPinned = tile->blockSwapping();
        return fixedPinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
    }
    bool finish(TileLeaseRelease *afterUnlock = nullptr) override {
        if (!body) { if (std::exchange(fixedPinned, false)) tile->unblockSwapping(); return true; }
        auto *b = std::exchange(body, nullptr);
        if (afterUnlock) *afterUnlock = TileLeaseRelease(b, &Impl::finishRaw);
        else Impl::finishRaw(b);
        return true;
    }
    static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(WriteLease)); }
    static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(WriteLease), alignof(WriteLease)); }
};

TileLease KisNativeResultProtocol::Impl::acquireTile(qint32 col, qint32 row, Access access, TileLease *cache)
{
    if (access == Access::Write) {
        Body *body = iteratorBody(IteratorAccess::Borrow);
        const bool shared = body != nullptr;
        auto cancel = qScopeGuard([&] {
            if (!body) return;
            if (shared) { body->failed = true; finishRaw(body); }
            else { body->deferredOwner.reset(); cancelBody(body); }
        });
        if (body && body->failed) return {};
        try {
            std::unique_ptr<WriteLease> result;
            if (cache && *cache) {
                auto *dormant = dynamic_cast<WriteLease *>(cache->get());
                if (dormant && dormant->owner.get() == this && !dormant->body && !dormant->fixedPinned)
                    result.reset(static_cast<WriteLease *>(cache->release()));
            }
            if (!result) result = std::make_unique<WriteLease>();
            result->writes = true;
            if (!body) { body = checkout(); if (!body) return {}; body->deferredOwner = shared_from_this();
                body->raw = true; body->thread = QThread::currentThreadId(); }
            size_t index = body->rows.size();
            {
                QMutexLocker lock(&gate);
                auto it = pages.find({row, col});
                if (it != pages.end() && it->second.writer == body) index = it->second.writerIndex;
            }
            if (index == body->rows.size()) {
                const QRect rect(col * 64, row * 64, 64, 64);
                if (!bindRows(*body, rect, nullptr, true)) return {};
            }
            if (!body->rows[index].target && !prepareTargets(*body, nullptr,
                QRect(col * 64, row * 64, 64, 64), 0, nullptr, nullptr, index, index + 1)) return {};
            result->owner = shared_from_this(); result->body = body; result->tile = body->rows[index].target->tile;
            if (!shared) { QMutexLocker lock(&gate); ++body->borrows; }
            body = nullptr;
            return TileLease(result.release());
        } catch (const std::bad_alloc &) { return {}; }
    }
    if (auto *b = iteratorBody(IteratorAccess::Borrow)) {
        auto release = qScopeGuard([&] { if (b) finishRaw(b); });
        Resource *target = nullptr;
        { QMutexLocker lock(&gate); auto it = pages.find({row, col});
          if (it != pages.end() && it->second.writer == b) target = b->rows[it->second.writerIndex].target; }
        if (target) {
            auto lease = std::make_unique<WriteLease>();
            lease->owner = shared_from_this(); lease->body = b; lease->tile = target->tile; lease->writes = false;
            b = nullptr;
            return TileLease(lease.release());
        }
    }
    std::shared_ptr<KisCpuResidentBinding> binding;
    KisReplicaHandle expected;
    KisTileData *tile = nullptr;
    {
        QMutexLocker lock(&gate);
        auto it = pages.find({row, col});
        if (it != pages.end() && it->second.current.resource) {
            auto *r = it->second.current.resource;
            binding = r->binding; expected = r->handle; tile = r->tile;
        }
    }
    if (!binding) {
        auto result = std::make_unique<DefaultLease>(); result->owner = shared_from_this();
        { QMutexLocker lock(&gate);
          result->tile = defaultBacking; result->tile->ref(); result->revision = defaultRevision; }
        return result->repinRead() == KisTilePageStoreLease::ReadPinResult::Ready ? TileLease(result.release()) : TileLease{};
    }
    std::unique_ptr<ReadLease> result;
    if (cache && *cache) {
        auto *old = dynamic_cast<ReadLease *>(cache->get());
        if (old && old->binding == binding && old->expected == expected)
            result.reset(static_cast<ReadLease *>(cache->release()));
    }
    if (!result) result = std::make_unique<ReadLease>();
    result->owner = shared_from_this(); result->binding = std::move(binding); result->expected = expected; result->tile = tile;
    return result->repinRead() == KisTilePageStoreLease::ReadPinResult::Ready ? TileLease(result.release()) : TileLease{};
}

std::shared_ptr<const KisPageStoreIteratorReadScope> KisNativeResultProtocol::captureIteratorScope(
    bool writable, std::shared_ptr<const KisPageStoreIteratorReadScope> existing, QString *why)
try {
    if (existing) {
        if (existing->m_nativeIdentity == d.get() && existing->m_beforeOnly == writable) return existing;
        error(why, "iterator selection owner or mode mismatch"); return d->failedReadScope;
    }
    auto result = std::allocate_shared<KisPageStoreIteratorReadScope>(Alloc<KisPageStoreIteratorReadScope>::retained(&d->budget));
    result->m_nativeIdentity = d.get(); result->m_beforeOnly = writable;
    if (!writable && d->iteratorBody()) { result->m_nativeLiveWriter = true; return result; }
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) result->m_nativeBefore = d->currentHistory->m_nativeHistory->before;
    }
    if (!writable) {
        result->m_nativeCurrent = capture(why);
        if (!result->m_nativeCurrent) return d->failedReadScope;
        if (!result->m_nativeBefore) result->m_nativeBefore = result->m_nativeCurrent;
    }
    return result;
} catch (const std::bad_alloc &) { error(why, "iterator selection admission refused"); return d->failedReadScope; }

void *KisNativeResultProtocol::IteratorSession::operator new(size_t n) { return kisAllocatePageProcessStorage(n, alignof(IteratorSession)); }
void KisNativeResultProtocol::IteratorSession::operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(IteratorSession), alignof(IteratorSession)); }
KisNativeResultProtocol::IteratorSession::~IteratorSession() { if (body) owner->finishIterator(std::exchange(body, nullptr), false); }
bool KisNativeResultProtocol::IteratorSession::finish() { return !body || owner->finishIterator(std::exchange(body, nullptr), true); }
bool KisNativeResultProtocol::IteratorSession::isActive() const
{
    if (!body) return false;
    QMutexLocker lock(&owner->gate);
    return !body->failed && !owner->closing;
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope> KisNativeResultProtocol::beginIterator(QString *why)
try {
    auto session = std::make_unique<IteratorSession>(); session->owner = d;
    auto *b = d->iteratorBody(Impl::IteratorAccess::Client);
    session->body = b;
    if (b && b->failed) { error(why, "original iterator has failed"); return {}; }
    if (!b) {
        b = d->checkout(); if (!b) return {};
        b->iterator = true; b->thread = QThread::currentThreadId(); b->deferredOwner = d;
        QMutexLocker lock(&d->gate);
        b->clients = 1;
        b->iteratorNext = d->iteratorHead.load(std::memory_order_relaxed);
        d->iteratorHead.store(b, std::memory_order_release);
    }
    session->body = b;
    return KisTiledDataManagerIteratorWriteScope::fromNative(std::move(session));
} catch (const std::bad_alloc &) { error(why, "iterator storage refused"); return {}; }

KisTileSP KisNativeResultProtocol::writableTile(qint32 col, qint32 row, QString *why)
try {
    if (!KisTileHashTable::supportsCoordinates(col, row)) return {};
    auto &table = KisNativeResultAdapter::table(d->adapter);
    const auto intentFor = [&](KisTileData *initial) {
        KisTileSP intent(new KisTile(col, row, initial, nullptr));
        auto lease = std::make_unique<Impl::WriteLease>(); lease->owner = d; lease->tile = initial;
        intent->setPageStoreBridge(d.get(), false);
        intent->m_pageStoreLeases[KisTile::ReadLease] = std::move(lease);
        intent->m_pageStoreNativeReadReady.storeRelease(1);
        return intent;
    };
    if (auto *body = d->iteratorBody(Impl::IteratorAccess::Borrow)) {
        auto release = qScopeGuard([&] { Impl::finishRaw(body); });
        if (body->failed) return {};
        size_t index = body->rows.size();
        {
            QMutexLocker lock(&d->gate);
            auto it = d->pages.find({row, col});
            if (it != d->pages.end() && it->second.writer == body) index = it->second.writerIndex;
        }
        if (index != body->rows.size() && body->rows[index].adapter)
            return body->rows[index].adapter.tile();
        if (!table.getExistingTileForPreparedUpdate(col, row)) {
            if (index == body->rows.size() && !d->bindRows(*body, QRect(col * 64, row * 64, 64, 64), why, true)) return {};
            auto &entry = body->rows[index];
            entry.adapter = table.prepareTileUpdate(col, row);
            auto intent = entry.adapter.tile();
            if (!intent) return {};
            intent->setPageStoreBridge(d.get(), false);
            body->rawDirty = true;
            // The same original row owns this wrapper until the all-D cut.
            // Decoding and iterators cannot recursively publish cold presence.
            return intent;
        }
    }
    if (auto tile = table.getExistingTileForPreparedUpdate(col, row)) {
        QMutexLocker gate(&tile->m_swapBarrierLock);
        if (!tile->m_lockCounter || tile->m_pageStoreWriteLockCount) return tile;
        // A new writer acquisition does not join an unrelated old reader's
        // nested tile lock. Direct read/write nesting on the SAME Tile keeps
        // its existing public last-unlock semantics.
        return intentFor(tile->m_tileData);
    }
    // Public writable acquisition establishes explicit presence, even before
    // pixels are exposed. Keep that accepted wrapper stable for reacquisition;
    // the iterator's later writable result still uses its original Body.
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return {};
    if (!d->bindRows(*p.body, QRect(col * 64, row * 64, 64, 64), why)) return {};
    auto &entry = p.body->rows.front();
    if (!entry.before.resource) {
        { QMutexLocker lock(&d->gate);
          if (entry.page->nextGeneration == std::numeric_limits<quint64>::max()) return {};
          entry.after.generation = entry.page->nextGeneration++; }
        // Writable presence consumes the already-owned default backing. It
        // needs no pixel loop, producer pin or second default materialization.
        auto source = d->provider->captureCompletedTileSource(d->descriptor, d->defaultBacking, why);
        if (!source) return {};
        entry.target = d->fresh(*entry.page, {entry.page->key, {entry.after.generation}}, nullptr, 0, nullptr,
                             QRect(0, 0, 64, 64), why, source);
        if (!entry.target) return {};
        entry.ownsTarget = entry.changed = true; entry.after.resource = entry.target;
    }
    p.body->ready = true;
    if (!accept(p, why)) return {};
    return table.getExistingTileForPreparedUpdate(col, row);
} catch (const std::bad_alloc &) { error(why, "writable tile intent refused"); return {}; }

bool KisNativeResultProtocol::pixelOperation(const QVector<QRect> &rects,
    const std::function<bool(KisPixelWriteCursor *)> &operation, const KisMementoSP &history, QString *why)
try {
    if (!operation) return false;
    if (history) return historyPixelOperation(rects, operation, history, why);
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory && d->currentHistory->m_nativeHistory->phase != KisNativeHistory::Phase::None) {
            error(why, "pixel operation has no open native history owner"); return false;
        }
    }
    QRegion pixels, pages;
    for (const auto &rect : rects) if (!rect.isEmpty()) {
        pixels += rect;
        pages += QRect(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                       QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    }
    if (pixels.isEmpty()) return true;
    quint64 pageCount = 0;
    for (const auto &part : pages) pageCount += quint64(part.width()) * quint64(part.height());
    KisPageStoreDiagnosticTimer phase(nullptr, KisPageStoreDiagnosticPhase::PixelOperationPreflight, pageCount);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare, pageCount);
    Prepared p;
    auto *body = d->iteratorBody(Impl::IteratorAccess::Client);
    if (body && body->failed) {
        d->finishIterator(body, false);
        error(why, "original producer has failed"); return false;
    }
    const bool joined = body != nullptr;
    if (!joined) { p.owner = d; p.body = d->checkout(); body = p.body; if (!body) return false; }
    bool succeeded = false;
    auto releaseJoined = qScopeGuard([&] {
        if (joined && body) d->finishIterator(body, succeeded);
    });
    QRegion missing = pages;
    if (joined) {
        // Reuse the original producer's claims and physical pointers. Only
        // missing page runs append rows; no second operation or translation.
        for (const auto &part : pages) for (int y = part.top(); y <= part.bottom(); ++y)
            for (int x = part.left(); x <= part.right(); ++x) {
                bool owned;
                { QMutexLocker lock(&d->gate); auto it = d->pages.find({y, x}); owned = it != d->pages.end() && it->second.writer == body; }
                if (owned) missing -= QRect(x, y, 1, 1);
            }
    }
    for (const auto &part : missing) {
        if (!KisTileHashTable::supportsCoordinates(part.left(), part.top()) ||
            !KisTileHashTable::supportsCoordinates(part.right(), part.bottom())) return false;
        if (!d->bindRows(*body, QRect(part.x() * 64, part.y() * 64, part.width() * 64, part.height() * 64), why, true)) return false;
    }
    class Cursor final : public KisPixelWriteCursor {
    public:
        Impl &owner; Body &body; const QRegion &declared; QString *why;
        Body::Row *row = nullptr; qint32 px = 0, py = 0; bool failed = false;
        Cursor(Impl &o, Body &b, const QRegion &r, QString *w) : owner(o), body(b), declared(r), why(w) {}
        void moveTo(qint32 x, qint32 y) override {
            px = x; py = y; row = nullptr;
            if (!declared.contains(QPoint(x, y))) { failed = true; return; }
            QMutexLocker lock(&owner.gate);
            auto it = owner.pages.find({floorTile(y), floorTile(x)});
            if (it == owner.pages.end() || it->second.writer != &body) { failed = true; return; }
            row = &body.rows[it->second.writerIndex];
        }
        quint8 *rawData() override {
            if (!row || failed || body.failed) return nullptr;
            if (!row->target) {
                const auto index = size_t(row - body.rows.data());
                const QRect rect(row->page->key.page.column * 64, row->page->key.page.row * 64, 64, 64);
                if (!owner.prepareTargets(body, nullptr, rect, 0, nullptr, why, index, index + 1)) { failed = true; return nullptr; }
            }
            return row->pixels + (py - row->page->key.page.row * 64) * 64 * owner.descriptor.format.pixelStride
                + (px - row->page->key.page.column * 64) * owner.descriptor.format.pixelStride;
        }
        qint32 numContiguousColumns(qint32 x) const override { return 64 - (x - floorTile(x) * 64); }
        qint32 numContiguousRows(qint32 y) const override { return 64 - (y - floorTile(y) * 64); }
        qint32 rowStride(qint32, qint32) const override { return 64 * owner.descriptor.format.pixelStride; }
        qint32 x() const override { return px; }
        qint32 y() const override { return py; }
    } cursor(*d, *body, pixels, why);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationBody, 1);
    if (!operation(&cursor) || cursor.failed) { error(why, "native pixel producer failed"); return false; }
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationFinish, pageCount);
    if (joined) {
        body->rawDirty = true;
        succeeded = true;
        return d->finishIterator(std::exchange(body, nullptr), true);
    }
    d->finishProducer(*body); succeeded = accept(p, why);
    if (succeeded) body = nullptr;
    return succeeded;
} catch (const std::bad_alloc &) { error(why, "native pixel operation storage refused"); return false; }

bool KisNativeResultProtocol::historyPixelOperation(const QVector<QRect> &rects,
    const std::function<bool(KisPixelWriteCursor *)> &operation, const KisMementoSP &memento, QString *why)
try {
    auto owner = d;
    std::shared_ptr<KisNativeHistory> history;
    {
        QMutexLocker lock(&owner->gate);
        if (!memento || memento != owner->currentHistory || owner->closing
            || !memento->m_nativeHistory || memento->m_nativeHistory->phase != KisNativeHistory::Phase::Open) {
            error(why, "history producer has no open owner"); return false;
        }
        history = memento->m_nativeHistory; ++history->workers;
    }
    Prepared freshRows;
    Vec<Body::Row *> rows{Alloc<Body::Row *>(&owner->budget)};
    bool entered = false, succeeded = false;
    const auto release = qScopeGuard([&] {
        // A callback keeps permission/identity, not a RAM pin between jobs.
        // The same reserved target is repinned by the next real consumer.
        for (auto *row : rows) if (row->pixels) {
            row->writable.unpin(); row->pixels = nullptr;
        }
        {
            QMutexLocker lock(&owner->gate);
            for (auto *row : rows) row->producerBusy = false;
            if (entered && !succeeded && history->phase != KisNativeHistory::Phase::ClosingAbort)
                history->phase = KisNativeHistory::Phase::Failed;
        }
        if (freshRows.body) {
            owner->cancelBody(std::exchange(freshRows.body, nullptr)); freshRows.owner.reset();
        }
        bool closeLast;
        {
            QMutexLocker lock(&owner->gate);
            Q_ASSERT(history->workers); --history->workers;
            closeLast = owner->closing && !history->workers;
        }
        if (closeLast) { owner->cancelHistorySegment(history); owner->finishClosing(); }
    });
    QRegion pixels, pages;
    for (const auto &rect : rects) if (!rect.isEmpty()) {
        pixels += rect;
        pages += QRect(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                       QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    }
    if (pixels.isEmpty()) { succeeded = true; return true; }
    quint64 count = 0;
    for (const auto &part : pages) {
        if (!KisTileHashTable::supportsCoordinates(part.left(), part.top())
            || !KisTileHashTable::supportsCoordinates(part.right(), part.bottom())) return false;
        count += quint64(part.width()) * quint64(part.height());
    }
    if (count > std::numeric_limits<quint32>::max()) return false;
    KisPageStoreDiagnosticTimer phase(nullptr, KisPageStoreDiagnosticPhase::PixelOperationPreflight, count);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare, count);
    rows.reserve(size_t(count));
    QRegion missing = pages;
    {
        QMutexLocker lock(&owner->gate);
        for (const auto &part : pages) for (int y = part.top(); y <= part.bottom(); ++y)
            for (int x = part.left(); x <= part.right(); ++x) {
                auto it = owner->pages.find({y, x});
                if (it == owner->pages.end() || !it->second.writer) continue;
                auto *body = it->second.writer;
                if (body->history != history) { error(why, "history range has a foreign writer"); return false; }
                auto &row = body->rows[it->second.writerIndex];
                if (row.producerBusy) { error(why, "history range already has a producer"); return false; }
                row.producerBusy = true; rows.push_back(&row);
            }
    }
    for (const auto *row : rows) missing -= QRect(row->page->key.page.column, row->page->key.page.row, 1, 1);
    if (!missing.isEmpty()) {
        freshRows.owner = owner; freshRows.body = owner->checkout();
        if (!freshRows.body) return false;
        auto *body = freshRows.body;
        for (const auto &part : missing)
            if (!owner->bindRows(*body, QRect(part.x() * 64, part.y() * 64, part.width() * 64, part.height() * 64), why, true)) return false;
        {
            QMutexLocker lock(&owner->gate);
            for (auto &row : body->rows) { row.producerBusy = true; rows.push_back(&row); }
            body->history = history;
            body->next = history->pending; history->pending = body; ++history->pendingCount;
        }
        freshRows.body = nullptr; freshRows.owner.reset();
    }
    // These pointers are a temporary admitted range, not a second result or
    // page index. Original row storage cannot move while any worker is live.
    std::sort(rows.begin(), rows.end(), [](const auto *a, const auto *b) {
        const auto &x = a->page->key.page; const auto &y = b->page->key.page;
        return Key{x.row, x.column} < Key{y.row, y.column};
    });
    class Cursor final : public KisPixelWriteCursor {
    public:
        Impl &owner; Vec<Body::Row *> &rows; const QRegion &declared;
        const std::shared_ptr<KisNativeHistory> &history; QString *why;
        Body::Row *row = nullptr; qint32 px = 0, py = 0; bool failed = false;
        Cursor(Impl &o, Vec<Body::Row *> &r, const QRegion &p,
               const std::shared_ptr<KisNativeHistory> &h, QString *w)
            : owner(o), rows(r), declared(p), history(h), why(w) {}
        void moveTo(qint32 x, qint32 y) override {
            px = x; py = y; row = nullptr;
            if (!declared.contains(QPoint(x, y))) { failed = true; return; }
            const Key key{floorTile(y), floorTile(x)};
            auto it = std::lower_bound(rows.begin(), rows.end(), key, [](const auto *r, Key k) {
                return Key{r->page->key.page.row, r->page->key.page.column} < k;
            });
            if (it != rows.end() && Key{(*it)->page->key.page.row, (*it)->page->key.page.column} == key) row = *it;
            else failed = true;
        }
        quint8 *rawData() override {
            if (!row || failed) return nullptr;
            const auto phase = history->phase.load(std::memory_order_acquire);
            if (phase == KisNativeHistory::Phase::ClosingAbort || phase == KisNativeHistory::Phase::Failed) {
                failed = true; return nullptr;
            }
            if (!row->target) {
                auto *body = row->page->writer;
                const auto index = row->page->writerIndex;
                const QRect rect(row->page->key.page.column * 64, row->page->key.page.row * 64, 64, 64);
                if (!owner.prepareTargets(*body, nullptr, rect, 0, nullptr, why, index, index + 1)) { failed = true; return nullptr; }
            } else if (!row->pixels) {
                if (!row->writable.isValid()) row->writable = KisCpuWriteBindingReservation::acquire(
                    row->target->binding, row->target->handle.allocationIdentity());
                row->pixels = static_cast<quint8 *>(row->writable.pinResident());
                if (!row->pixels) row->pixels = static_cast<quint8 *>(row->writable.materialize());
                if (!row->pixels) { failed = true; return nullptr; }
            }
            return row->pixels + ((py - row->page->key.page.row * 64) * 64
                + px - row->page->key.page.column * 64) * owner.descriptor.format.pixelStride;
        }
        qint32 numContiguousColumns(qint32 x) const override { return 64 - (x - floorTile(x) * 64); }
        qint32 numContiguousRows(qint32 y) const override { return 64 - (y - floorTile(y) * 64); }
        qint32 rowStride(qint32, qint32) const override { return 64 * owner.descriptor.format.pixelStride; }
        qint32 x() const override { return px; }
        qint32 y() const override { return py; }
    } cursor(*owner, rows, pixels, history, why);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationBody, 1);
    entered = true;
    if (!operation(&cursor) || cursor.failed
        || history->phase == KisNativeHistory::Phase::ClosingAbort
        || history->phase == KisNativeHistory::Phase::Failed) {
        error(why, "history pixel producer failed"); return false;
    }
    succeeded = true;
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationFinish, count);
    return true;
} catch (const std::bad_alloc &) { error(why, "history original-row admission refused"); return false; }

KisTileSP KisNativeResultProtocol::selectReadTile(KisTileSP candidate, bool *present, QString *why)
try {
    if (!candidate || !present) return {};
    if (d->iteratorBody()) { candidate->setPageStoreBridge(d.get(), false); return candidate; }
    const bool indexed = *present;
    TileLease previous;
    QMutexLocker tileGate(&candidate->m_swapBarrierLock);
    if (candidate->m_pageStoreWriteLockCount) return candidate;
    auto &cache = candidate->m_pageStoreLeases[KisTile::ReadLease];
    auto selected = d->acquireTile(candidate->col(), candidate->row(), KisTilePageStoreBridge::Access::ReadSelection,
        candidate->m_lockCounter ? nullptr : &cache);
    if (!selected || !selected->tileData()) { error(why, "native current tile selection refused"); return {}; }
    const bool selectedDefault = dynamic_cast<Impl::DefaultLease *>(selected.get()) != nullptr;
    auto *data = selected->tileData();
    KisTileSP result = candidate;
    if (candidate->m_lockCounter && data != candidate->m_tileData) {
        result = new KisTile(candidate->col(), candidate->row(), data, nullptr);
        selected->finish();
        result->m_pageStoreNativeReadReady.storeRelease(1);
    } else if (!candidate->m_lockCounter) {
        result->replacePageStoreReadCacheLocked(data);
        selected->finish(); previous = std::move(cache);
        if (indexed) cache = std::move(selected);
        else result->m_pageStoreBridge.storeRelease(nullptr);
        result->m_pageStoreNativeReadReady.storeRelease(1);
    } else selected->finish();
    *present = !selectedDefault;
    return result;
} catch (const std::bad_alloc &) { error(why, "native current tile selection storage refused"); return {}; }

KisTiledDataManagerIteratorWriteScope::~KisTiledDataManagerIteratorWriteScope() = default;

void *KisTiledDataManagerIteratorWriteScope::operator new(size_t bytes)
{
    return KisPageProcessStorageObject::operator new(bytes);
}

void KisTiledDataManagerIteratorWriteScope::operator delete(void *data) noexcept
{
    KisPageProcessStorageObject::operator delete(data);
}

std::unique_ptr<KisTiledDataManagerIteratorWriteScope> KisTiledDataManagerIteratorWriteScope::fromNative(
    std::unique_ptr<KisNativeResultProtocol::IteratorSession> session)
{
    std::unique_ptr<KisTiledDataManagerIteratorWriteScope> result(new KisTiledDataManagerIteratorWriteScope);
    result->m_session = std::move(session);
    return result;
}

bool KisTiledDataManagerIteratorWriteScope::finish()
{
    return m_session && m_session->finish();
}

bool KisTiledDataManagerIteratorWriteScope::isActive() const
{
    return m_session && m_session->isActive();
}
