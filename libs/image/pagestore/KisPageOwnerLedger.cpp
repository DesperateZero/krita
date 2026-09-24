/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageOwnerLedger.h"
#include "KisPageWriteCoordinator_p.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <limits>

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

struct ProviderOperationRecord
{
    KisCompletionTicket completion;
    bool detachedRetirement = false;
};

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
};

struct PhysicalBackingRecord
{
    KisReplicaHandle representative;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
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

struct PreparedRetirementSlot
{
    QVector<KisBackingClassChange> changes;
    KisBackingBudgetReservation reservation;
    quint32 generation = 0;
};

bool buildPhysicalBackingDelta(
    const QVector<KisBackingClassChange> &changes,
    const QHash<BackingKey, BackingRecord> &backings,
    const QHash<PhysicalBackingKey, PhysicalBackingRecord> &physicalBackings,
    const QSet<BackingKey> &preparedRetirements,
    KisBackingBudgetDelta *signedDelta,
    QString *error)
{
    QHash<PhysicalBackingKey, PhysicalBackingRecord> projected;
    QSet<BackingKey> classified;
    classified.reserve(changes.size());
    for (const KisBackingClassChange &change : changes) {
        const BackingKey key = change.replica.allocationIdentity();
        const auto found = backings.constFind(key);
        if (!change.replica.isValid() || found == backings.constEnd()
            || preparedRetirements.contains(key)
            || found->budgetClass != change.before
            || !(found->replica == change.replica)
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
        auto physical = projected.find(found->physical);
        if (physical == projected.end()) {
            const auto source = physicalBackings.constFind(found->physical);
            if (source == physicalBackings.constEnd()) {
                KisPageStoreDetail::setError(error, QStringLiteral("physical backing owner is missing"));
                return false;
            }
            physical = projected.insert(found->physical, *source);
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
        const auto found = operations.constFind(operation.value);
        if (!completions || !operation.isValid() || found == operations.constEnd())
            return {};
        *registry = completions;
        return found->completion;
    }

    PreparedRetirementSlot *preparedRetirement(quint64 cookie)
    {
        const quint32 slotIndex = quint32(cookie);
        if (slotIndex >= preparedRetirements.size())
            return nullptr;
        auto &slot = preparedRetirements.at(slotIndex);
        return slot.reservation.isValid() && slot.generation == quint32(cookie >> 32)
            ? &slot : nullptr;
    }

    void applyBackingChange(const KisBackingClassChange &change,
                            KisBackingBudgetDelta *signedDelta) noexcept
    {
        auto found = backings.find(change.replica.allocationIdentity());
        Q_ASSERT(found != backings.end());
        Q_ASSERT(found->budgetClass == change.before);
        auto physical = physicalBackings.find(found->physical);
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
        found->budgetClass = change.after;
    }

    mutable QMutex mutex;
    QSharedPointer<KisCompletionRegistry> completions;
    quint64 nextValidationStamp = 1;
    QHash<ProviderKey, QSharedPointer<KisPageReplicaProvider>> providers;
    QHash<quint64, ProviderOperationRecord> operations;
    QHash<quint64, KisPreparedPageProof> sealedProofs;
    KisBackingBudgetController *backingBudget = nullptr;
    QHash<BackingKey, BackingRecord> backings;
    QHash<PhysicalBackingKey, PhysicalBackingRecord> physicalBackings;
    // Lookup only for physically shared backing; singleton uses representative.
    QHash<PhysicalBackingKey, QSet<BackingKey>> sharedPhysicalMembers;
    QSet<BackingKey> preparedRetirementBackings;
    std::vector<PreparedRetirementSlot> preparedRetirements;
    std::vector<quint32> freePreparedRetirements;
};

KisPageOwnerLedger::KisPageOwnerLedger()
    : d(new Private)
{
}

KisPageOwnerLedger::~KisPageOwnerLedger() = default;

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
    d->backingBudget = &budget;
}

KisReplicaBackingFootprint KisPageOwnerLedger::observeBackingFootprint(
    const KisReplicaHandle &replica) const
{
    QSharedPointer<KisPageReplicaProvider> replicaProvider;
    {
        QMutexLocker locker(&d->mutex);
        replicaProvider = d->providers.value(providerKey(replica.provider,
                                                         replica.providerEpoch));
    }
    return replicaProvider ? replicaProvider->backingFootprint(replica)
                           : KisReplicaBackingFootprint{};
}

bool KisPageOwnerLedger::registerBacking(const KisReplicaHandle &replica,
                                         KisBackingBudgetReservation &reservation,
                                         KisBackingBudgetClass budgetClass,
                                         QString *error)
{
    if (!replica.isValid() || !reservation.isValid()
        || static_cast<size_t>(budgetClass) >= static_cast<size_t>(KisBackingBudgetClass::Count)
        || replica.layout.byteSize > quint64(std::numeric_limits<qint64>::max())) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica exceeds its backing reservation"));
        return false;
    }
    const KisReplicaBackingFootprint footprint = observeBackingFootprint(replica);
    if (!footprint.isValid() || footprint.bytes != replica.layout.byteSize) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing footprint is invalid"));
        return false;
    }

    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    const PhysicalBackingKey physicalKey = physicalBackingKey(replica, footprint);
    if (!d->backingBudget || reservation.owner != d->backingBudget
        || d->backings.contains(key)) {
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
        record.references[static_cast<size_t>(budgetClass)] = 1;
        record.chargedClass = budgetClass;
        physical = d->physicalBackings.insert(physicalKey, record);
    } else {
        if (physical->domain != footprint.domain || physical->byteSize() != footprint.bytes) {
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
    d->backings.insert(key, {replica, budgetClass, physicalKey});

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
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetClass KisPageOwnerLedger::backingClass(
    const KisReplicaHandle &replica) const
{
    QMutexLocker locker(&d->mutex);
    if (!replica.isValid()) return KisBackingBudgetClass::Count;
    const auto found = d->backings.constFind(replica.allocationIdentity());
    return found == d->backings.constEnd()
        ? KisBackingBudgetClass::Count : found->budgetClass;
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
    if (found == d->backings.end() || !(found->replica == replica) || !d->backingBudget
        || (reservation && reservation->owner != d->backingBudget)
        || d->preparedRetirementBackings.contains(key)) {
        KisPageStoreDetail::setError(error, reservation
            ? QStringLiteral("replica backing is not available for reserved reclassification")
            : QStringLiteral("replica backing is not owned by this store"));
        return false;
    }
    if (found->budgetClass == budgetClass) {
        if (reservation) reservation->release();
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    auto physical = d->physicalBackings.find(found->physical);
    if (physical == d->physicalBackings.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica physical backing is missing"));
        return false;
    }
    PhysicalBackingRecord next = *physical;
    const KisBackingBudgetClass beforeCharge = physical->chargedClass;
    if (!changePhysicalReferenceClass(&next, found->budgetClass, budgetClass)) {
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
    if (!reservation) {
        automaticReservation = d->backingBudget->reserve(needed, error);
        if (!automaticReservation.isValid()) return false;
        reservation = &automaticReservation;
    } else if (!d->backingBudget->reservationCovers(reservation->cookie, needed)) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation does not cover the physical class change"));
        return false;
    }
    reservation->commit(installed);
    *physical = next;
    physical->chargedClass = afterCharge;
    found->budgetClass = budgetClass;
    KisPageStoreDetail::setError(error, {});
    return true;
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
    QList<QSharedPointer<KisPageReplicaProvider>> providers;
    {
        QMutexLocker locker(&d->mutex);
        providers = d->providers.values();
    }

    for (const auto &provider : std::as_const(providers)) {
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
    KisBackingBudgetDelta signedDelta;
    bool hasChanges = false;
    for (const Probe &probe : std::as_const(probes)) {
        if (!probe.change.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider backing-domain change is invalid"));
            return false;
        }
        const auto physical = d->physicalBackings.constFind(probe.key());
        if (physical == d->physicalBackings.constEnd()) {
            continue;
        }
        if (probe.change.bytes != physical->byteSize()) {
            KisPageStoreDetail::setError(error, QStringLiteral("provider physical backing observation is invalid"));
            return false;
        }
        if (probe.change.domain == physical->domain)
            continue;
        const qint64 bytes = qint64(physical->byteSize());
        if (!addBackingDeltaChecked(&signedDelta, physical->chargedClass,
                                    physical->domain, -bytes)
            || !addBackingDeltaChecked(&signedDelta, physical->chargedClass,
                                       probe.change.domain, bytes)) {
            KisPageStoreDetail::setError(error, QStringLiteral("physical backing domain delta overflows"));
            return false;
        }
        hasChanges = true;
    }
    if (hasChanges) {
        auto reservation = d->backingBudget->reserve(positiveBackingDelta(signedDelta), error);
        if (!reservation.isValid())
            return false;
        reservation.commit(signedDelta);
        for (const Probe &probe : std::as_const(probes)) {
            auto physical = d->physicalBackings.find(probe.key());
            if (physical != d->physicalBackings.end()
                && probe.change.isValid()) {
                physical->domain = probe.change.domain;
            }
        }
    }
    locker.unlock();
    for (const Probe &probe : std::as_const(probes)) {
        probe.provider->acknowledgeBackingDomainChange(
            probe.change.physicalSlot, probe.change.sequence);
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisBackingBudgetReservation KisPageOwnerLedger::prepareBackingChanges(
    QVector<KisBackingClassChange> *changes,
    const QVector<KisPageTransitionEffect> &retirementEffects,
    QString *error)
{
    if (!changes) {
        KisPageStoreDetail::setError(error, QStringLiteral("publication backing change output is missing"));
        return {};
    }
    if (!synchronizeBackingDomains(error)) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return {};
    }

    QHash<BackingKey, KisBackingBudgetClass> targetClasses;
    targetClasses.reserve(changes->size());
    for (const auto &change : std::as_const(*changes))
        targetClasses.insert(change.replica.allocationIdentity(), change.after);
    for (const KisPageTransitionEffect &effect : retirementEffects) {
        if (!effect.replica.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("publication retirement effect is invalid"));
            return {};
        }
        const BackingKey key = effect.replica.allocationIdentity();
        const auto found = d->backings.constFind(key);
        if (found == d->backings.constEnd())
            continue; // a rejected, unregistered provider result owns its reservation
        const auto existing = targetClasses.constFind(key);
        if (existing != targetClasses.cend()) {
            if (existing.value() != KisBackingBudgetClass::RetirementDebt) {
                KisPageStoreDetail::setError(error, QStringLiteral("retired publication backing has a competing target class"));
                return {};
            }
            continue;
        }
        if (found->budgetClass != KisBackingBudgetClass::RetirementDebt) {
            changes->append({effect.replica, found->budgetClass,
                             KisBackingBudgetClass::RetirementDebt});
            targetClasses.insert(key, KisBackingBudgetClass::RetirementDebt);
        }
    }

    KisBackingBudgetDelta signedDelta;
    if (!buildPhysicalBackingDelta(*changes, d->backings, d->physicalBackings,
                                   d->preparedRetirementBackings,
                                   &signedDelta, error))
        return {};
    return d->backingBudget->reserve(positiveBackingDelta(signedDelta), error);
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
    if (!synchronizeBackingDomains(error)) return false;
    QMutexLocker locker(&d->mutex);
    if (!d->backingBudget) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing budget is not attached"));
        return false;
    }

    QVector<KisBackingClassChange> changes;
    changes.reserve(effects.size());
    QSet<BackingKey> seen;
    seen.reserve(effects.size());
    for (const KisPageTransitionEffect &effect : effects) {
        if (!effect.replica.isValid()) {
            KisPageStoreDetail::setError(error, QStringLiteral("retirement effect is invalid"));
            return false;
        }
        const BackingKey key = effect.replica.allocationIdentity();
        const auto found = d->backings.constFind(key);
        if (found == d->backings.constEnd())
            continue;
        if (d->preparedRetirementBackings.contains(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica already has a prepared retirement"));
            return false;
        }
        if (seen.contains(key)) {
            continue;
        }
        if (!(found->replica == effect.replica)) {
            KisPageStoreDetail::setError(error, QStringLiteral("retirement backing no longer matches its owner record"));
            return false;
        }
        if (found->budgetClass == KisBackingBudgetClass::RetirementDebt)
            continue;
        changes.append({effect.replica, found->budgetClass,
                        KisBackingBudgetClass::RetirementDebt});
        seen.insert(key);
    }

    KisBackingBudgetDelta signedDelta;
    if (!buildPhysicalBackingDelta(changes, d->backings, d->physicalBackings,
                                   d->preparedRetirementBackings,
                                   &signedDelta, error))
        return false;
    auto reservation = d->backingBudget->reserve(positiveBackingDelta(signedDelta), error);
    if (!reservation.isValid())
        return false;

    quint32 slotIndex = 0;
    if (d->freePreparedRetirements.empty()) {
        slotIndex = quint32(d->preparedRetirements.size());
        if (d->freePreparedRetirements.capacity() < d->preparedRetirements.size() + 1) {
            d->freePreparedRetirements.reserve(
                std::max(d->preparedRetirements.size() + 1,
                         d->freePreparedRetirements.capacity()
                             ? d->freePreparedRetirements.capacity() * 2 : size_t(1)));
        }
        d->preparedRetirements.emplace_back();
    } else {
        slotIndex = d->freePreparedRetirements.back();
        d->freePreparedRetirements.pop_back();
    }
    PreparedRetirementSlot &slot = d->preparedRetirements.at(slotIndex);
    ++slot.generation;
    if (!slot.generation)
        ++slot.generation;
    slot.changes = std::move(changes);
    slot.reservation = std::move(reservation);
    for (const KisBackingClassChange &change : std::as_const(slot.changes))
        d->preparedRetirementBackings.insert(change.replica.allocationIdentity());
    *cookie = (quint64(slot.generation) << 32) | slotIndex;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisPageOwnerLedger::commitRetirementDebt(quint64 cookie) noexcept
{
    QMutexLocker locker(&d->mutex);
    const quint32 slotIndex = quint32(cookie);
    PreparedRetirementSlot *slot = d->preparedRetirement(cookie);
    if (!slot) return;

    KisBackingBudgetDelta signedDelta;
    for (const KisBackingClassChange &change : std::as_const(slot->changes)) {
        const BackingKey key = change.replica.allocationIdentity();
        Q_ASSERT(d->preparedRetirementBackings.contains(key));
        d->applyBackingChange(change, &signedDelta);
        d->preparedRetirementBackings.remove(key);
    }
    slot->reservation.commit(signedDelta);
    slot->changes.clear();
    d->freePreparedRetirements.push_back(slotIndex);
}

void KisPageOwnerLedger::cancelRetirementDebt(quint64 cookie) noexcept
{
    QMutexLocker locker(&d->mutex);
    const quint32 slotIndex = quint32(cookie);
    PreparedRetirementSlot *slot = d->preparedRetirement(cookie);
    if (!slot) return;
    for (const KisBackingClassChange &change : std::as_const(slot->changes))
        d->preparedRetirementBackings.remove(change.replica.allocationIdentity());
    slot->reservation.release();
    slot->changes.clear();
    d->freePreparedRetirements.push_back(slotIndex);
}

void KisPageOwnerLedger::commitBackingChanges(
    KisBackingBudgetReservation &&reservation,
    const QVector<KisBackingClassChange> &changes) noexcept
{
    QMutexLocker locker(&d->mutex);
    Q_ASSERT(reservation.isValid());
    KisBackingBudgetDelta signedDelta;
    for (const KisBackingClassChange &change : changes) {
        const BackingKey key = change.replica.allocationIdentity();
        Q_ASSERT(!d->preparedRetirementBackings.contains(key));
        d->applyBackingChange(change, &signedDelta);
    }
    reservation.commit(signedDelta);
}

void KisPageOwnerLedger::releaseRetiredBacking(const KisReplicaHandle &replica) noexcept
{
    QMutexLocker locker(&d->mutex);
    const BackingKey key = replica.allocationIdentity();
    Q_ASSERT(!d->preparedRetirementBackings.contains(key));
    auto found = d->backings.find(key);
    if (found == d->backings.end() || !(found->replica == replica)) return;
    const BackingRecord record = *found;
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
            const auto replacement = d->backings.constFind(*members->constBegin());
            Q_ASSERT(replacement != d->backings.constEnd());
            if (replacement != d->backings.constEnd())
                physical->representative = replacement->replica;
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
    if (!provider || !provider->providerId().isValid() ||
        !provider->providerEpoch().isValid() ||
        !provider->capabilities().isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity or capabilities are invalid"));
        return false;
    }
    const KisReplicaProviderId id = provider->providerId();
    const KisReplicaProviderEpoch epoch = provider->providerEpoch();
    QMutexLocker locker(&d->mutex);
    if (!d->completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("owner ledger is not configured"));
        return false;
    }
    const ProviderKey key = providerKey(id, epoch);
    if (d->providers.contains(key)) {
        KisPageStoreDetail::setError(error, QStringLiteral("replica provider identity is already registered"));
        return false;
    }
    // One active epoch per stable provider ID. Re-registration after restart
    // requires a new document/session ledger so stale handles fail closed.
    for (auto registered = d->providers.cbegin(); registered != d->providers.cend(); ++registered) {
        if (registered.key().first == id.value) {
            KisPageStoreDetail::setError(error, QStringLiteral("replica provider ID already has an active epoch"));
            return false;
        }
    }
    d->providers.insert(key, provider);
    KisPageStoreDetail::setError(error, {});
    return true;
}

QSharedPointer<KisPageReplicaProvider> KisPageOwnerLedger::provider(
    KisReplicaProviderId providerId,
    KisReplicaProviderEpoch epoch) const
{
    QMutexLocker locker(&d->mutex);
    return d->providers.value(providerKey(providerId, epoch));
}

QSharedPointer<KisPageReplicaProvider> KisPageOwnerLedger::providerFor(
    KisPageAccessRequirement access) const
{
    if (!access.isValid()) return {};
    QList<QSharedPointer<KisPageReplicaProvider>> providers;
    {
        QMutexLocker locker(&d->mutex);
        providers = d->providers.values();
    }
    QSharedPointer<KisPageReplicaProvider> selected;
    for (const QSharedPointer<KisPageReplicaProvider> &candidate : providers) {
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
    return bindProviderOperationImpl(operation, result, true, error);
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
    if (!d->completions || d->operations.contains(operation.value) ||
        !d->providers.contains(providerKey(result.replica.provider,
                                           result.replica.providerEpoch))) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation is duplicate or foreign"));
        return false;
    }
    d->operations.insert(operation.value, {result.completion, detachedRetirement});
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
        !(operationIt->completion == completion)) {
        KisPageStoreDetail::setError(error, QStringLiteral("provider operation changed before release"));
        return false;
    }
    d->operations.erase(operationIt);
    KisPageStoreDetail::setError(error, {});
    return true;
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
                         [](const ProviderOperationRecord &record) {
                             return !record.detachedRetirement;
                         });
}

qsizetype KisPageOwnerLedger::sealedProofCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->sealedProofs.size();
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
