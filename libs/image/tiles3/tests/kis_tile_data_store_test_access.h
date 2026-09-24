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
    static void failNextSwapIn(KisSwapInFailurePoint point)
    {
        KisTileDataStore::instance()->testingFailNextSwapIn(point);
    }
};

#endif
