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

class KisTilePageStoreLease
{
public:
    virtual ~KisTilePageStoreLease() = default;
    virtual KisTileData *tileData() const = 0;
    virtual bool writable() const = 0;
    virtual void markDirty() = 0;
    // Idempotent: a barrier-cancelled lease is already successfully finished.
    virtual bool finish() = 0;
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
    virtual std::unique_ptr<KisTilePageStoreLease> acquireTile(
        qint32 column, qint32 row, bool writable, bool oldData) = 0;
};

#endif // KIS_TILE_PAGE_STORE_BRIDGE_H
