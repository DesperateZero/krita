/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_STORE_RECLAMATION_P_H
#define KIS_PAGE_STORE_RECLAMATION_P_H

#include <functional>
#include <QtGlobal>
#include <kritaimage_export.h>

// Separate from the image/brush worker pool. Jobs must own everything they
// use, must not wait for another reclamation job, and must bound their work.
// This is execution only: it never decides logical/physical release eligibility.
// Private runtime/test boundary; exporting the symbol does not grant page
// mutation authority. Tests use queued barriers to prove inter-pass ordering.
KRITAIMAGE_EXPORT void kisSchedulePageStoreReclamation(std::function<void()> job);
KRITAIMAGE_EXPORT bool kisOnPageStoreReclamationThread();

// Cold correctness/shutdown endpoint. Requires quiescent producers; never call
// from a reclamation job or an interaction/read-guard destructor.
KRITAIMAGE_EXPORT void kisDrainPageStoreReclamation();

struct KisPageTreeReclamationStatistics
{
    quint64 foregroundNodeDestructions = 0;
    quint64 backgroundNodeDestructions = 0;
    quint64 passes = 0;
    quint64 maximumReferenceDropsPerPass = 0;
};
KRITAIMAGE_EXPORT KisPageTreeReclamationStatistics kisPageTreeReclamationStatistics();

#endif
