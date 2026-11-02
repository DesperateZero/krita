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
    using Acceptance = KisPageOwnerLedger::BackingAcceptance;
    // Fixture-owned input/scratch adapts to the actual publication stream;
    // it is not a second product protocol or a storage-cost measurement.
    template<class Install>
    static Acceptance acceptChanges(KisPageOwnerLedger &owner,
        const KisPageSnapshotArray<KisBackingClassChange> &changes,
        Install install, QString *error = nullptr)
    {
        std::vector<KisPagePreparedBackingChange> rows(size_t(changes.size()));
        auto reader = [position = changes.cbegin(), end = changes.cend()](KisBackingClassChange *change) mutable -> int {
            if (position == end) return 0;
            *change = *position++;
            return 1;
        };
        return acceptRows(owner, rows.data(), rows.size(), reader, std::move(install), error);
    }
    static bool changeClasses(KisPageOwnerLedger &owner,
        const KisPageSnapshotArray<KisBackingClassChange> &changes,
        QString *error = nullptr, bool commit = true)
    {
        return acceptChanges(owner, changes, [commit] { return commit; }, error)
            == (commit ? Acceptance::Accepted : Acceptance::InstallRejected);
    }
    template<class Reader, class Install>
    static Acceptance acceptRows(KisPageOwnerLedger &owner,
        KisPagePreparedBackingChange *rows, size_t capacity, Reader &reader,
        Install install, QString *error = nullptr)
    {
        if (!owner.synchronizeBackingDomains(error)) return KisPageOwnerLedger::BackingAcceptance::FeesRefused;
        return owner.acceptPreparedBackingChanges(rows, capacity, &reader,
            [](void *context, KisBackingClassChange *change) { return (*static_cast<Reader *>(context))(change); },
            &install, [](void *context) { return (*static_cast<Install *>(context))(); }, error);
    }
    static bool acceptIdle(KisPageOwnerLedger &owner, KisPagePublicationRows rows,
        quint64 allowance, QString *error = nullptr, bool commit = true)
    {
        return acceptIdleRows(owner, rows, allowance, [commit] { return commit; }, error)
            == (commit ? Acceptance::Accepted : Acceptance::InstallRejected);
    }
    template<class Install>
    static Acceptance acceptIdleRows(KisPageOwnerLedger &owner, KisPagePublicationRows rows,
        quint64 allowance, Install install, QString *error = nullptr)
    {
        if (!owner.synchronizeBackingDomains(error)) return Acceptance::FeesRefused;
        return owner.acceptPreparedIdleCapacity(rows, allowance, &install,
            [](void *context) { return (*static_cast<Install *>(context))(); }, error);
    }
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
