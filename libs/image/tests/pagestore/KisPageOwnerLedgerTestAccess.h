/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_OWNER_LEDGER_TEST_ACCESS_H
#define KIS_PAGE_OWNER_LEDGER_TEST_ACCESS_H
#include "KisPageOwnerLedger.h"

// Independent ledger fixtures borrow provider storage owned by their outer
// fixture. Withdraw only that local registration; this does not free a provider
// allocation or prove a terminal result. Production adoption uses the same
// private rollback with the record already retained by its original caller.
class KisPageOwnerLedgerTestAccess
{
public:
    static bool withdrawRegistration(KisPageOwnerLedger &owner, KisPageRetirementRecord &record)
    {
        return owner.rollbackBackingRegistration(record);
    }
    static bool withdrawRegistration(KisPageOwnerLedger &owner, const KisReplicaHandle &replica)
    {
        auto record = owner.takeRetirementRecord(replica);
        return record && owner.rollbackBackingRegistration(*record);
    }
};
#endif
