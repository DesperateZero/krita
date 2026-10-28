/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_VIEW_TEST_ACCESS_H
#define KIS_PAGE_VIEW_TEST_ACCESS_H
#include "KisImageEpochReferenceModel.h"
#include "KisPageMetadataCoordinator.h"

// Composed owner fixtures use the production Value domain explicitly. Pure
// epoch reference tests continue to use the independent identity-only oracle.
class KisPageViewTestAccess
{
public:
    static bool initialize(KisImageEpochReferenceModel &epochs, KisPageMetadataCoordinator &metadata,
                           const KisImageEpochSnapshot &initial, QString *error)
    {
        auto prepared = epochs.prepareInitialization(initial, error, &metadata);
        return prepared && epochs.installInitialization(std::move(prepared));
    }
    static auto values(KisPageMetadataCoordinator &metadata) { return metadata.viewValues(); }
};
#endif
