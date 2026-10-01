/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageOwnerLedger.h"
#include "KisPageRetirementRecord_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QHash>
#include <QEnableSharedFromThis>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QScopeGuard>
#include <QSet>
#include <QWaitCondition>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <unordered_map>

namespace {

std::atomic<quint64> s_nextRequestId{1};
std::atomic<quint64> s_nextOperationId{1};
std::atomic<quint64> s_nextLeaseId{1};
std::atomic<quint64> s_nextWriterToken{1};

using ProviderKey = QPair<quint64, quint64>;

ProviderKey providerKey(KisReplicaProviderId provider,
                        KisReplicaProviderEpoch epoch)
{
    return {provider.value, epoch.value};
}

using ProviderOperationRecord = KisPageProviderOperationRecord;
using OperationIndex = KisPageProviderOperationIndex;

struct ProviderRegistration
{
    QSharedPointer<KisPageReplicaProvider> provider;
    bool backgroundRetirement = false;
};
using ProviderIndex = QHash<ProviderKey, ProviderRegistration>;

using BackingKey = KisReplicaAllocationIdentity;
using PhysicalBackingKey = KisReplicaPhysicalSlotIdentity;

PhysicalBackingKey physicalBackingKey(
    const KisReplicaHandle &replica,
    const KisReplicaBackingFootprint &footprint)
{
    return {replica.provider, replica.providerEpoch, footprint.physicalSlot};
}

void addBackingDelta(KisBackingBudgetDelta *delta, KisBackingBudgetClass budgetClass,
                     KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->buckets[static_cast<size_t>(budgetClass)];
    if (qint64 *component = kisPageDomainComponent(bucket, domain))
        *component += bytes;
}

bool addBackingDeltaChecked(KisBackingBudgetDelta *delta,
                            KisBackingBudgetClass budgetClass,
                            KisPageAccessDomain domain, qint64 bytes)
{
    auto &bucket = delta->buckets[static_cast<size_t>(budgetClass)];
    qint64 *component = kisPageDomainComponent(bucket, domain);
    if (!component) return false;
    qint64 result = 0;
    if (__builtin_add_overflow(*component, bytes, &result)) return false;
    *component = result;
    return true;
}

KisBackingBudgetDelta positiveBackingDelta(const KisBackingBudgetDelta &signedDelta)
{
    KisBackingBudgetDelta result;
    for (size_t i = 0; i < signedDelta.buckets.size(); ++i) {
        const auto &source = signedDelta.buckets[i];
        auto &target = result.buckets[i];
        target.cpuRam = std::max(qint64(0), source.cpuRam);
        target.umaShared = std::max(qint64(0), source.umaShared);
        target.discreteVram = std::max(qint64(0), source.discreteVram);
        target.ssd = std::max(qint64(0), source.ssd);
    }
    return result;
}

struct BackingRecord
{
    KisReplicaHandle replica;
    KisBackingBudgetClass budgetClass = KisBackingBudgetClass::ActivePending;
    PhysicalBackingKey physical;
    KisBackingBudgetReservation retirementHeadroom;
    KisPageRetirementRecordPointer retirement;
};

// Reuse the existing node when allocation generation changes. Cardinality is
// unchanged under the ledger mutex, so extract/reinsert cannot rehash or
// allocate. No target placeholder or second identity table is needed.
struct BackingKeyHash {
    size_t operator()(const BackingKey &key) const noexcept { return qHash(key); }
};
using BackingIndex = std::unordered_map<BackingKey, BackingRecord, BackingKeyHash>;

struct PhysicalBackingRecord
{
    KisReplicaHandle representative;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 domainRevision = 0;
    quint64 domainClaim = 0;
    quint64 classClaim = 0;
    std::array<quint32, static_cast<size_t>(KisBackingBudgetClass::Count)> references{};
    KisBackingBudgetClass chargedClass = KisBackingBudgetClass::Count;

    quint64 byteSize() const { return representative.layout.byteSize; }
};

KisBackingBudgetClass chargedBackingClass(const PhysicalBackingRecord &record)
{
    // Physical bytes are charged once. Prefer the class that must remain live
    // for foreground correctness over rebuildable/history/debt ownership.
    constexpr std::array<KisBackingBudgetClass, 7> priority{
        KisBackingBudgetClass::Current,
        KisBackingBudgetClass::ActivePending,
        KisBackingBudgetClass::InFlight,
        KisBackingBudgetClass::RetainedHistory,
        KisBackingBudgetClass::RetirementDebt,
        KisBackingBudgetClass::OptionalCache,
        KisBackingBudgetClass::MetadataArena};
    for (const KisBackingBudgetClass budgetClass : priority) {
        if (record.references[static_cast<size_t>(budgetClass)] != 0)
            return budgetClass;
    }
    return KisBackingBudgetClass::Count;
}

bool changePhysicalReferenceClass(PhysicalBackingRecord *record,
                                  KisBackingBudgetClass before,
                                  KisBackingBudgetClass after)
{
    if (!record || before == after)
        return false;
    auto &beforeCount = record->references[static_cast<size_t>(before)];
    auto &afterCount = record->references[static_cast<size_t>(after)];
    if (!beforeCount || afterCount == std::numeric_limits<quint32>::max())
        return false;
    --beforeCount;
    ++afterCount;
    return true;
}

bool addPhysicalChargeChange(KisBackingBudgetDelta *delta,
                             const PhysicalBackingRecord &record,
                             KisBackingBudgetClass before,
                             KisBackingBudgetClass after)
{
    if (before == after)
        return true;
    if (before == KisBackingBudgetClass::Count
        || after == KisBackingBudgetClass::Count
        || record.byteSize() > quint64(std::numeric_limits<qint64>::max())) {
        return false;
    }
    const qint64 bytes = qint64(record.byteSize());
    return addBackingDeltaChecked(delta, before, record.domain, -bytes)
        && addBackingDeltaChecked(delta, after, record.domain, bytes);
}

struct PreparedRetirementCredit
{
    BackingKey backing;
    KisBackingBudgetClass before;
    KisPageAccessDomain domain;
    quint64 bytes;

    KisBackingBudgetDelta delta() const
    {
        KisBackingBudgetDelta value;
        addBackingDelta(&value, before, domain, -qint64(bytes));
        addBackingDelta(&value, KisBackingBudgetClass::RetirementDebt, domain, qint64(bytes));
        return value;
    }
};

void subtractBackingDelta(KisBackingBudgetDelta *total, const KisBackingBudgetDelta &part)
{
    for (size_t i = 0; i < total->buckets.size(); ++i) {
        total->buckets[i].cpuRam -= part.buckets[i].cpuRam;
        total->buckets[i].umaShared -= part.buckets[i].umaShared;
        total->buckets[i].discreteVram -= part.buckets[i].discreteVram;
        total->buckets[i].ssd -= part.buckets[i].ssd;
    }
}

struct PreparedBackingChangeSlot
{
    QVector<KisBackingClassChange> changes;
    QVector<PhysicalBackingKey> physicals;
    KisBackingBudgetReservation reservation;
    KisReplicaHandle handoffTarget;
    std::vector<PreparedRetirementCredit> retirementCredits;
    quint32 generation = 0;
};

bool buildPhysicalBackingDelta(
    const QVector<KisBackingClassChange> &changes,
    const BackingIndex &backings,
    const QHash<PhysicalBackingKey, PhysicalBackingRecord> &physicalBackings,
    const QSet<BackingKey> &preparedChanges,
    KisBackingBudgetDelta *signedDelta,
    std::vector<PreparedRetirementCredit> *credits,
    QString *error)
{
    QHash<PhysicalBackingKey, PhysicalBackingRecord> projected;
    QHash<PhysicalBackingKey, BackingKey> availableCredits;
    QSet<BackingKey> classified;
    classified.reserve(changes.size());
    for (const KisBackingClassChange &change : changes) {
        const BackingKey key = change.replica.allocationIdentity();
        const auto found = backings.find(key);
        if (!change.replica.isValid() || found == backings.end()
            || preparedChanges.contains(key)
            || found->second.budgetClass != change.before
            || !(found->second.replica == change.replica)
            || change.before == change.after
            || static_cast<size_t>(change.after) >=
                   static_cast<size_t>(KisBackingBudgetClass::Count)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing class change is invalid"));
            return false;
        }
        if (classified.contains(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing is classified more than once"));
            return false;
        }
        classified.insert(key);
        if (found->second.retirementHeadroom.isValid())
            availableCredits.insert(found->second.physical, key);
        auto physical = projected.find(found->second.physical);
        if (physical == projected.end()) {
            const auto source = physicalBackings.constFind(found->second.physical);
            if (source == physicalBackings.constEnd()) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing owner is missing"));
                return false;
            }
            if (source->domainClaim || source->classClaim) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("physical backing transition is active"));
                return false;
            }
            physical = projected.insert(found->second.physical, *source);
        }
        if (!changePhysicalReferenceClass(&physical.value(), change.before,
                                          change.after)) {
            KisPageStoreDetail::setError(error, QStringLiteral("physical backing class references are invalid"));
            return false;
        }
    }
    for (auto projectedIt = projected.cbegin(); projectedIt != projected.cend();
         ++projectedIt) {
        const auto source = physicalBackings.constFind(projectedIt.key());
        Q_ASSERT(source != physicalBackings.constEnd());
        if (!addPhysicalChargeChange(signedDelta, projectedIt.value(),
                                     source->chargedClass,
                                     chargedBackingClass(projectedIt.value()))) {
            KisPageStoreDetail::setError(error, QStringLiteral("physical backing delta overflows"));
            return false;
        }
        if (source->chargedClass != KisBackingBudgetClass::RetirementDebt
            && chargedBackingClass(projectedIt.value()) == KisBackingBudgetClass::RetirementDebt) {
            // A physical charge needs one credit even when all its aliases
            // retire together. Only touched records are inspected.
            const auto credit = availableCredits.constFind(projectedIt.key());
            if (credit != availableCredits.cend())
                credits->push_back({credit.value(), source->chargedClass,
                                    source->domain, source->byteSize()});
        }
    }
    return true;
}

}

class KisPageOwnerLedger::Private
{
public:
    KisCompletionTicket operationCompletion(
        KisPageOperationId operation,
        QSharedPointer<KisCompletionRegistry> *registry) const
    {
        QMutexLocker locker(&mutex);
        const auto found = operations.find(operation.value);
        if (!completions || !operation.isValid() || found == operations.end())
            return {};
        *registry = completions;
        return found->second.completion;
    }

    PreparedBackingChangeSlot *preparedChange(quint64 cookie)
    {
        const quint32 slotIndex = quint32(cookie);
        if (slotIndex >= preparedChanges.size())
            return nullptr;
        auto &slot = preparedChanges.at(slotIndex);
        return slot.reservation.isValid() && slot.generation == quint32(cookie >> 32)
            ? &slot : nullptr;
    }

    quint64 storePreparedChange(
        QVector<KisBackingClassChange> changes,
        KisBackingBudgetReservation reservation,
        const KisReplicaHandle &handoffTarget = {},
        std::vector<PreparedRetirementCredit> credits = {})
    {
        quint32 slotIndex = 0;
        if (freePreparedChanges.empty()) {
            slotIndex = quint32(preparedChanges.size());
            if (freePreparedChanges.capacity() < preparedChanges.size() + 1) {
                freePreparedChanges.reserve(
                    std::max(preparedChanges.size() + 1,
                             freePreparedChanges.capacity()
                                 ? freePreparedChanges.capacity() * 2 : size_t(1)));
            }
            preparedChanges.emplace_back();
        } else {
            slotIndex = freePreparedChanges.back();
            freePreparedChanges.pop_back();
        }
        PreparedBackingChangeSlot &slot = preparedChanges.at(slotIndex);
        ++slot.generation;
        if (!slot.generation)
            ++slot.generation;
        const quint64 cookie = (quint64(slot.generation) << 32) | slotIndex;
        slot.handoffTarget = handoffTarget;
        slot.changes = std::move(changes);
        slot.reservation = std::move(reservation);
        slot.retirementCredits = std::move(credits);

        QSet<PhysicalBackingKey> claimedPhysicals;
        claimedPhysicals.reserve(slot.changes.size());
        slot.physicals.reserve(slot.changes.size());
        for (const KisBackingClassChange &change : std::as_const(slot.changes)) {
            const BackingKey key = change.replica.allocationIdentity();
            const auto backing = backings.find(key);
            Q_ASSERT(backing != backings.end());
            preparedBackingChanges.insert(key);
            if (backing == backings.end()
                || claimedPhysicals.contains(backing->second.physical)) {
                continue;
            }
            auto physical = physicalBackings.find(backing->second.physical);
            Q_ASSERT(physical != physicalBackings.end()
                     && !physical->domainClaim && !physical->classClaim);
            if (physical != physicalBackings.end()) {
                physical->classClaim = cookie;
                claimedPhysicals.insert(backing->second.physical);
                slot.physicals.append(backing->second.physical);
            }
        }
        if (handoffTarget.isValid())
            preparedBackingChanges.insert(handoffTarget.allocationIdentity());
        return cookie;
    }

    void releasePreparedClaims(PreparedBackingChangeSlot &slot,
                               quint64 cookie) noexcept
    {
        for (const KisBackingClassChange &change : std::as_const(slot.changes))
            preparedBackingChanges.remove(change.replica.allocationIdentity());
        for (const PhysicalBackingKey &key : std::as_const(slot.physicals)) {
            auto physical = physicalBackings.find(key);
            if (physical != physicalBackings.end()
                && physical->classClaim == cookie) {
                physical->classClaim = 0;
            }
        }
        if (slot.handoffTarget.isValid())
            preparedBackingChanges.remove(slot.handoffTarget.allocationIdentity());
        slot.handoffTarget = {};
        slot.physicals.clear();
        physicalClaimsChanged.wakeAll();
    }

    void applyBackingChange(const KisBackingClassChange &change,
                            KisBackingBudgetDelta *signedDelta) noexcept
    {
        auto found = backings.find(change.replica.allocationIdentity());
        Q_ASSERT(found != backings.end());
        Q_ASSERT(found->second.budgetClass == change.before);
        if (change.before == change.after) return;
        auto physical = physicalBackings.find(found->second.physical);
        Q_ASSERT(physical != physicalBackings.end());
        const KisBackingBudgetClass beforeCharge = physical->chargedClass;
        const bool changed = changePhysicalReferenceClass(
            &physical.value(), change.before, change.after);
        Q_ASSERT(changed);
        Q_UNUSED(changed);
        const KisBackingBudgetClass afterCharge = chargedBackingClass(*physical);
        const bool deltaAdded = addPhysicalChargeChange(
            signedDelta, *physical, beforeCharge, afterCharge);
        Q_ASSERT(deltaAdded);
        Q_UNUSED(deltaAdded);
        physical->chargedClass = afterCharge;
        found->second.budgetClass = change.after;
    }

    void moveRetirementHeadroom(const PhysicalBackingRecord &physical,
                                KisPageAccessDomain target) noexcept
    {
        // Handoff requires a sole physical owner and makes that owner the
        // representative. Aliases cannot create another headroom. Retirement
        // consumes/releases it before replacing the representative, so domain
        // migration needs one lookup, not an alias-membership scan.
        auto found = backings.find(physical.representative.allocationIdentity());
        Q_ASSERT(found != backings.end());
        auto &headroom = found->second.retirementHeadroom;
        if (headroom.isValid())
            backingBudget->moveRetirementHeadroom(headroom.cookie, physical.domain,
                                                   target, physical.byteSize());
    }

    mutable QMutex mutex;
    QSharedPointer<KisCompletionRegistry> completions;
    quint64 nextValidationStamp = 1;
    ProviderIndex providers;
    QSet<ProviderKey> pendingProviderRegistrations;
    OperationIndex operations;
    QHash<quint64, KisPreparedPageProof> sealedProofs;
    KisBackingBudgetController *backingBudget = nullptr;
    BackingIndex backings;
    QHash<PhysicalBackingKey, PhysicalBackingRecord> physicalBackings;
    QWaitCondition physicalClaimsChanged;
    // Lookup only for physically shared backing; singleton uses representative.
    QHash<PhysicalBackingKey, QSet<BackingKey>> sharedPhysicalMembers;
    QSet<BackingKey> preparedBackingChanges;
    std::vector<PreparedBackingChangeSlot> preparedChanges;
    std::vector<quint32> freePreparedChanges;
    QSharedPointer<KisPageOwnerDomainAdmission> domainAdmission;
};

class KisPageOwnerDomainAdmission final
    : public KisReplicaBackingDomainAdmission
    , public QEnableSharedFromThis<KisPageOwnerDomainAdmission>
{
public:
    explicit KisPageOwnerDomainAdmission(KisPageOwnerLedger *owner)
        : m_owner(owner) {}

    class Reservation final : public KisReplicaBackingDomainReservation
    {
    public:
        Reservation(QSharedPointer<KisPageOwnerDomainAdmission> owner,
                    quint64 cookie)
            : m_owner(std::move(owner)), m_cookie(cookie) {}
        ~Reservation() override
        {
            if (m_owner)
                m_owner->cancel(m_cookie);
        }
        void commit(quint64 targetRevision) noexcept override
        {
            if (!m_owner)
                return;
            m_owner->commit(m_cookie, targetRevision);
            m_owner.reset();
            m_cookie = 0;
        }

    private:
        QSharedPointer<KisPageOwnerDomainAdmission> m_owner;
        quint64 m_cookie = 0;
    };

    QSharedPointer<KisReplicaBackingDomainReservation> prepare(
        const KisReplicaPhysicalSlotIdentity &physical,
        quint64 bytes,
        KisPageAccessDomain sourceDomain,
        quint64 sourceRevision,
        KisPageAccessDomain targetDomain,
        QString *error) override
    {
        QMutexLocker authorityLocker(&m_mutex);
        if (!m_owner || !physical.provider.isValid()
            || !physical.providerEpoch.isValid() || !physical.slot
            || !bytes || !sourceRevision
            || sourceDomain == KisPageAccessDomain::Unknown
            || targetDomain == KisPageAccessDomain::Unknown
            || sourceDomain == targetDomain
            || bytes > quint64(std::numeric_limits<qint64>::max())) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition request is invalid"));
            return {};
        }
        if (m_pendingRegistrations.contains(physical)) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain registration is active"));
            return {};
        }
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        auto record = owner->physicalBackings.find(physical);
        if (record == owner->physicalBackings.end()) {
            KisPageStoreDetail::setError(error, {});
            return QSharedPointer<Reservation>::create(sharedFromThis(), 0);
        }
        if (!owner->backingBudget || record->byteSize() != bytes
            || record->domain != sourceDomain
            || record->domainRevision != sourceRevision
            || record->domainClaim || record->classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition source is stale or busy"));
            return {};
        }

        KisBackingBudgetDelta signedDelta;
        if (!addBackingDeltaChecked(&signedDelta, record->chargedClass,
                                    sourceDomain, -qint64(bytes))
            || !addBackingDeltaChecked(&signedDelta, record->chargedClass,
                                       targetDomain, qint64(bytes))) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing-domain transition delta overflows"));
            return {};
        }
        auto reservation = owner->backingBudget->reserveChange(
            signedDelta, error);
        if (!reservation.isValid())
            return {};
        ++m_nextCookie;
        if (!m_nextCookie)
            ++m_nextCookie;
        const quint64 cookie = m_nextCookie;
        record->domainClaim = cookie;
        Claim claim;
        claim.physical = physical;
        claim.sourceDomain = sourceDomain;
        claim.sourceRevision = sourceRevision;
        claim.targetDomain = targetDomain;
        claim.bytes = bytes;
        claim.signedDelta = signedDelta;
        claim.reservation = std::move(reservation);
        m_claims.emplace(cookie, std::move(claim));
        KisPageStoreDetail::setError(error, {});
        return QSharedPointer<Reservation>::create(sharedFromThis(), cookie);
    }

    void detach() noexcept
    {
        QMutexLocker authorityLocker(&m_mutex);
        if (!m_owner)
            return;
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        for (const auto &[cookie, claim] : m_claims) {
            auto record = owner->physicalBackings.find(claim.physical);
            if (record != owner->physicalBackings.end()
                && record->domainClaim == cookie) {
                record->domainClaim = 0;
            }
        }
        m_claims.clear();
        m_pendingRegistrations.clear();
        owner->physicalClaimsChanged.wakeAll();
        m_owner = nullptr;
    }

    bool beginRegistration(const PhysicalBackingKey &physical)
    {
        QMutexLocker locker(&m_mutex);
        if (!m_owner || m_pendingRegistrations.contains(physical))
            return false;
        m_pendingRegistrations.insert(physical);
        return true;
    }

    void endRegistration(const PhysicalBackingKey &physical) noexcept
    {
        QMutexLocker locker(&m_mutex);
        m_pendingRegistrations.remove(physical);
    }

private:
    struct Claim {
        PhysicalBackingKey physical;
        KisPageAccessDomain sourceDomain = KisPageAccessDomain::Unknown;
        quint64 sourceRevision = 0;
        KisPageAccessDomain targetDomain = KisPageAccessDomain::Unknown;
        quint64 bytes = 0;
        KisBackingBudgetDelta signedDelta;
        KisBackingBudgetReservation reservation;
    };

    void cancel(quint64 cookie) noexcept
    {
        if (!cookie)
            return;
        QMutexLocker authorityLocker(&m_mutex);
        auto claim = m_claims.find(cookie);
        if (claim == m_claims.end())
            return;
        if (m_owner) {
            auto *owner = m_owner->d.data();
            QMutexLocker ownerLocker(&owner->mutex);
            auto record = owner->physicalBackings.find(claim->second.physical);
            if (record != owner->physicalBackings.end()
                && record->domainClaim == cookie) {
                record->domainClaim = 0;
            }
            m_claims.erase(claim);
            owner->physicalClaimsChanged.wakeAll();
        } else {
            m_claims.erase(claim);
        }
    }

    void commit(quint64 cookie, quint64 targetRevision) noexcept
    {
        if (!cookie)
            return;
        QMutexLocker authorityLocker(&m_mutex);
        auto claim = m_claims.find(cookie);
        if (claim == m_claims.end() || !m_owner)
            return;
        auto *owner = m_owner->d.data();
        QMutexLocker ownerLocker(&owner->mutex);
        auto record = owner->physicalBackings.find(claim->second.physical);
        const auto revisionOrder = record != owner->physicalBackings.end()
            ? kisCompareBackingRevision(targetRevision, record->domainRevision)
            : KisBackingRevisionOrder::Ambiguous;
        Q_ASSERT(record != owner->physicalBackings.end()
                 && record->domainClaim == cookie
                 && record->domain == claim->second.sourceDomain
                 && record->domainRevision == claim->second.sourceRevision
                 && revisionOrder == KisBackingRevisionOrder::Newer);
        if (record != owner->physicalBackings.end()
            && record->domainClaim == cookie
            && record->domain == claim->second.sourceDomain
            && record->domainRevision == claim->second.sourceRevision
            && revisionOrder == KisBackingRevisionOrder::Newer) {
            owner->backingBudget->commitReservation(
                std::move(claim->second.reservation), claim->second.signedDelta);
            owner->moveRetirementHeadroom(*record, claim->second.targetDomain);
            record->domain = claim->second.targetDomain;
            record->domainRevision = targetRevision;
            record->domainClaim = 0;
        }
        m_claims.erase(claim);
        owner->physicalClaimsChanged.wakeAll();
    }

    QMutex m_mutex;
    KisPageOwnerLedger *m_owner = nullptr;
    quint64 m_nextCookie = 0;
    std::unordered_map<quint64, Claim> m_claims;
    QSet<PhysicalBackingKey> m_pendingRegistrations;
};

KisBackingClassChangeReservation::KisBackingClassChangeReservation(
    KisPageOwnerLedger *owner, quint64 cookie)
    : m_owner(owner), m_cookie(cookie)
{
}

KisBackingClassChangeReservation::~KisBackingClassChangeReservation()
{
    release();
}

KisBackingClassChangeReservation::KisBackingClassChangeReservation(
    KisBackingClassChangeReservation &&other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr))
    , m_cookie(std::exchange(other.m_cookie, 0))
{
}

KisBackingClassChangeReservation &
KisBackingClassChangeReservation::operator=(
    KisBackingClassChangeReservation &&other) noexcept
{
    if (this != &other) {
        release();
        m_owner = std::exchange(other.m_owner, nullptr);
        m_cookie = std::exchange(other.m_cookie, 0);
    }
    return *this;
}

bool KisBackingClassChangeReservation::isValid() const
{
    return m_owner && m_cookie;
}

void KisBackingClassChangeReservation::release() noexcept
{
    if (m_owner)
        m_owner->cancelPreparedBackingChanges(m_cookie);
    m_owner = nullptr;
    m_cookie = 0;
}

KisPageOwnerLedger::KisPageOwnerLedger()
    : d(new Private)
{
    d->domainAdmission = QSharedPointer<KisPageOwnerDomainAdmission>::create(this);
}

KisPageOwnerLedger::~KisPageOwnerLedger()
{
    d->domainAdmission->detach();
}

bool KisPageOwnerLedger::configure(
    const QSharedPointer<KisCompletionRegistry> &completions,
    QString *error)
{
    if (!completions || !completions->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral("owner ledger completion registry is invalid"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is already configured"));
        return false;
    }
    d->completions = completions;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageOwnerLedger::attachBackingBudget(KisBackingBudgetController &budget)
{
    QMutexLocker locker(&d->mutex);
    Q_ASSERT(!d->backingBudget);
    Q_ASSERT(d->operations.empty());
    d->operations = OperationIndex(std::less<quint64>{},
        KisMutationStorageAllocator<std::pair<const quint64, ProviderOperationRecord>>(&budget));
    d->backingBudget = &budget;
}

KisReplicaBackingFootprint KisPageOwnerLedger::observeBackingFootprint(
    const KisReplicaHandle &replica) const
{
    QSharedPointer<KisPageReplicaProvider> replicaProvider;
    {
        QMutexLocker locker(&d->mutex);
        replicaProvider = d->providers.value(providerKey(replica.provider,
                                                         replica.providerEpoch)).provider;
    }
    return replicaProvider ? replicaProvider->backingFootprint(replica)
                           : KisReplicaBackingFootprint{};
}

bool KisPageOwnerLedger::registerBacking(const KisReplicaHandle &replica,
                                         KisBackingBudgetReservation &reservation,
                                         KisBackingBudgetClass budgetClass,
                                         QString *error,
                                         KisPageRetirementRecordPointer *preparedRetirement)
{
    if (!replica.isValid() || !reservation.isValid()
        || static_cast<size_t>(budgetClass) >= static_cast<size_t>(KisBackingBudgetClass::Count)
        || replica.layout.byteSize > quint64(std::numeric_limits<qint64>::max())) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica exceeds its backing reservation"));
        return false;
    }
    KisPageRetirementRecordPointer localRetirement;
    if (!preparedRetirement) {
        // Direct registration/adoption leaves physical ownership with its
        // caller on refusal. Fresh production already prepared this before
        // entering the provider, and transfers that exact node below.
        try { localRetirement = kisPreparePageRetirementRecord(d->backingBudget); }
        catch (const std::bad_alloc &) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing retirement record budget storage was refused"));
            return false;
        }
        preparedRetirement = &localRetirement;
    }
    if (!*preparedRetirement) return false;
    KisReplicaBackingFootprint footprint = observeBackingFootprint(replica);
    if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing footprint is invalid"));
        return false;
    }
    const PhysicalBackingKey registrationPhysical =
        physicalBackingKey(replica, footprint);
    if (!d->domainAdmission->beginRegistration(registrationPhysical)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("physical backing registration is busy"));
        return false;
    }
    const auto finishRegistration = qScopeGuard([&] {
        d->domainAdmission->endRegistration(registrationPhysical);
    });
    footprint = observeBackingFootprint(replica);
    if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize
        || !(physicalBackingKey(replica, footprint) == registrationPhysical)) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("provider physical backing changed during registration"));
        return false;
    }

    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    const PhysicalBackingKey physicalKey = physicalBackingKey(replica, footprint);
    if (!d->backingBudget || reservation.owner != d->backingBudget
        || d->backings.find(key) != d->backings.end()
        || d->preparedBackingChanges.contains(key)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica backing identity is already owned"));
        return false;
    }
    auto physical = d->physicalBackings.find(physicalKey);
    const bool newPhysicalBacking = physical == d->physicalBackings.end();
    KisBackingBudgetDelta sharedChargeChange;
    KisBackingBudgetClass sharedAfterCharge = KisBackingBudgetClass::Count;
    if (newPhysicalBacking) {
        if (!d->backingBudget->reservationCovers(reservation.cookie,
                                                 budgetClass, footprint.domain,
                                                 footprint.bytes)) {
            KisPageStoreDetail::setError(error, QStringLiteral("physical backing domain is not reserved"));
            return false;
        }
        PhysicalBackingRecord record;
        record.representative = replica;
        record.domain = footprint.domain;
        record.domainRevision = footprint.revision;
        record.references[static_cast<size_t>(budgetClass)] = 1;
        record.chargedClass = budgetClass;
        physical = d->physicalBackings.insert(physicalKey, record);
    } else {
        if (physical->domainClaim || physical->classClaim) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("shared physical backing transition is active"));
            return false;
        }
        if (physical->domain != footprint.domain
            || physical->domainRevision != footprint.revision
            || physical->byteSize() != footprint.bytes) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing footprint changed"));
            return false;
        }
        PhysicalBackingRecord next = *physical;
        auto &references = next.references[static_cast<size_t>(budgetClass)];
        if (references == std::numeric_limits<quint32>::max()) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing reference count overflow"));
            return false;
        }
        ++references;
        sharedAfterCharge = chargedBackingClass(next);
        if (!addPhysicalChargeChange(&sharedChargeChange, next,
                                     physical->chargedClass, sharedAfterCharge)
            || (sharedAfterCharge != physical->chargedClass
                && !d->backingBudget->reservationCovers(
                    reservation.cookie, sharedAfterCharge, footprint.domain,
                    footprint.bytes))) {
            KisPageStoreDetail::setError(error, QStringLiteral("shared physical backing class change is not reserved"));
            return false;
        }
        *physical = next;
    }
    d->backings.emplace(key, BackingRecord{replica, budgetClass, physicalKey, {}, {}});

    if (!newPhysicalBacking) {
        auto members = d->sharedPhysicalMembers.find(physicalKey);
        if (members == d->sharedPhysicalMembers.end()) {
            QSet<BackingKey> firstPair;
            firstPair.insert(physical->representative.allocationIdentity());
            firstPair.insert(key);
            d->sharedPhysicalMembers.insert(physicalKey, std::move(firstPair));
        } else {
            members->insert(key);
        }
        // The same bytes have one charge, but a new foreground alias can
        // outrank the former owner class and must move that charge atomically.
        if (sharedAfterCharge != physical->chargedClass) {
            reservation.commit(sharedChargeChange);
            physical->chargedClass = sharedAfterCharge;
        } else {
            reservation.release();
        }
        d->backings.find(key)->second.retirement = std::move(*preparedRetirement);
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    KisBackingBudgetDelta installed;
    addBackingDelta(&installed, budgetClass, footprint.domain, qint64(footprint.bytes));
    if (budgetClass != KisBackingBudgetClass::RetirementDebt
        && d->backingBudget->reservationCovers(
            reservation.cookie, KisBackingBudgetClass::RetirementDebt,
            footprint.domain, footprint.bytes)) {
        KisBackingBudgetDelta retirementFallback;
        addBackingDelta(&retirementFallback, KisBackingBudgetClass::RetirementDebt,
                        footprint.domain, qint64(footprint.bytes));
        reservation.commitRetaining(installed, retirementFallback);
    } else {
        reservation.commit(installed);
    }
    d->backings.find(key)->second.retirement = std::move(*preparedRetirement);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetClass KisPageOwnerLedger::backingClass(
    const KisReplicaHandle &replica) const
{
    QMutexLocker locker(&d->mutex);
    if (!replica.isValid()) return KisBackingBudgetClass::Count;
    const auto found = d->backings.find(replica.allocationIdentity());
    return found == d->backings.end() || !(found->second.replica == replica)
        ? KisBackingBudgetClass::Count : found->second.budgetClass;
}

KisPageRetirementRecordPointer KisPageOwnerLedger::takeRetirementRecord(const KisReplicaHandle &replica)
{
    QMutexLocker lock(&d->mutex);
    const auto found = d->backings.find(replica.allocationIdentity());
    if (found == d->backings.end() || !(found->second.replica == replica)) return {};
    auto record = std::move(found->second.retirement);
    if (record) {
        const auto registered = d->providers.value(providerKey(replica.provider, replica.providerEpoch));
        record->replica = replica;
        record->provider = registered.provider;
        record->backgroundRetirement = registered.backgroundRetirement;
    }
    return record;
}

bool KisPageOwnerLedger::reclassifyBacking(const KisReplicaHandle &replica,
                                           KisBackingBudgetClass budgetClass,
                                           QString *error)
{
    return reclassifyBackingImpl(replica, budgetClass, nullptr, error);
}

bool KisPageOwnerLedger::reclassifyBacking(
    const KisReplicaHandle &replica,
    KisBackingBudgetClass budgetClass,
    KisBackingBudgetReservation &reservation,
    QString *error)
{
    return reclassifyBackingImpl(replica, budgetClass, &reservation, error);
}

bool KisPageOwnerLedger::reclassifyBackingImpl(
    const KisReplicaHandle &replica,
    KisBackingBudgetClass budgetClass,
    KisBackingBudgetReservation *reservation,
    QString *error)
{
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    auto found = d->backings.find(key);
    if (found == d->backings.end() || !(found->second.replica == replica) || !d->backingBudget
        || (reservation && reservation->owner != d->backingBudget)
        || d->preparedBackingChanges.contains(key)) {
        KisPageStoreDetail::setError(error, reservation
            ? QStringLiteral("replica backing is not available for reserved reclassification")
            : QStringLiteral("replica backing is not owned by this store"));
        return false;
    }
    if (found->second.budgetClass == budgetClass) {
        if (reservation) reservation->release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    auto physical = d->physicalBackings.find(found->second.physical);
    if (physical == d->physicalBackings.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical backing is missing"));
        return false;
    }
    if (physical->domainClaim || physical->classClaim) {
        KisPageStoreDetail::setError(
            error, QStringLiteral("replica physical backing transition is active"));
        return false;
    }
    PhysicalBackingRecord next = *physical;
    const KisBackingBudgetClass beforeCharge = physical->chargedClass;
    if (!changePhysicalReferenceClass(&next, found->second.budgetClass, budgetClass)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class references are invalid"));
        return false;
    }
    const KisBackingBudgetClass afterCharge = chargedBackingClass(next);
    KisBackingBudgetDelta installed;
    if (!addPhysicalChargeChange(&installed, next, beforeCharge, afterCharge)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical class delta is invalid"));
        return false;
    }
    const KisBackingBudgetDelta needed = positiveBackingDelta(installed);
    KisBackingBudgetReservation automaticReservation;
    if (!reservation && afterCharge == KisBackingBudgetClass::RetirementDebt
        && found->second.retirementHeadroom.isValid())
        reservation = &found->second.retirementHeadroom;
    if (!reservation) {
        automaticReservation = d->backingBudget->reserveChange(installed, error);
        if (!automaticReservation.isValid()) return false;
        reservation = &automaticReservation;
    } else if (!d->backingBudget->reservationCovers(reservation->cookie, needed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation does not cover the physical class change"));
        return false;
    }
    reservation->commit(installed);
    *physical = next;
    physical->chargedClass = afterCharge;
    found->second.budgetClass = budgetClass;
    if (budgetClass != KisBackingBudgetClass::ActivePending)
        found->second.retirementHeadroom.release();
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageReadinessStatus KisPageOwnerLedger::watchRetirementBudget(
    const KisReplicaHandle &replica, KisPageReadinessCallback notify,
    KisBackingBudgetWaiter *waiter)
{
    QMutexLocker lock(&d->mutex);
    const auto key = replica.allocationIdentity();
    const auto found = d->backings.find(key);
    if (!d->backingBudget || found == d->backings.end() ||
        !(found->second.replica == replica) || d->preparedBackingChanges.contains(key) ||
        found->second.retirementHeadroom.isValid()) return KisPageReadinessStatus::Unavailable;
    const auto physical = d->physicalBackings.find(found->second.physical);
    if (physical == d->physicalBackings.end() || physical->domainClaim || physical->classClaim)
        return KisPageReadinessStatus::Unavailable;
    auto next = *physical;
    if (!changePhysicalReferenceClass(&next, found->second.budgetClass,
                                       KisBackingBudgetClass::RetirementDebt))
        return KisPageReadinessStatus::Unavailable;
    KisBackingBudgetDelta change;
    if (!addPhysicalChargeChange(&change, next, physical->chargedClass, chargedBackingClass(next)))
        return KisPageReadinessStatus::Unavailable;
    auto *budget = d->backingBudget;
    lock.unlock(); // Cold waiter/capture admission never holds the backing gate.
    return budget->waitForChange(change, std::move(notify), waiter);
}

bool KisPageOwnerLedger::retainRetirementDebtReservation(
    const KisReplicaHandle &replica,
    KisBackingBudgetReservation &reservation,
    QString *error)
{
    const KisReplicaBackingFootprint footprint = observeBackingFootprint(replica);
    QMutexLocker locker(&d->mutex);
    if (!replica.isValid() || !footprint.isValid() || !d->backingBudget
        || reservation.owner != d->backingBudget
        || !d->backingBudget->reservationCovers(
            reservation.cookie, KisBackingBudgetClass::RetirementDebt,
            footprint.domain, footprint.bytes)) {
        KisPageStoreDetail::setError(error, QStringLiteral("orphan reservation does not cover retirement debt"));
        return false;
    }
    if (d->physicalBackings.contains(physicalBackingKey(replica, footprint))) {
        reservation.release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    KisBackingBudgetDelta retained;
    addBackingDelta(&retained, KisBackingBudgetClass::RetirementDebt,
                    footprint.domain, qint64(footprint.bytes));
    if (!reservation.retainOnly(retained)) {
        KisPageStoreDetail::setError(error, QStringLiteral("orphan retirement reservation is stale"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageOwnerLedger::synchronizeBackingDomains(QString *error)
{
    struct Probe {
        QSharedPointer<KisPageReplicaProvider> provider;
        KisReplicaBackingDomainChange change;

        PhysicalBackingKey key() const
        {
            return {provider->providerId(), provider->providerEpoch(),
                    change.physicalSlot};
        }
    };
    QVector<Probe> probes;
    // QHash is implicitly shared: capture the existing registry without a
    // values() allocation, then call providers outside the owner gate. A
    // concurrent registration detaches its copy, preserving this iteration.
    ProviderIndex providers;
    {
        QMutexLocker locker(&d->mutex);
        providers = d->providers;
    }

    for (const auto &registered : std::as_const(providers)) {
        const auto &provider = registered.provider;
        if (!provider->mayHaveBackingDomainChanges()) continue;
        const auto changes = provider->backingDomainChanges();
        probes.reserve(probes.size() + changes.size());
        for (const auto &change : changes)
            probes.append({provider, change});
    }

    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return false;
    }
    QVector<Probe> acknowledged;
    for (const Probe &probe : std::as_const(probes)) {
        if (!probe.change.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain change is invalid"));
            return false;
        }
        const PhysicalBackingKey key = probe.key();
        const auto physical = d->physicalBackings.constFind(key);
        if (physical == d->physicalBackings.constEnd()) {
            // Allocation and ledger registration are separate operations. Do
            // not consume an observation before this owner has the backing.
            continue;
        }
        if (probe.change.bytes != physical->byteSize()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing observation is invalid"));
            return false;
        }
        const auto order = kisCompareBackingRevision(
            probe.change.revision, physical->domainRevision);
        if (order == KisBackingRevisionOrder::Ambiguous) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain revision is ambiguous"));
            return false;
        }
        if (order == KisBackingRevisionOrder::Older) {
            acknowledged.append(probe);
            continue;
        }
        if (order == KisBackingRevisionOrder::Same) {
            if (probe.change.domain != physical->domain) {
                KisPageStoreDetail::setError(error, QStringLiteral("provider reused a backing-domain revision"));
                return false;
            }
            acknowledged.append(probe);
            continue;
        }
        // Mutable providers must install domain/revision through the
        // pre-admission token before publishing this journal observation.
        // A newer observation here proves that an owner was skipped.
        KisPageStoreDetail::setError(
            error, QStringLiteral("provider published an unadmitted backing-domain change"));
        return false;
    }
    locker.unlock();
    for (const Probe &probe : std::as_const(acknowledged)) {
        probe.provider->acknowledgeBackingDomainChange(
            probe.change.physicalSlot, probe.change.revision);
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingClassChangeReservation KisPageOwnerLedger::prepareBackingChanges(
    QVector<KisBackingClassChange> changes,
    const QVector<KisPageTransitionEffect> &retirementEffects,
    QString *error)
{
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }

    QHash<BackingKey, KisBackingBudgetClass> targetClasses;
    targetClasses.reserve(changes.size());
    for (const auto &change : std::as_const(changes))
        targetClasses.insert(change.replica.allocationIdentity(), change.after);
    for (const KisPageTransitionEffect &effect : retirementEffects) {
        if (!effect.replica.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("publication retirement effect is invalid"));
            return {};
        }
        const BackingKey key = effect.replica.allocationIdentity();
        const auto found = d->backings.find(key);
        if (found == d->backings.end())
            continue; // a rejected, unregistered provider result owns its reservation
        const auto existing = targetClasses.constFind(key);
        if (existing != targetClasses.cend()) {
            if (existing.value() != KisBackingBudgetClass::RetirementDebt) {
                KisPageStoreDetail::setError(error, QStringLiteral("retired publication backing has a competing target class"));
                return {};
            }
            continue;
        }
        if (found->second.budgetClass != KisBackingBudgetClass::RetirementDebt) {
            changes.append({effect.replica, found->second.budgetClass,
                            KisBackingBudgetClass::RetirementDebt});
            targetClasses.insert(key, KisBackingBudgetClass::RetirementDebt);
        }
    }

    KisBackingBudgetDelta signedDelta;
    std::vector<PreparedRetirementCredit> credits;
    if (!buildPhysicalBackingDelta(changes, d->backings, d->physicalBackings,
                                   d->preparedBackingChanges,
                                   &signedDelta, &credits, error))
        return {};
    for (const auto &credit : credits) subtractBackingDelta(&signedDelta, credit.delta());
    auto reservation = d->backingBudget->reserveChange(signedDelta, error);
    if (!reservation.isValid())
        return {};
    const quint64 cookie = d->storePreparedChange(
        std::move(changes), std::move(reservation), {}, std::move(credits));
    KisPageStoreDetail::setError(error, {});
    return {this, cookie};
}

KisBackingHandoffReservation KisPageOwnerLedger::prepareBackingHandoff(
    const KisReplicaHandle &source, const KisReplicaHandle &target, QString *error)
{
    if (!source.isValid() || !target.isValid()
        || source.domain != KisPageAccessDomain::CpuRam || target.domain != source.domain
        || !(source.provider == target.provider) || !(source.providerEpoch == target.providerEpoch)
        || source.allocation.slot != target.allocation.slot
        || source.allocation.generation == std::numeric_limits<quint64>::max()
        || target.allocation.generation != source.allocation.generation + 1
        || !(source.version.key == target.version.key)
        || target.version.generation.value <= source.version.generation.value
        || !(source.layout == target.layout)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff identity is invalid"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    const auto oldKey = source.allocationIdentity();
    const auto newKey = target.allocationIdentity();
    const auto backing = d->backings.find(oldKey);
    if (!d->backingBudget || backing == d->backings.end()
        || !(backing->second.replica == source)
        || (backing->second.budgetClass != KisBackingBudgetClass::Current
            && backing->second.budgetClass != KisBackingBudgetClass::ActivePending)
        || d->backings.find(newKey) != d->backings.end()
        || d->preparedBackingChanges.contains(oldKey)
        || d->preparedBackingChanges.contains(newKey)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff source is not available"));
        return {};
    }
    const auto physical = d->physicalBackings.constFind(backing->second.physical);
    if (physical == d->physicalBackings.cend() || physical->domainClaim || physical->classClaim
        || physical->domain != source.domain || !(physical->representative == source)
        || d->sharedPhysicalMembers.contains(backing->second.physical)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff physical owner is shared or busy"));
        return {};
    }
    for (size_t i = 0; i < physical->references.size(); ++i) {
        if (physical->references[i] != (i == size_t(backing->second.budgetClass) ? 1u : 0u)) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing handoff requires one physical owner"));
            return {};
        }
    }
    KisBackingBudgetDelta change;
    if (!addPhysicalChargeChange(&change, *physical, physical->chargedClass,
                                 KisBackingBudgetClass::ActivePending)
        || (!backing->second.retirementHeadroom.isValid()
            && !addBackingDeltaChecked(&change, KisBackingBudgetClass::RetirementDebt,
                                       physical->domain, qint64(physical->byteSize())))) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing handoff delta overflows"));
        return {};
    }
    auto reservation = d->backingBudget->reserveChange(change, error);
    if (!reservation.isValid()) return {};
    const quint64 cookie = d->storePreparedChange(
        {{source, backing->second.budgetClass, KisBackingBudgetClass::ActivePending}},
        std::move(reservation), target);
    KisPageStoreDetail::setError(error, {});
    return KisBackingHandoffReservation{KisBackingClassChangeReservation{this, cookie}};
}

bool KisPageOwnerLedger::commitBackingHandoff(
    KisBackingHandoffReservation &&reservation) noexcept
{
    QMutexLocker locker(&d->mutex);
    auto &changeReservation = reservation.m_change;
    if (changeReservation.m_owner != this || !changeReservation.m_cookie) return {};
    const quint64 cookie = changeReservation.m_cookie;
    auto *slot = d->preparedChange(cookie);
    if (!slot || !slot->handoffTarget.isValid()) return {};
    Q_ASSERT(slot->changes.size() == 1 && slot->physicals.size() == 1);
    const auto &change = slot->changes.front();
    const auto target = slot->handoffTarget;
    KisBackingBudgetDelta installed;
    d->applyBackingChange(change, &installed);

    // No cardinality growth and no interleaving under mutex: insertion of the
    // same node cannot reach the table's rehash threshold. Claims exclude a
    // competing registration of the target key throughout preparation.
    auto node = d->backings.extract(change.replica.allocationIdentity());
    Q_ASSERT(!node.empty());
    node.key() = target.allocationIdentity();
    node.mapped().replica = target;
    auto inserted = d->backings.insert(std::move(node));
    Q_ASSERT(inserted.inserted);
    auto physical = d->physicalBackings.find(inserted.position->second.physical);
    Q_ASSERT(physical != d->physicalBackings.end() && physical->classClaim == cookie);
    physical->representative = target;

    if (inserted.position->second.retirementHeadroom.isValid()) {
        slot->reservation.commit(installed);
    } else {
        KisBackingBudgetDelta retained;
        addBackingDelta(&retained, KisBackingBudgetClass::RetirementDebt,
                         physical->domain, qint64(physical->byteSize()));
        slot->reservation.commitRetaining(installed, retained);
        inserted.position->second.retirementHeadroom = std::move(slot->reservation);
    }
    d->releasePreparedClaims(*slot, cookie);
    slot->changes.clear();
    d->freePreparedChanges.push_back(quint32(cookie));
    changeReservation.m_owner = nullptr;
    changeReservation.m_cookie = 0;
    return true;
}

bool KisPageOwnerLedger::prepareRetirementDebt(
    const QVector<KisPageTransitionEffect> &effects,
    quint64 *cookie,
    QString *error)
{
    if (!cookie) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement debt cookie output is missing"));
        return false;
    }
    *cookie = 0;
    auto reservation = prepareBackingChanges({}, effects, error);
    if (!reservation.isValid())
        return false;
    *cookie = std::exchange(reservation.m_cookie, 0);
    reservation.m_owner = nullptr;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageOwnerLedger::commitRetirementDebt(quint64 cookie) noexcept
{
    commitPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::cancelRetirementDebt(quint64 cookie) noexcept
{
    cancelPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::commitPreparedBackingChanges(quint64 cookie) noexcept
{
    QMutexLocker locker(&d->mutex);
    const quint32 slotIndex = quint32(cookie);
    PreparedBackingChangeSlot *slot = d->preparedChange(cookie);
    if (!slot || slot->handoffTarget.isValid()) return;

    KisBackingBudgetDelta signedDelta;
    for (const KisBackingClassChange &change : std::as_const(slot->changes)) {
        const BackingKey key = change.replica.allocationIdentity();
        Q_ASSERT(d->preparedBackingChanges.contains(key));
        d->applyBackingChange(change, &signedDelta);
    }
    for (const auto &credit : slot->retirementCredits) {
        auto found = d->backings.find(credit.backing);
        Q_ASSERT(found != d->backings.end() && found->second.retirementHeadroom.isValid());
        const auto funded = credit.delta();
        found->second.retirementHeadroom.commit(funded);
        subtractBackingDelta(&signedDelta, funded);
    }
    slot->reservation.commit(signedDelta);
    for (const auto &change : std::as_const(slot->changes)) {
        if (change.after != KisBackingBudgetClass::ActivePending)
            d->backings.find(change.replica.allocationIdentity())->second.retirementHeadroom.release();
    }
    d->releasePreparedClaims(*slot, cookie);
    slot->changes.clear();
    slot->retirementCredits.clear();
    d->freePreparedChanges.push_back(slotIndex);
}

void KisPageOwnerLedger::cancelPreparedBackingChanges(quint64 cookie) noexcept
{
    QMutexLocker locker(&d->mutex);
    const quint32 slotIndex = quint32(cookie);
    PreparedBackingChangeSlot *slot = d->preparedChange(cookie);
    if (!slot) return;
    slot->reservation.release();
    d->releasePreparedClaims(*slot, cookie);
    slot->changes.clear();
    slot->retirementCredits.clear();
    d->freePreparedChanges.push_back(slotIndex);
}

void KisPageOwnerLedger::commitBackingChanges(
    KisBackingClassChangeReservation &&reservation) noexcept
{
    Q_ASSERT(reservation.m_owner == this && reservation.m_cookie);
    if (reservation.m_owner != this || !reservation.m_cookie)
        return;
    const quint64 cookie = std::exchange(reservation.m_cookie, 0);
    reservation.m_owner = nullptr;
    commitPreparedBackingChanges(cookie);
}

void KisPageOwnerLedger::releaseRetiredBacking(const KisReplicaHandle &replica) noexcept
{
    BackingRecord released;
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    Q_ASSERT(!d->preparedBackingChanges.contains(key));
    auto found = d->backings.find(key);
    if (found == d->backings.end() || !(found->second.replica == replica)) return;
    while (true) {
        const auto physical = d->physicalBackings.constFind(found->second.physical);
        if (physical == d->physicalBackings.cend()
            || (!physical->domainClaim && !physical->classClaim))
            break;
        d->physicalClaimsChanged.wait(&d->mutex);
        found = d->backings.find(key);
        if (found == d->backings.end() || !(found->second.replica == replica))
            return;
    }
    released = std::move(found->second);
    const auto &record = released;
    d->backings.erase(found);
    auto physical = d->physicalBackings.find(record.physical);
    Q_ASSERT(physical != d->physicalBackings.end());
    if (physical == d->physicalBackings.end())
        return;
    auto &references = physical->references[static_cast<size_t>(record.budgetClass)];
    Q_ASSERT(references != 0);
    if (references)
        --references;
    const KisBackingBudgetClass afterCharge = chargedBackingClass(*physical);
    if (afterCharge == KisBackingBudgetClass::Count) {
        d->sharedPhysicalMembers.remove(record.physical);
        if (d->backingBudget) {
            d->backingBudget->releaseLive(physical->chargedClass,
                                          physical->domain,
                                          physical->byteSize());
        }
        d->physicalBackings.erase(physical);
        return;
    }
    auto members = d->sharedPhysicalMembers.find(record.physical);
    Q_ASSERT(members != d->sharedPhysicalMembers.end());
    if (members != d->sharedPhysicalMembers.end()) {
        members->remove(key);
        if (physical->representative == record.replica) {
            Q_ASSERT(!members->isEmpty());
            const auto replacement = d->backings.find(*members->constBegin());
            Q_ASSERT(replacement != d->backings.end());
            if (replacement != d->backings.end())
                physical->representative = replacement->second.replica;
        }
        if (members->size() == 1)
            d->sharedPhysicalMembers.erase(members);
    }
    // Retirement admission already exposed and budgeted every surviving
    // owner class before provider destruction. Removing a terminal Debt alias
    // therefore cannot reveal an unprepared class here.
    Q_ASSERT(afterCharge == physical->chargedClass);
}

bool KisPageOwnerLedger::registerProvider(
    const QSharedPointer<KisPageReplicaProvider> &provider,
    QString *error)
{
    const auto capabilities = provider ? provider->capabilities() : KisReplicaCapabilities{};
    if (!provider || !provider->providerId().isValid() ||
        !provider->providerEpoch().isValid() || !capabilities.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity or capabilities are invalid"));
        return false;
    }
    const KisReplicaProviderId id = provider->providerId();
    const KisReplicaProviderEpoch epoch = provider->providerEpoch();
    const ProviderKey key = providerKey(id, epoch);
    const auto validateRegistration = [&]() {
        if (!d->completions) {
            KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is not configured"));
            return false;
        }
        if (d->providers.contains(key)
            || d->pendingProviderRegistrations.contains(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity is already registered"));
            return false;
        }
        // One active epoch per stable provider ID. Re-registration after
        // restart requires a new ledger so stale handles fail closed.
        for (auto registered = d->providers.cbegin();
             registered != d->providers.cend(); ++registered) {
            if (registered.key().first == id.value) {
                KisPageStoreDetail::setError(error, QStringLiteral("replica provider ID already has an active epoch"));
                return false;
            }
        }
        for (const ProviderKey &pending :
             std::as_const(d->pendingProviderRegistrations)) {
            if (pending.first == id.value) {
                KisPageStoreDetail::setError(
                    error,
                    QStringLiteral("replica provider ID registration is already active"));
                return false;
            }
        }
        return true;
    };
    {
        QMutexLocker locker(&d->mutex);
        if (!validateRegistration())
            return false;
        d->pendingProviderRegistrations.insert(key);
    }
    // Provider code remains outside the owner gate. Mutable-domain providers
    // retain only a weak reference to this ledger admission authority.
    if (!provider->registerBackingDomainAdmission(d->domainAdmission, error)) {
        QMutexLocker locker(&d->mutex);
        d->pendingProviderRegistrations.remove(key);
        return false;
    }
    QMutexLocker locker(&d->mutex);
    const bool removed = d->pendingProviderRegistrations.remove(key);
    Q_ASSERT(removed);
    Q_UNUSED(removed);
    d->providers.insert(key, {provider, capabilities.backgroundRetirement});
    KisPageStoreDetail::setError(error, {});
    return true;
}

QSharedPointer<KisPageReplicaProvider> KisPageOwnerLedger::provider(
    KisReplicaProviderId providerId,
    KisReplicaProviderEpoch epoch) const
{
    QMutexLocker locker(&d->mutex);
    return d->providers.value(providerKey(providerId, epoch)).provider;
}

QSharedPointer<KisPageReplicaProvider> KisPageOwnerLedger::providerFor(
    KisPageAccessRequirement access) const
{
    if (!access.isValid()) return {};
    QList<ProviderRegistration> providers;
    {
        QMutexLocker locker(&d->mutex);
        providers = d->providers.values();
    }
    QSharedPointer<KisPageReplicaProvider> selected;
    for (const auto &registered : providers) {
        const auto &candidate = registered.provider;
        if (!candidate->capabilities().supports(access)) continue;
        if (selected) {
            // Route selection must be explicit when more than one provider
            // advertises the same consumer access requirement.
            return {};
        }
        selected = candidate;
    }
    return selected;
}

KisPageRequestId KisPageOwnerLedger::nextRequestId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageRequestId>(&s_nextRequestId)
                          : KisPageRequestId();
}

KisPageOperationId KisPageOwnerLedger::nextOperationId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageOperationId>(&s_nextOperationId)
                          : KisPageOperationId();
}

KisPageLeaseId KisPageOwnerLedger::nextLeaseId()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageLeaseId>(&s_nextLeaseId)
                          : KisPageLeaseId();
}

KisPageWriterToken KisPageOwnerLedger::nextWriterToken()
{
    QMutexLocker locker(&d->mutex);
    return d->completions ? KisPageStoreDetail::allocateMonotonicId<KisPageWriterToken>(&s_nextWriterToken)
                          : KisPageWriterToken();
}

bool KisPageOwnerLedger::bindProviderOperation(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error)
{
    return bindProviderOperationImpl(operation, result, false, error);
}

KisVerifiedCompletion KisPageOwnerLedger::consumeTerminalProviderOperation(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error)
{
    if (!bindProviderOperation(operation, result, error))
        return {};
    const KisVerifiedCompletion verified = verifyProviderOperation(operation, error);
    if (!verified.isValid() || !releaseTerminalProviderOperation(operation, error))
        return {};
    return verified;
}

bool KisPageOwnerLedger::bindRetirementOperation(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    QString *error)
{
    QMutexLocker lock(&d->mutex);
    auto found = d->operations.find(operation.value);
    if (found == d->operations.end()) {
        lock.unlock();
        return bindProviderOperationImpl(operation, result, true, error);
    }
    if (!d->completions || found == d->operations.end() ||
        !found->second.detachedRetirement || found->second.completion.isValid() ||
        !result.isValid() || !(result.operation == operation) ||
        !d->providers.contains(providerKey(result.replica.provider, result.replica.providerEpoch))) {
        KisPageStoreDetail::setError(error, QStringLiteral("retirement operation was not prepared or result mismatched"));
        return false;
    }
    found->second.completion = result.completion;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageOperationId KisPageOwnerLedger::prepareRetirementOperation(KisPageRetirementRecord &record)
{
    QMutexLocker lock(&d->mutex);
    if (!d->completions || record.operationStorage.empty()) return {};
    if (!d->operations.empty() && d->operations.get_allocator() != record.operationStorage.get_allocator())
        return {}; // A node handle must retain the same accounting allocator.
    const auto operation = KisPageStoreDetail::allocateMonotonicId<KisPageOperationId>(&s_nextOperationId);
    if (!operation.isValid()) return {};
    record.operationStorage.key() = operation.value;
    record.operationStorage.mapped() = {{}, true, &record};
    if (d->operations.empty() && d->operations.get_allocator() != record.operationStorage.get_allocator())
        d->operations = OperationIndex(std::less<quint64>{}, record.operationStorage.get_allocator());
    const auto inserted = d->operations.insert(std::move(record.operationStorage));
    Q_ASSERT(inserted.inserted);
    return operation;
}

void KisPageOwnerLedger::cancelRetirementOperation(KisPageOperationId operation)
{
    QMutexLocker lock(&d->mutex);
    auto found = d->operations.find(operation.value);
    Q_ASSERT(found != d->operations.end() && !found->second.completion.isValid());
    if (found != d->operations.end() && !found->second.completion.isValid()) {
        auto *record = found->second.retirement;
        Q_ASSERT(record);
        record->operationStorage = d->operations.extract(found);
    }
}

bool KisPageOwnerLedger::bindProviderOperationImpl(
    KisPageOperationId operation,
    const KisReplicaOperation &result,
    bool detachedRetirement,
    QString *error)
{
    if (!operation.isValid() || !result.isValid() ||
        !(result.operation == operation)) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation result is invalid or mismatched"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->operations.find(operation.value) != d->operations.end() ||
        !d->providers.contains(providerKey(result.replica.provider,
                                           result.replica.providerEpoch))) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is duplicate or foreign"));
        return false;
    }
    try {
        if (d->operations.empty())
            d->operations = OperationIndex(std::less<quint64>{},
                KisMutationStorageAllocator<std::pair<const quint64, ProviderOperationRecord>>::retained(d->backingBudget));
        d->operations.emplace(operation.value, ProviderOperationRecord{result.completion, detachedRetirement});
    }
    catch (const std::bad_alloc &) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation storage was refused"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisVerifiedCompletion KisPageOwnerLedger::verifyProviderOperation(
    KisPageOperationId operation,
    QString *error) const
{
    QSharedPointer<KisCompletionRegistry> completions;
    const KisCompletionTicket completion = d->operationCompletion(operation, &completions);
    if (!completion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is unknown"));
        return {};
    }
    const KisVerifiedCompletion verified = completions->verifyTerminal(completion);
    if (!verified.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is not terminal"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return verified;
}

bool KisPageOwnerLedger::releaseTerminalProviderOperation(
    KisPageOperationId operation,
    QString *error)
{
    QSharedPointer<KisCompletionRegistry> completions;
    const KisCompletionTicket completion = d->operationCompletion(operation, &completions);
    if (!completion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is unknown"));
        return false;
    }
    if (!completions || !completions->verifyTerminal(completion).isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is not terminal"));
        return false;
    }
    QMutexLocker locker(&d->mutex);
    auto operationIt = d->operations.find(operation.value);
    if (!d->completions || operationIt == d->operations.end() ||
        !(operationIt->second.completion == completion)) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation changed before release"));
        return false;
    }
    if (auto *record = operationIt->second.retirement)
        record->operationStorage = d->operations.extract(operationIt);
    else
        d->operations.erase(operationIt);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageReadinessStatus KisPageOwnerLedger::watchProviderOperation(
    KisPageOperationId operation, KisPageReadinessCallback scheduleReady,
    KisPageReadinessSubscription *subscription) const
{
    QSharedPointer<KisCompletionRegistry> registry;
    const auto ticket = d->operationCompletion(operation, &registry);
    return registry && ticket.isValid()
        ? registry->watchTerminal(ticket, std::move(scheduleReady), subscription)
        : KisPageReadinessStatus::Unavailable;
}

KisPageReadinessStatus KisPageOwnerLedger::watchCompletion(
    const KisCompletionTicket &ticket, KisPageReadinessCallback scheduleReady,
    KisPageReadinessSubscription *subscription) const
{
    QSharedPointer<KisCompletionRegistry> registry;
    {
        QMutexLocker lock(&d->mutex);
        registry = d->completions;
    }
    return registry ? registry->watchTerminal(ticket, std::move(scheduleReady), subscription)
                    : KisPageReadinessStatus::Unavailable;
}

qsizetype KisPageOwnerLedger::providerOperationCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->operations.size();
}

qsizetype KisPageOwnerLedger::publicationBlockingOperationCount() const
{
    QMutexLocker locker(&d->mutex);
    return std::count_if(d->operations.cbegin(), d->operations.cend(),
                         [](const auto &entry) {
                             return !entry.second.detachedRetirement;
                         });
}

qsizetype KisPageOwnerLedger::sealedProofCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->sealedProofs.size();
}

bool KisPageOwnerLedger::ownsPreparedPageProof(
    const KisPreparedPageProof &proof) const
{
    if (!proof.isValid())
        return false;
    QMutexLocker locker(&d->mutex);
    const auto found = d->sealedProofs.constFind(proof.providerValidationStamp);
    return found != d->sealedProofs.cend() && found.value() == proof;
}

bool KisPageOwnerLedger::sealPreparedPage(
    const KisPageMetadataCoordinator &metadata,
    const KisPageVersion &version,
    KisPageTransactionId transaction,
    const KisPageAllocationDescriptor &descriptor,
    const KisCompletionTicket &producerCompletion,
    KisPreparedPageProof *proof,
    QString *error)
{
    if (!proof || !version.isValid() || !transaction.isValid() ||
        !descriptor.isValid() || !producerCompletion.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof request is invalid"));
        return false;
    }

    QSharedPointer<KisCompletionRegistry> completions;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->completions) {
            KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is not configured"));
            return false;
        }
        completions = d->completions;
    }
    const KisVerifiedCompletion verified =
        completions->verifyTerminal(producerCompletion);
    if (!verified.isValid() || !verified.succeeded()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page producer did not complete successfully"));
        return false;
    }

    KisPageStateSnapshot snapshot;
    if (!metadata.versionSnapshot(version, &snapshot)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page metadata is unavailable"));
        return false;
    }
    const KisPageVersionStateSnapshot *prepared = snapshot.findVersion(version);
    if (!prepared || prepared->publication != KisPagePublicationState::Prepared ||
        !(prepared->preparedBy == transaction) || !prepared->authority.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("page is not prepared by the requested transaction"));
        return false;
    }

    const QSharedPointer<KisPageReplicaProvider> authorityProvider =
        provider(prepared->authority.provider, prepared->authority.providerEpoch);
    if (!authorityProvider ||
        !authorityProvider->validate(prepared->authority, descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page authority failed provider validation"));
        return false;
    }

    KisPreparedPageProof sealed;
    sealed.transaction = transaction;
    sealed.authority = prepared->authority;
    sealed.producerCompletion = producerCompletion;
    QMutexLocker locker(&d->mutex);
    if (!d->completions || d->nextValidationStamp == 0 ||
        d->nextValidationStamp == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof could not be sealed"));
        return false;
    }
    sealed.providerValidationStamp = d->nextValidationStamp++;
    if (!sealed.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof could not be sealed"));
        return false;
    }
    d->sealedProofs.insert(sealed.providerValidationStamp, sealed);
    locker.unlock();
    *proof = sealed;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageOwnerLedger::validatePreparedPage(
    const KisPageMetadataCoordinator &metadata,
    const KisPreparedPageProof &proof,
    const KisPageAllocationDescriptor &descriptor,
    QString *error) const
{
    if (!proof.isValid() || !descriptor.isValid() ||
        !proof.authority.layout.matches(descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof or layout is invalid"));
        return false;
    }
    QSharedPointer<KisCompletionRegistry> completions;
    {
        QMutexLocker locker(&d->mutex);
        const auto sealedIt =
            d->sealedProofs.constFind(proof.providerValidationStamp);
        if (!d->completions || sealedIt == d->sealedProofs.constEnd() ||
            !(sealedIt.value() == proof)) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof was not sealed by this owner"));
            return false;
        }
        completions = d->completions;
    }
    const KisVerifiedCompletion verified =
        completions ? completions->verifyTerminal(proof.producerCompletion)
                    : KisVerifiedCompletion();
    if (!verified.isValid() || !verified.succeeded()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof completion is not successful"));
        return false;
    }

    KisPageStateSnapshot snapshot;
    const KisPageVersionStateSnapshot *prepared = nullptr;
    if (metadata.versionSnapshot(proof.authority.version, &snapshot)) {
        prepared = snapshot.findVersion(proof.authority.version);
    }
    if (!prepared || prepared->publication != KisPagePublicationState::Prepared ||
        !(prepared->preparedBy == proof.transaction) ||
        !(prepared->authority == proof.authority)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof no longer matches metadata"));
        return false;
    }
    const QSharedPointer<KisPageReplicaProvider> authorityProvider =
        provider(proof.authority.provider, proof.authority.providerEpoch);
    if (!authorityProvider ||
        !authorityProvider->validate(prepared->authority, descriptor)) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page proof no longer matches provider state"));
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageOwnerLedger::revokePreparedPage(const KisPreparedPageProof &proof)
{
    if (!proof.isValid()) return false;
    QMutexLocker locker(&d->mutex);
    auto proofIt = d->sealedProofs.find(proof.providerValidationStamp);
    if (!d->completions || proofIt == d->sealedProofs.end() ||
        !(proofIt.value() == proof)) {
        return false;
    }
    d->sealedProofs.erase(proofIt);
    return true;
}
