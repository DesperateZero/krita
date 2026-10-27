/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TILE_DATA_STORE_TEST_ACCESS_H
#define KIS_TILE_DATA_STORE_TEST_ACCESS_H

#include "tiles3/kis_tile_data_store.h"

class KisTileDataStoreTestAccess
{
public:
    // The same original registration pin retained by a prepared swap. This
    // observes its independent tail without manufacturing a provider transition.
    static KisTileDataStore::PreparedResidencyObserver holdResidencyRegistration(KisTileData *tile)
    {
        auto *store = KisTileDataStore::instance();
        QMutexLocker gate(&store->m_residencyObserverLock);
        const auto found = store->m_residencyObservers.find(tile);
        return found == store->m_residencyObservers.end()
            ? KisTileDataStore::PreparedResidencyObserver{}
            : KisTileDataStore::PreparedResidencyObserver(&*found);
    }

    static void failNextSwapIn(KisSwapInFailurePoint point)
    {
        KisTileDataStore::instance()->testingFailNextSwapIn(point);
    }

    static void rereadConfig()
    {
        KisTileDataStore::instance()->testingRereadConfig();
    }

    static void clear()
    {
        KisTileDataStore::instance()->debugClear();
    }
};

#endif
