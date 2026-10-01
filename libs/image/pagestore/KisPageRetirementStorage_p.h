/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_RETIREMENT_STORAGE_P_H
#define KIS_PAGE_RETIREMENT_STORAGE_P_H
#include <memory>
#include <kritaimage_export.h>
class KisBackingBudgetController;
struct KisPageRetirementRecord;
struct KisPageRetirementRecordDeleter {
    KRITAIMAGE_EXPORT void operator()(KisPageRetirementRecord *) const noexcept;
};
using KisPageRetirementRecordPointer =
    std::unique_ptr<KisPageRetirementRecord, KisPageRetirementRecordDeleter>;
// Real terminal storage, prepared before a provider can return physical state.
// Its sole owner transfers from the original backing admission to ledger/queue.
KRITAIMAGE_EXPORT KisPageRetirementRecordPointer kisPreparePageRetirementRecord(KisBackingBudgetController *);
#endif
