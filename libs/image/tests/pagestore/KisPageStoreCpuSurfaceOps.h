/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_CPU_SURFACE_OPS_H
#define KIS_PAGE_STORE_CPU_SURFACE_OPS_H

#include "KisPageStoreRandomAccessor.h"

/**
 * Test-only rectangular PageStore reference operations. These preserve the
 * standalone Store coverage without adding an unused production adapter.
 */
class KisPageStoreCpuSurfaceOps
{
public:
    static bool removePage(KisPageStore *store,
                           const KisPageTransaction &transaction,
                           const KisPageKey &key,
                           QString *error = nullptr);

    static bool readRect(KisPageStore *store,
                         KisSurfaceId surface,
                         const KisPageReadView &view,
                         const QRect &rect,
                         QByteArray *bytes,
                         qsizetype destinationRowStride = 0,
                         QString *error = nullptr);

    static bool writeRect(KisPageStore *store,
                          KisSurfaceId surface,
                          const KisPageTransaction &transaction,
                          const QRect &rect,
                          const QByteArray &bytes,
                          qsizetype sourceRowStride = 0,
                          QString *error = nullptr);

    static bool fillRect(KisPageStore *store,
                         KisSurfaceId surface,
                         const KisPageTransaction &transaction,
                         const QRect &rect,
                         const QByteArray &pixel,
                         QString *error = nullptr);

    static bool copyRect(KisPageStore *store,
                         KisSurfaceId sourceSurface,
                         const KisPageReadView &sourceView,
                         const QRect &sourceRect,
                         KisSurfaceId destinationSurface,
                         const KisPageTransaction &destinationTransaction,
                         const QPoint &destinationTopLeft,
                         QString *error = nullptr);
};

#endif // KIS_PAGE_STORE_CPU_SURFACE_OPS_H
