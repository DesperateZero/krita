/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILE_PAGE_STORE_BRIDGE_H
#define KIS_TILE_PAGE_STORE_BRIDGE_H

#include <QtGlobal>

#include <memory>

class KisTileData;
using TileLease = std::unique_ptr<class KisTilePageStoreLease>;

class KisTilePageStoreLease
{
public:
    enum class ReadPinResult { Ready, Unavailable, Stale, Retired };
    virtual ~KisTilePageStoreLease() = default;
    virtual KisTileData *tileData() const = 0;
    virtual bool writable() const = 0;
    virtual void markDirty() = 0;
    // Idempotent: a barrier-cancelled lease is already successfully finished.
    virtual bool finish() = 0;
    // A dormant cache is only an expected-identity candidate. Each outer
    // read must acquire a fresh physical pin before returning tileData().
    virtual bool reusableReadCache() const { return false; }
    virtual bool readPinned() const { return true; }
    virtual ReadPinResult repinRead() { return ReadPinResult::Unavailable; }
    virtual TileLease takeReadCache() { return {}; }
};

/**
 * Narrow internal bridge used while the public tiles3 API is retained over a
 * PageStore authority. It intentionally exposes neither PageStore identity nor
 * provider handles to legacy iterators.
 */
class KisTilePageStoreBridge
{
public:
    virtual ~KisTilePageStoreBridge() = default;
    virtual TileLease acquireTile(
        qint32 column, qint32 row, bool writable, bool oldData,
        TileLease *readCache = nullptr) = 0;
};

#endif // KIS_TILE_PAGE_STORE_BRIDGE_H
