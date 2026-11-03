/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"

bool KisNativeResultProtocol::alias(const Snapshot &s, const QRect &source, const QPoint &destination, QString *why)
try {
    if (!s.d || s.d->owner != d || source.isEmpty() || source.x() % 64 || source.y() % 64
        || source.width() % 64 || source.height() % 64 || destination.x() % 64 || destination.y() % 64
        || (s.d->explicitItems && !s.d->range.contains(source))) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    const QRect target(destination, source.size());
    auto &b = *p.body; if (!d->bindRows(b, target, why)) return false;
    {
        QMutexLocker lock(&d->gate);
        for (auto &row : b.rows) {
            const Key key{row.page->key.page.row + (source.y() - destination.y()) / 64,
                          row.page->key.page.column + (source.x() - destination.x()) / 64};
            const auto selected = d->viewSelection(*s.d, key);
            if (selected.resource == row.before.resource
                && (selected.resource || s.d->defaultPixel == d->descriptor.format.defaultPixel)) continue;
            if (row.page->nextGeneration == std::numeric_limits<quint64>::max()) return false;
            row.after = {selected.resource, row.page->nextGeneration++}; row.target = selected.resource; row.changed = true;
        }
    }
    Vec<quint8> constant{Alloc<quint8>(&d->budget)};
    Impl::Resource *virtualDefault = nullptr;
    for (auto &row : b.rows) if (row.changed && !row.after.resource) {
        if (s.d->defaultPixel != d->descriptor.format.defaultPixel) {
            if (!virtualDefault) {
                const auto bpp = d->descriptor.format.pixelStride;
                constant.resize(size_t(d->descriptor.minimumByteSize()));
                for (int i = 0; i < 4096; ++i) std::memcpy(constant.data() + i * bpp, s.d->defaultPixel.constData(), bpp);
                virtualDefault = d->fresh(*row.page, {row.page->key, {row.after.generation}}, constant.data(), 64 * bpp,
                                           nullptr, QRect(0, 0, 64, 64), why);
                if (!virtualDefault) return false;
                row.ownsTarget = true;
            }
            row.target = row.after.resource = virtualDefault;
        } else row.adapter = KisNativeResultAdapter::table(d->adapter).prepareTileUpdate(
            row.page->key.page.column, row.page->key.page.row, KisTileHashTable::Update::Absent, &row.location);
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "alias preparation refused"); return false; }

bool KisNativeResultProtocol::remove(const QRect &pixels, QString *why)
try {
    if (pixels.isEmpty() || pixels.x() % 64 || pixels.y() % 64 || pixels.width() % 64 || pixels.height() % 64) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; if (!d->bindRows(b, pixels, why)) return false;
    for (auto &row : b.rows) if (row.before.resource) {
        row.changed = true; row.after = {};
        row.adapter = KisNativeResultAdapter::table(d->adapter).prepareTileUpdate(
            row.page->key.page.column, row.page->key.page.row, KisTileHashTable::Update::Absent, &row.location);
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "remove preparation refused"); return false; }

bool KisNativeResultProtocol::Impl::prepareDefault(Body &b, const QByteArray &pixel, QString *why)
{
    if (pixel.size() != descriptor.format.pixelStride) return false;
    {
        QMutexLocker lock(&gate);
        if (active != 1 || closing || defaultRevision == std::numeric_limits<quint64>::max()) {
            error(why, "default change conflicts with active result work"); return false;
        }
        defaultWriter = &b;
    }
    b.defaultPixel = pixel;
    b.defaultTile = KisTileDataStore::instance()->createDefaultTileData(pixel.size(), reinterpret_cast<const quint8 *>(pixel.constData()));
    if (!b.defaultTile) { error(why, "real default storage refused"); return false; }
    b.defaultTile->ref(); return true;
}

bool KisNativeResultProtocol::setDefaultPixel(const QByteArray &pixel, QString *why)
try {
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    { QMutexLocker lock(&d->gate); if (pixel == d->descriptor.format.defaultPixel) return true; }
    if (!d->prepareDefault(*p.body, pixel, why)) return false;
    p.body->ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "default metadata refused"); return false; }

std::unique_ptr<KisNativeResultProtocol> KisNativeResultProtocol::cloneTo(KisTiledDataManager &adapter, QString *why)
try {
    auto view = capture(why); if (!view) return {};
    Vec<Key> keys{Alloc<Key>(&d->budget)};
    size_t count;
    { QMutexLocker lock(&d->gate); count = d->pages.size(); }
    keys.reserve(count);
    {
        QMutexLocker lock(&d->gate);
        if (count != d->pages.size()) { error(why, "clone page index changed; retry capture"); return {}; }
        for (const auto &p : d->pages) if (d->selection(p.second, view.d->epoch).resource) keys.push_back(p.first);
    }
    auto branchDescriptor = d->descriptor; branchDescriptor.format.defaultPixel = view.d->defaultPixel;
    auto result = std::make_unique<KisNativeResultProtocol>(adapter, branchDescriptor);
    Prepared p; p.owner = result->d; p.body = result->d->checkout(); if (!p.body) return {};
    auto &b = *p.body;
    b.rows.resize(keys.size()); b.moves.reserve(2 * keys.size());
    using Imported = std::map<Impl::Resource *, Impl::Resource *, std::less<Impl::Resource *>,
        Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>>;
    Imported imported(std::less<Impl::Resource *>{}, Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>(&result->d->budget));
    // This branch is private until return. Alias topology stays physical once;
    // all selected pages share one visibility cut, not P little transactions.
    for (size_t index = 0; index < keys.size(); ++index) {
        const auto key = keys[index];
        auto &page = result->d->pages.try_emplace(key).first->second;
        page.key = {{1}, {key.second, key.first}}; page.writer = &b;
        auto &row = b.rows[index]; row.page = &page;
        Impl::Selection selected;
        { QMutexLocker lock(&d->gate); selected = d->viewSelection(*view.d, key); }
        auto *r = selected.resource;
        auto node = imported.try_emplace(r, nullptr).first;
        const auto gen = page.nextGeneration++;
        if (!node->second) {
            if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return {};
            auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
            auto source = result->d->provider->captureCompletedTileSource(d->descriptor, r->tile, why);
            if (!source) return {};
            row.target = result->d->fresh(page, {page.key, {gen}}, nullptr, 0, nullptr,
                                          QRect(0, 0, 64, 64), why, source);
            if (!row.target) return {};
            row.ownsTarget = true; node->second = row.target;
        } else row.target = node->second;
        row.changed = true; row.after = {row.target, gen};
    }
    b.ready = true;
    if (!result->accept(p, why)) return {};
    return result;
} catch (const std::bad_alloc &) { error(why, "branch native alias capacity refused"); return {}; }

bool KisNativeResultProtocol::clear(QString *why)
try {
    {
        QMutexLocker bodies(&d->bodyGate);
        QMutexLocker lock(&d->gate);
        for (auto &item : d->pages) if (auto *writer = item.second.writer)
            if ((!writer->iterator && !writer->raw) || writer->thread != QThread::currentThreadId()) {
                error(why, "clear conflicts with explicit or foreign result work"); return false;
            }
        for (auto &body : d->bodies) if (body.iterator && body.clients) {
            if (body.thread != QThread::currentThreadId()) { error(why, "clear conflicts with foreign iterator"); return false; }
        }
        for (auto &body : d->bodies) if (body.iterator && body.clients) body.failed = body.revoked = true;
        for (auto &item : d->pages) if (auto *writer = item.second.writer) {
            writer->failed = writer->revoked = true; item.second.writer = nullptr;
        }
    }
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &row = b.rows[n++]; row.page = &item.second;
            if (row.page->writer) return false;
            row.page->writer = &b; row.before = row.page->current;
            row.changed = row.before.resource != nullptr; row.after = {};
        }
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "clear preparation refused"); return false; }

bool KisNativeResultProtocol::mutate(const QRect &rect, const PageProducer &produce,
    bool discardCovered, const QByteArray &sparseDefault, QString *why)
try {
    if (rect.isEmpty()) return true;
    if (d->iteratorBody()) {
        // A live public writable capability is the original producer. Fill,
        // clear and planar kernels consume its rows just like pixelOperation;
        // they never open a conflicting second native result.
        return pixelOperation({rect}, [&](KisPixelWriteCursor *cursor) {
            for (int y = floorTile(rect.top()); y <= floorTile(rect.bottom()); ++y)
                for (int x = floorTile(rect.left()); x <= floorTile(rect.right()); ++x) {
                    const QRect hit = rect.intersected(QRect(x * 64, y * 64, 64, 64));
                    cursor->moveTo(hit.left(), hit.top());
                    auto *pixel = cursor->rawData(); if (!pixel) return false;
                    const auto stride = cursor->rowStride(hit.left(), hit.top());
                    const QRect local = hit.translated(-x * 64, -y * 64);
                    auto *page = pixel - local.y() * stride - local.x() * d->descriptor.format.pixelStride;
                    if (!produce(x, y, page, stride, local)) return false;
                }
            return true;
        }, {}, why);
    }
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    if (!d->bindRows(b, rect, why)) return false;
    const bool removeCovered = !sparseDefault.isEmpty() && sparseDefault == d->descriptor.format.defaultPixel;
    for (auto &row : b.rows) {
        const bool full = row.local == QRect(0, 0, 64, 64);
        row.remove = full && removeCovered; row.discard = full && discardCovered;
    }
    if (!d->prepareTargets(b, nullptr, rect, 0, nullptr, why)) return false;
    for (auto &row : b.rows) if (row.pixels && !produce(row.page->key.page.column, row.page->key.page.row,
        row.pixels, 64 * d->descriptor.format.pixelStride, row.local)) return false;
    d->finishProducer(b);
    return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "native pixel operation preparation refused"); return false; }

bool KisNativeResultProtocol::fill(const QRect &rect, const QByteArray &pixel, QString *why)
{
    if (pixel.size() != d->descriptor.format.pixelStride) return false;
    return mutate(rect, [&](qint32, qint32, quint8 *data, quint32 stride, const QRect &local) {
        for (int y = local.top(); y <= local.bottom(); ++y)
            for (int x = local.left(); x <= local.right(); ++x)
                std::memcpy(data + y * stride + x * pixel.size(), pixel.constData(), pixel.size());
        return true;
    }, true, pixel, why);
}

bool KisNativeResultProtocol::copyFrom(const Snapshot &source, const QRect &rect, QString *why)
try {
    if (!source.d || source.d->owner->descriptor.format.pixelStride != d->descriptor.format.pixelStride) return false;
    if (rect.isEmpty()) return true;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; if (!d->bindRows(b, rect, why)) return false;
    const auto bpp = d->descriptor.format.pixelStride;
    using Imported = std::map<Impl::Resource *, Impl::Resource *, std::less<Impl::Resource *>,
        Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>>;
    Imported imported(std::less<Impl::Resource *>{}, Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>(&d->budget));
    for (auto &row : b.rows) {
        if (row.local != QRect(0, 0, 64, 64)) continue;
        Impl::Selection selected;
        { QMutexLocker lock(&source.d->owner->gate);
          selected = source.d->owner->viewSelection(*source.d, {row.page->key.page.row, row.page->key.page.column}); }
        if (!selected.resource) { row.remove = source.d->defaultPixel == d->descriptor.format.defaultPixel; row.discard = !row.remove; continue; }
        if (source.d->owner == d && selected.resource == row.before.resource) { row.selectionOnly = true; continue; }
        { QMutexLocker lock(&d->gate);
          if (row.page->nextGeneration == std::numeric_limits<quint64>::max()) return false;
          row.after.generation = row.page->nextGeneration++; }
        auto *r = selected.resource;
        if (source.d->owner == d) row.after.resource = r;
        else {
            auto node = imported.try_emplace(r, nullptr).first;
            if (!node->second) {
                if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return false;
                auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
                auto sourceTile = d->provider->captureCompletedTileSource(source.d->owner->descriptor, r->tile, why);
                if (!sourceTile) return false;
                row.target = d->fresh(*row.page, {row.page->key, {row.after.generation}}, nullptr, 0, nullptr,
                    QRect(0, 0, 64, 64), why, sourceTile);
                if (!row.target) return false;
                row.ownsTarget = true; node->second = row.target;
            }
            row.after.resource = node->second;
        }
        row.changed = row.selectionOnly = true;
    }
    if (!d->prepareTargets(b, nullptr, rect, 0, nullptr, why)) return false;
    for (auto &row : b.rows) if (row.pixels) {
        const auto local = row.local;
        const QRect hit = local.translated(row.page->key.page.column * 64, row.page->key.page.row * 64);
        if (!source.read(row.pixels + local.y() * 64 * bpp + local.x() * bpp, hit, 64 * bpp, why)) return false;
    }
    d->finishProducer(b); return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "copy admission refused"); return false; }

bool KisNativeResultProtocol::trim(const QRect &kept, QString *why)
try {
    if (kept.isEmpty()) return clear(why);
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &row = b.rows[n++]; row.page = &item.second;
            if (row.page->writer) return false;
            row.page->writer = &b; row.before = row.page->current;
            row.local = QRect(0, 0, 64, 64);
            const QRect tile(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
            row.selectionOnly = !row.before.resource || kept.contains(tile);
            row.remove = !row.selectionOnly && !kept.intersects(tile);
        }
    }
    // Original rows enumerate actual page slots; never expand a sparse image
    // into its bounding rectangle just to perform a trim.
    if (!d->prepareTargets(b, nullptr, kept, 0, nullptr, why)) return false;
    const auto bpp = d->descriptor.format.pixelStride;
    for (auto &row : b.rows) if (row.pixels) {
        const int ox = row.page->key.page.column * 64, oy = row.page->key.page.row * 64;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            if (!kept.contains(ox + x, oy + y)) std::memcpy(row.pixels + (y * 64 + x) * bpp,
                d->descriptor.format.defaultPixel.constData(), bpp);
    }
    d->finishProducer(b); return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "trim admission refused"); return false; }

bool KisNativeResultProtocol::purge(const QRect &area, QString *why)
try {
    if (area.isEmpty()) return true;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.reserve(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        for (auto &item : d->pages) {
            auto &page = item.second;
            if (!page.current.resource || !area.intersects(QRect(item.first.second * 64, item.first.first * 64, 64, 64))) continue;
            if (page.writer) { error(why, "purge conflicts with writer"); return false; }
            b.rows.emplace_back(); auto &row = b.rows.back();
            row.page = &page; row.before = page.current; page.writer = &b;
        }
    }
    const int bpp = d->descriptor.format.pixelStride;
    for (auto &row : b.rows) {
        auto *r = row.before.resource;
        auto *pixels = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
        if (!pixels) return false;
        auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
        bool same = true;
        for (int i = 0; i < 4096 && same; ++i)
            same = std::memcmp(pixels + i * bpp, d->descriptor.format.defaultPixel.constData(), bpp) == 0;
        row.changed = same; row.after = same ? Impl::Selection{} : row.before;
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "purge preparation refused"); return false; }

