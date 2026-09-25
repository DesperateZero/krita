/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageWriteCoordinator_p.h"

#include "KisPageMetadataCoordinator.h"
#include "KisPageOwnerLedger.h"
#include "KisPageReplicaProvider.h"
#include "KisPagePublicationCoordinator_p.h"

#include <QMutexLocker>

#include <algorithm>
#include <utility>

namespace
{

constexpr size_t budgetClassCount = static_cast<size_t>(KisBackingBudgetClass::Count);

template<typename DomainBytes>
auto components(const DomainBytes &bytes)
{
    return std::array{bytes.cpuRam, bytes.umaShared, bytes.discreteVram, bytes.ssd};
}

KisPageDomainBytes fromComponents(const std::array<quint64, 4> &values)
{
    return {values[0], values[1], values[2], values[3]};
}

quint64 saturatedAdd(quint64 left, quint64 right)
{
    const quint64 maximum = std::numeric_limits<quint64>::max();
    return right > maximum - left ? maximum : left + right;
}

bool hasPositiveBytes(const KisBackingBudgetDelta &delta)
{
    for (const auto &bucket : delta.buckets)
        for (qint64 value : components(bucket))
            if (value > 0) return true;
    return false;
}

quint64 domainLimit(const KisPageBackingLimits &limits, KisBackingBudgetClass budgetClass, size_t domain)
{
    switch (budgetClass) {
    case KisBackingBudgetClass::Current:
        return components(limits.residentCurrentBytes)[domain];
    case KisBackingBudgetClass::RetainedHistory:
        return components(limits.residentHistoryBytes)[domain];
    case KisBackingBudgetClass::ActivePending:
    case KisBackingBudgetClass::InFlight:
    case KisBackingBudgetClass::RetirementDebt:
    case KisBackingBudgetClass::OptionalCache:
    case KisBackingBudgetClass::MetadataArena:
        return std::numeric_limits<quint64>::max();
    case KisBackingBudgetClass::Count:
        break;
    }
    return 0;
}

quint64 aggregateLimit(const KisPageBackingLimits &limits, KisBackingBudgetClass budgetClass)
{
    switch (budgetClass) {
    case KisBackingBudgetClass::Current:
        return limits.logicalCurrentBytes;
    case KisBackingBudgetClass::RetainedHistory:
        return limits.retainedHistoryBytes;
    case KisBackingBudgetClass::ActivePending:
        return limits.activePendingBytes;
    case KisBackingBudgetClass::InFlight:
        return limits.inFlightReplicaBytes;
    case KisBackingBudgetClass::RetirementDebt:
        return limits.retirementDebtBytes;
    case KisBackingBudgetClass::OptionalCache:
        return limits.optionalCacheBytes;
    case KisBackingBudgetClass::MetadataArena:
        return limits.metadataArenaBytes;
    case KisBackingBudgetClass::Count:
        break;
    }
    return 0;
}

quint64 sumComponents(const std::array<quint64, 4> &values)
{
    quint64 result = 0;
    for (quint64 value : values)
        result = saturatedAdd(result, value);
    return result;
}

} // namespace

KisBackingBudgetReservation::KisBackingBudgetReservation(KisBackingBudgetController *ownerValue,
                                                         quint64 cookieValue)
    : owner(ownerValue)
    , cookie(cookieValue)
{
}

KisBackingBudgetReservation::~KisBackingBudgetReservation()
{
    release();
}

KisBackingBudgetReservation::KisBackingBudgetReservation(KisBackingBudgetReservation &&other) noexcept
{
    *this = std::move(other);
}

KisBackingBudgetReservation &KisBackingBudgetReservation::operator=(KisBackingBudgetReservation &&other) noexcept
{
    if (this != &other) {
        release();
        owner = std::exchange(other.owner, nullptr);
        cookie = std::exchange(other.cookie, 0);
    }
    return *this;
}

bool KisBackingBudgetReservation::isValid() const
{
    return owner != nullptr;
}

void KisBackingBudgetReservation::release() noexcept
{
    if (owner)
        owner->release(cookie);
    clear();
}

void KisBackingBudgetReservation::clear() noexcept
{
    owner = nullptr;
    cookie = 0;
}

void KisBackingBudgetReservation::commit(const KisBackingBudgetDelta &delta) noexcept
{
    if (owner)
        owner->commit(cookie, delta);
    clear();
}

void KisBackingBudgetReservation::commitRetaining(
    const KisBackingBudgetDelta &installed,
    const KisBackingBudgetDelta &retained) noexcept
{
    if (!owner || !owner->commitRetaining(cookie, installed, retained)) clear();
}

bool KisBackingBudgetReservation::retainOnly(
    const KisBackingBudgetDelta &retained) noexcept
{
    if (!owner || !owner->retainOnly(cookie, retained)) clear();
    return owner != nullptr;
}

KisBackingBudgetController::KisBackingBudgetController(const KisPageBackingLimits &limits)
    : m_limits(limits)
{
}

KisBackingBudgetReservation KisBackingBudgetController::reserve(
    const KisBackingBudgetDelta &delta, QString *error)
{
    std::array<quint64, budgetClassCount> aggregateBytes{};
    quint64 durableBytes = 0;
    for (size_t i = 0; i < delta.buckets.size(); ++i) {
        const auto &bucket = delta.buckets[i];
        for (qint64 value : components(bucket)) {
            if (value < 0) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("backing reservation contains a negative prepare delta"));
                return {};
            }
            aggregateBytes[i] = saturatedAdd(
                aggregateBytes[i], quint64(value));
        }
        if (bucket.ssd > 0)
            durableBytes = saturatedAdd(durableBytes, quint64(bucket.ssd));
    }
    return reserveImpl(delta, aggregateBytes, durableBytes, error);
}

KisBackingBudgetReservation KisBackingBudgetController::reserveChange(
    const KisBackingBudgetDelta &change, QString *error)
{
    KisBackingBudgetDelta prepared;
    std::array<quint64, budgetClassCount> aggregateBytes{};
    qint64 durableChange = 0;
    for (size_t i = 0; i < change.buckets.size(); ++i) {
        const auto values = components(change.buckets[i]);
        auto target = components(prepared.buckets[i]);
        qint64 aggregateChange = 0;
        for (size_t domain = 0; domain < values.size(); ++domain)
            target[domain] = std::max(qint64(0), values[domain]);
        for (qint64 value : values) {
            if (__builtin_add_overflow(aggregateChange, value,
                                       &aggregateChange)) {
                KisPageStoreDetail::setError(
                    error, QStringLiteral("backing transition aggregate delta overflows"));
                return {};
            }
        }
        prepared.buckets[i] = {
            target[0], target[1], target[2], target[3]};
        aggregateBytes[i] = aggregateChange > 0
            ? quint64(aggregateChange) : 0;
        if (__builtin_add_overflow(durableChange, change.buckets[i].ssd,
                                   &durableChange)) {
            KisPageStoreDetail::setError(
                error, QStringLiteral("backing transition SSD delta overflows"));
            return {};
        }
    }
    return reserveImpl(prepared, aggregateBytes,
                       durableChange > 0 ? quint64(durableChange) : 0,
                       error);
}

KisBackingBudgetReservation KisBackingBudgetController::reserveImpl(
    const KisBackingBudgetDelta &delta,
    const std::array<quint64, budgetClassCount> &aggregateBytes,
    quint64 durableBytes, QString *error)
{
    QMutexLocker lock(&m_mutex);
    const auto reject = [&](const QString &message) {
        ++m_usage.backpressureCount;
        KisPageStoreDetail::setError(error, message);
        return KisBackingBudgetReservation{};
    };

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        const auto budgetClass = static_cast<KisBackingBudgetClass>(bucketIndex);
        const auto live = components(m_usage.buckets[bucketIndex].live);
        const auto reserved = components(m_usage.buckets[bucketIndex].reserved);
        const auto requested = components(delta.buckets[bucketIndex]);
        for (size_t domain = 0; domain < requested.size(); ++domain) {
            if (requested[domain] < 0)
                return reject(QStringLiteral("backing reservation contains a negative prepare delta"));
            const quint64 addition = quint64(requested[domain]);
            const quint64 limit = domainLimit(m_limits, budgetClass, domain);
            if (addition > limit || live[domain] > limit - addition
                || reserved[domain] > limit - addition - live[domain])
                return reject(QStringLiteral("backing reservation exceeds its hard budget"));
        }
        const quint64 liveTotal = sumComponents(live);
        const quint64 reservedTotal = m_reservedAggregateBytes[bucketIndex];
        const quint64 requestedTotal = aggregateBytes[bucketIndex];
        const quint64 limit = aggregateLimit(m_limits, budgetClass);
        if (requestedTotal > limit || liveTotal > limit - requestedTotal
            || reservedTotal > limit - requestedTotal - liveTotal)
            return reject(QStringLiteral("backing reservation exceeds its aggregate hard budget"));
    }

    quint64 liveSsd = 0;
    const quint64 reservedSsd = m_reservedDurableBytes;
    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        liveSsd = saturatedAdd(liveSsd, m_usage.buckets[bucketIndex].live.ssd);
    }
    const quint64 durableLimit = m_limits.durableStoreCapacity;
    if (durableBytes > durableLimit || liveSsd > durableLimit - durableBytes
        || reservedSsd > durableLimit - durableBytes - liveSsd)
        return reject(QStringLiteral("backing reservation exceeds durable store capacity"));

    quint32 slotIndex = 0;
    if (m_freeSlots.empty()) {
        slotIndex = quint32(m_slots.size());
        // Commit/release are noexcept and only recycle an already prepared
        // slot. Reserve their free-list capacity during the fallible prepare.
        if (m_freeSlots.capacity() < m_slots.size() + 1)
            m_freeSlots.reserve(std::max(m_slots.size() + 1,
                                         m_freeSlots.capacity() ? m_freeSlots.capacity() * 2 : size_t(1)));
        m_slots.emplace_back();
    } else {
        slotIndex = m_freeSlots.back();
        m_freeSlots.pop_back();
    }
    ReservationSlot &slot = m_slots[slotIndex];
    ++slot.generation;
    if (!slot.generation)
        ++slot.generation;
    slot.delta = delta;
    slot.aggregateBytes = aggregateBytes;
    slot.durableBytes = durableBytes;
    slot.active = true;
    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        m_reservedAggregateBytes[bucketIndex] = saturatedAdd(
            m_reservedAggregateBytes[bucketIndex], aggregateBytes[bucketIndex]);
    }
    m_reservedDurableBytes = saturatedAdd(m_reservedDurableBytes, durableBytes);

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        auto reserved = components(m_usage.buckets[bucketIndex].reserved);
        auto peak = components(m_usage.buckets[bucketIndex].peak);
        const auto live = components(m_usage.buckets[bucketIndex].live);
        const auto requested = components(delta.buckets[bucketIndex]);
        for (size_t domain = 0; domain < requested.size(); ++domain) {
            reserved[domain] += quint64(requested[domain]);
            peak[domain] = std::max(peak[domain], saturatedAdd(live[domain], reserved[domain]));
        }
        m_usage.buckets[bucketIndex].reserved = fromComponents(reserved);
        m_usage.buckets[bucketIndex].peak = fromComponents(peak);
    }
    KisPageStoreDetail::setError(error, {});
    const quint64 cookie = (quint64(slot.generation) << 32) | slotIndex;
    return {this, cookie};
}

void KisBackingBudgetController::commitReservation(
    KisBackingBudgetReservation &&reservation,
    const KisBackingBudgetDelta &installed) noexcept
{
    if (reservation.owner == this)
        reservation.commit(installed);
}

KisPageBackingUsage KisBackingBudgetController::usage() const
{
    QMutexLocker lock(&m_mutex);
    return m_usage;
}

quint32 KisBackingBudgetController::maxTransientVersionsPerPage() const
{
    QMutexLocker lock(&m_mutex);
    return m_limits.maxTransientVersionsPerPage;
}

bool KisBackingBudgetController::reservationCovers(quint64 cookie,
                                                    KisBackingBudgetClass budgetClass,
                                                    KisPageAccessDomain domain,
                                                    quint64 bytes) const
{
    const size_t classIndex = static_cast<size_t>(budgetClass);
    QMutexLocker lock(&m_mutex);
    const ReservationSlot *slot = activeReservation(cookie);
    if (classIndex >= budgetClassCount || !slot)
        return false;
    const qint64 *reserved = kisPageDomainComponent(
        slot->delta.buckets[classIndex], domain);
    return reserved && *reserved >= qint64(bytes);
}

bool KisBackingBudgetController::reservationCovers(
    quint64 cookie, const KisBackingBudgetDelta &needed) const
{
    QMutexLocker lock(&m_mutex);
    const ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return false;
    for (size_t i = 0; i < budgetClassCount; ++i) {
        const auto reserved = components(slot->delta.buckets[i]);
        const auto requested = components(needed.buckets[i]);
        for (size_t domain = 0; domain < requested.size(); ++domain)
            if (requested[domain] < 0 || requested[domain] > reserved[domain]) return false;
    }
    return true;
}

bool KisBackingBudgetController::configureLimits(const KisPageBackingLimits &limits,
                                                  QString *error)
{
    QMutexLocker lock(&m_mutex);
    for (const auto &bucket : m_usage.buckets) {
        if (sumComponents(components(bucket.live))
            || sumComponents(components(bucket.reserved))) {
            KisPageStoreDetail::setError(error, QStringLiteral("backing limits are already in use"));
            return false;
        }
    }
    m_limits = limits;
    KisPageStoreDetail::setError(error, {});
    return true;
}

void KisBackingBudgetController::releaseLive(KisBackingBudgetClass budgetClass,
                                             KisPageAccessDomain domain,
                                             quint64 bytes) noexcept
{
    const size_t bucketIndex = static_cast<size_t>(budgetClass);
    if (bucketIndex >= budgetClassCount) return;
    QMutexLocker lock(&m_mutex);
    quint64 *live = kisPageDomainComponent(m_usage.buckets[bucketIndex].live, domain);
    if (!live) return;
    Q_ASSERT(*live >= bytes);
    *live = bytes > *live ? 0 : *live - bytes;
}

void KisBackingBudgetController::commit(quint64 cookie, const KisBackingBudgetDelta &delta) noexcept
{
    commitRetaining(cookie, delta, {});
}

bool KisBackingBudgetController::commitRetaining(
    quint64 cookie,
    const KisBackingBudgetDelta &installed,
    const KisBackingBudgetDelta &retained) noexcept
{
    QMutexLocker lock(&m_mutex);
    const quint32 slotIndex = quint32(cookie);
    ReservationSlot *slot = activeReservation(cookie);
    if (!slot) return 0;

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        Q_ASSERT(m_reservedAggregateBytes[bucketIndex]
                 >= slot->aggregateBytes[bucketIndex]);
        m_reservedAggregateBytes[bucketIndex] -= slot->aggregateBytes[bucketIndex];
    }
    Q_ASSERT(m_reservedDurableBytes >= slot->durableBytes);
    m_reservedDurableBytes -= slot->durableBytes;

    for (size_t bucketIndex = 0; bucketIndex < budgetClassCount; ++bucketIndex) {
        auto live = components(m_usage.buckets[bucketIndex].live);
        auto peak = components(m_usage.buckets[bucketIndex].peak);
        auto reserved = components(m_usage.buckets[bucketIndex].reserved);
        const auto prepared = components(slot->delta.buckets[bucketIndex]);
        const auto keep = components(retained.buckets[bucketIndex]);
        const auto commit = components(installed.buckets[bucketIndex]);
        for (size_t domain = 0; domain < prepared.size(); ++domain) {
            Q_ASSERT(prepared[domain] >= 0 && keep[domain] >= 0
                     && keep[domain] <= prepared[domain]);
            reserved[domain] -= quint64(prepared[domain] - keep[domain]);
            if (commit[domain] < 0) {
                const quint64 removal = quint64(-commit[domain]);
                Q_ASSERT(live[domain] >= removal);
                live[domain] = removal > live[domain] ? 0 : live[domain] - removal;
            } else {
                live[domain] = saturatedAdd(live[domain], quint64(commit[domain]));
                peak[domain] = std::max(peak[domain], live[domain]);
            }
        }
        m_usage.buckets[bucketIndex].live = fromComponents(live);
        m_usage.buckets[bucketIndex].peak = fromComponents(peak);
        m_usage.buckets[bucketIndex].reserved = fromComponents(reserved);
    }
    slot->delta = retained;
    slot->aggregateBytes = {};
    slot->durableBytes = 0;
    for (size_t i = 0; i < retained.buckets.size(); ++i) {
        const auto &bucket = retained.buckets[i];
        for (qint64 value : components(bucket)) {
            if (value > 0) {
                slot->aggregateBytes[i] = saturatedAdd(
                    slot->aggregateBytes[i], quint64(value));
            }
        }
        if (bucket.ssd > 0)
            slot->durableBytes = saturatedAdd(
                slot->durableBytes, quint64(bucket.ssd));
        m_reservedAggregateBytes[i] = saturatedAdd(
            m_reservedAggregateBytes[i], slot->aggregateBytes[i]);
    }
    m_reservedDurableBytes = saturatedAdd(
        m_reservedDurableBytes, slot->durableBytes);
    const bool retainedReservation = hasPositiveBytes(retained);
    if (!retainedReservation) {
        slot->active = false;
        slot->aggregateBytes = {};
        slot->durableBytes = 0;
        m_freeSlots.push_back(slotIndex);
    }
    return retainedReservation;
}

bool KisBackingBudgetController::retainOnly(
    quint64 cookie,
    const KisBackingBudgetDelta &retained) noexcept
{
    return commitRetaining(cookie, {}, retained);
}

void KisBackingBudgetController::release(quint64 cookie) noexcept
{
    commitRetaining(cookie, {}, {});
}

KisMutationPageEntry *KisMutationWriteSet::find(const KisPageKey &key)
{
    return const_cast<KisMutationPageEntry *>(std::as_const(*this).find(key));
}

const KisMutationPageEntry *KisMutationWriteSet::find(const KisPageKey &key) const
{
    const EntryIndex found = findIndex(key);
    return found == InvalidEntry ? nullptr : at(found);
}

KisMutationWriteSet::EntryIndex KisMutationWriteSet::findIndex(const KisPageKey &key) const
{
    if (!inlineEntry)
        return InvalidEntry;
    if (index) {
        const auto found = index->constFind(key);
        return found == index->constEnd() ? InvalidEntry : found.value();
    }
    if (inlineEntry->key() == key)
        return 0;
    for (size_t i = 0; i < overflow.size(); ++i) {
        if (overflow[i].key() == key)
            return EntryIndex(i + 1);
    }
    return InvalidEntry;
}

KisMutationPageEntry &KisMutationWriteSet::getOrCreate(const KisPageWriteIntent &intent)
{
    if (KisMutationPageEntry *found = find(intent.key))
        return *found;
    if (!inlineEntry) {
        inlineEntry.emplace(intent);
        if (index)
            index->insert(intent.key, 0);
        return *inlineEntry;
    }
    const EntryIndex newIndex = EntryIndex(overflow.size() + 1);
    overflow.emplace_back(intent);
    if (size() == 9 && !index)
        ensureIndex();
    else if (index)
        index->insert(intent.key, newIndex);
    return overflow.back();
}

KisMutationPageEntry *KisMutationWriteSet::at(EntryIndex entry)
{
    return const_cast<KisMutationPageEntry *>(std::as_const(*this).at(entry));
}

const KisMutationPageEntry *KisMutationWriteSet::at(EntryIndex entry) const
{
    if (entry == 0)
        return inlineEntry ? &*inlineEntry : nullptr;
    const size_t overflowIndex = size_t(entry - 1);
    return overflowIndex < overflow.size() ? &overflow[overflowIndex] : nullptr;
}

void KisMutationWriteSet::reserveKnownTargetCount(qsizetype count)
{
    if (count > 1)
        overflow.reserve(size_t(count - 1));
    if (count > 8) {
        if (!index)
            index.emplace();
        index->reserve(count);
        ensureIndex();
    }
}

qsizetype KisMutationWriteSet::size() const
{
    return inlineEntry ? qsizetype(overflow.size() + 1) : 0;
}

void KisMutationWriteSet::ensureIndex()
{
    if (!index)
        index.emplace();
    index->reserve(size());
    if (inlineEntry)
        index->insert(inlineEntry->key(), 0);
    for (size_t i = 0; i < overflow.size(); ++i) {
        index->insert(overflow[i].key(), EntryIndex(i + 1));
    }
}

KisPageWriteAdmission::ClaimSet::~ClaimSet()
{
    release();
}

KisPageWriteAdmission::ClaimSet::ClaimSet(ClaimSet &&other) noexcept
{
    *this = std::move(other);
}

KisPageWriteAdmission::ClaimSet &KisPageWriteAdmission::ClaimSet::operator=(ClaimSet &&other) noexcept
{
    if (this != &other) {
        release();
        owner = std::exchange(other.owner, nullptr);
        writeSet = std::exchange(other.writeSet, nullptr);
        token = std::exchange(other.token, 0);
    }
    return *this;
}

bool KisPageWriteAdmission::ClaimSet::isValid() const
{
    return owner && writeSet && token;
}

void KisPageWriteAdmission::ClaimSet::release() noexcept
{
    releaseImpl(true);
}

void KisPageWriteAdmission::ClaimSet::releaseLocked() noexcept
{
    releaseImpl(false);
}

void KisPageWriteAdmission::ClaimSet::releaseImpl(bool lockOwner) noexcept
{
    if (owner)
        owner->release(*this, lockOwner);
    owner = nullptr;
    writeSet = nullptr;
    token = 0;
}

KisPageWriteAdmission::KisPageWriteAdmission(QMutex &ownerMutex, QWaitCondition &ownerCondition)
    : m_ownerMutex(&ownerMutex)
    , m_ownerCondition(&ownerCondition)
{
}

qsizetype KisPageWriteAdmission::activeNativeClaimCountLocked() const
{
    return m_claims.size() - activeGenericClaimCountLocked();
}

qsizetype KisPageWriteAdmission::activeGenericClaimCountLocked() const
{
    return std::count_if(m_claims.cbegin(), m_claims.cend(),
                         [](const ActiveClaim &claim) {
                             return claim.origin == ClaimOrigin::GenericWrite;
                         });
}

KisPageWriteAdmission::Conflict KisPageWriteAdmission::conflictLocked(
    const KisPageKey &key, Qt::HANDLE requester) const
{
    const auto found = m_claims.constFind(key);
    if (found == m_claims.constEnd())
        return Conflict::None;
    if (found->origin == ClaimOrigin::LegacyAdapter)
        return found->thread == requester ? Conflict::LegacySameThread
                                           : Conflict::LegacyBorrower;
    if (found->origin == ClaimOrigin::ManagedRange)
        return found->thread == requester ? Conflict::ManagedSameThread
                                           : Conflict::ManagedOtherThread;
    return found->thread == requester ? Conflict::WriterSameThread
                                       : Conflict::WriterOtherThread;
}

bool KisPageWriteAdmission::claimDirectLocked(const KisPageKey &key, quint64 ownerToken)
{
    Q_ASSERT(ownerToken);
    if (!ownerToken)
        return false;
    const auto found = m_claims.constFind(key);
    if (found != m_claims.constEnd()) {
        if (found->origin == ClaimOrigin::GenericWrite && found->token == ownerToken)
            return true;
        return false;
    }
    m_claims.insert(key, {ownerToken, ClaimOrigin::GenericWrite,
                          QThread::currentThreadId()});
    return true;
}

void KisPageWriteAdmission::releaseDirectLocked(const KisPageKey &key,
                                                 quint64 ownerToken) noexcept
{
    auto found = m_claims.find(key);
    Q_ASSERT(found != m_claims.end() && found->origin == ClaimOrigin::GenericWrite
             && found->token == ownerToken);
    if (found == m_claims.end() || found->origin != ClaimOrigin::GenericWrite
        || found->token != ownerToken)
        return;
    m_claims.erase(found);
    m_ownerCondition->wakeAll();
}

KisPageWriteAdmission::ClaimSet KisPageWriteAdmission::beginClaimSet(
    const KisMutationWriteSet &writeSet)
{
    ClaimSet result;
    result.owner = this;
    result.writeSet = &writeSet;
    result.token = m_nextClaimToken++;
    if (!result.token)
        result.token = m_nextClaimToken++;
    return result;
}

bool KisPageWriteAdmission::claimOne(ClaimSet &claims,
                                     KisMutationWriteSet::EntryIndex entry,
                                     QString *error,
                                     ClaimOrigin origin)
{
    if (!claims.isValid() || claims.owner != this) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission claim set is invalid"));
        return false;
    }
    const KisMutationPageEntry *page = claims.writeSet->at(entry);
    if (!page) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission entry is invalid"));
        return false;
    }
    const auto found = m_claims.constFind(page->key());
    if (found == m_claims.constEnd()) {
        m_claims.insert(page->key(), {claims.token, origin,
                                     QThread::currentThreadId()});
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    if (found->origin != ClaimOrigin::GenericWrite && found->token == claims.token) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    KisPageStoreDetail::setError(error, QStringLiteral("page is claimed by another writer"));
    return false;
}

bool KisPageWriteAdmission::claimAll(ClaimSet &claims, QString *error,
                                     ClaimOrigin origin)
{
    if (!claims.isValid() || claims.owner != this) {
        KisPageStoreDetail::setError(error, QStringLiteral("write admission claim set is invalid"));
        return false;
    }
    for (qsizetype i = 0; i < claims.writeSet->size(); ++i) {
        const auto *entry = claims.writeSet->at(KisMutationWriteSet::EntryIndex(i));
        const auto found = m_claims.constFind(entry->key());
        if (found != m_claims.constEnd()
            && (found->origin == ClaimOrigin::GenericWrite
                || found->token != claims.token)) {
            KisPageStoreDetail::setError(error, QStringLiteral("write set intersects another writer"));
            return false;
        }
    }
    for (qsizetype i = 0; i < claims.writeSet->size(); ++i) {
        const KisPageKey &key = claims.writeSet->at(KisMutationWriteSet::EntryIndex(i))->key();
        if (!m_claims.contains(key)) {
            m_claims.insert(key, {claims.token, origin,
                                  QThread::currentThreadId()});
        }
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageWriteAdmission::ownsClaimSetLocked(const ClaimSet &claims) const
{
    if (!claims.isValid() || claims.owner != this)
        return false;
    for (qsizetype i = 0; i < claims.writeSet->size(); ++i) {
        const auto *page = claims.writeSet->at(KisMutationWriteSet::EntryIndex(i));
        const auto found = m_claims.constFind(page->key());
        if (found == m_claims.constEnd()
            || found->origin == ClaimOrigin::GenericWrite
            || found->token != claims.token)
            return false;
    }
    return true;
}

void KisPageWriteAdmission::rebindClaimSetLocked(ClaimSet &claims,
                                                 const KisMutationWriteSet &writeSet) noexcept
{
    Q_ASSERT(claims.isValid() && claims.owner == this);
    Q_ASSERT(writeSet.size() > 0);
    for (qsizetype i = 0; i < writeSet.size(); ++i) {
        const auto *entry = writeSet.at(KisMutationWriteSet::EntryIndex(i));
        const auto found = m_claims.constFind(entry->key());
        Q_ASSERT(found != m_claims.constEnd()
                 && found->origin != ClaimOrigin::GenericWrite
                 && found->token == claims.token);
    }
    claims.writeSet = &writeSet;
}

void KisPageWriteAdmission::release(ClaimSet &claims, bool lockOwner) noexcept
{
    std::optional<QMutexLocker<QMutex>> lock;
    if (lockOwner)
        lock.emplace(m_ownerMutex);
    if (claims.owner != this || !claims.writeSet)
        return;
    bool changed = false;
    for (qsizetype i = 0; i < claims.writeSet->size(); ++i) {
        const auto *page = claims.writeSet->at(KisMutationWriteSet::EntryIndex(i));
        auto found = m_claims.find(page->key());
        if (found != m_claims.end()
            && found->origin != ClaimOrigin::GenericWrite
            && found->token == claims.token) {
            m_claims.erase(found);
            changed = true;
        }
    }
    if (changed)
        m_ownerCondition->wakeAll();
}

KisPageWriteCoordinator::KisPageWriteCoordinator(KisPageMetadataCoordinator &metadataValue,
                                                 KisImageEpochReferenceModel &epochValue,
                                                 KisBackingBudgetController &budgetValue,
                                                 KisPageOwnerLedger &ownerLedgerValue)
    : metadata(&metadataValue)
    , epoch(&epochValue)
    , budget(&budgetValue)
    , ownerLedger(&ownerLedgerValue)
{
}

bool KisPageWriteCoordinator::prepareWriteBaseLocked(
    const KisPageTransaction &transaction, const KisPageWriteIntent &intent,
    KisPagePublicationCoordinator &publication, KisPageTransition *write,
    KisPageAllocationDescriptor *descriptor, QString *error, const KisPageTransition *pending)
{
    const auto fail = [&](const QString &reason) { KisPageStoreDetail::setError(error, reason); return false; };
    const auto overlay = KisPageReadView::transactionOverlay(transaction.id);
    KisPageVersion baseVersion;
    if (!epoch->resolve(intent.key, overlay, &baseVersion))
        return fail(QStringLiteral("write base does not resolve"));
    const auto *proofs = publication.findProofsLocked(transaction.id);
    const auto sealed = pending ? pending->baseVersion
                                : proofs ? proofs->value(intent.key).authority.version
                                         : KisPageVersion{};
    KisPageStateSnapshot snapshot;
    if (!metadata->mutationBaseSnapshot(baseVersion, sealed, &snapshot)
        || !snapshot.findVersion(baseVersion)) {
        KisSurfaceEpochState before;
        if (!epoch->surfaceState(intent.key.surface, overlay, &before)
            || !publication.ensureVirtualDefaultLocked(baseVersion, before, error)
            || !metadata->mutationBaseSnapshot(baseVersion, sealed, &snapshot))
            return false;
    }
    if (snapshot.writer.isValid())
        return fail(QStringLiteral("write page already has a writer"));
    // Only a previously sealed overlay (or the exact base of a private target
    // being replaced) supersedes the transaction root. Never scan all history
    // for another Prepared version and accidentally expose unsealed work.
    if (sealed.isValid()) baseVersion = sealed;
    const auto *base = snapshot.findVersion(baseVersion);
    if (!base || (!base->isVirtualDefault() && !base->authority.isValid()))
        return fail(QStringLiteral("write base has no exact authority"));
    const bool removed = (intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval))
        || publication.stagesRemovalLocked(transaction.id, intent.key);
    if (removed || baseVersion.isDefaultPixel() || intent.inputKind == KisPageWriteInputKind::Semantic) {
        KisSurfaceEpochState surface;
        if (!publication.resolveSurfaceLocked(intent.key.surface, overlay, &surface))
            return fail(QStringLiteral("write surface does not resolve"));
        *descriptor = surface.allocationDescriptor();
    } else if (!publication.descriptorLocked(baseVersion, descriptor))
        return fail(QStringLiteral("write base descriptor is unavailable"));
    *write = {};
    write->kind = KisPageTransitionKind::AcquireWrite;
    write->baseVersion = baseVersion;
    write->version = pending ? pending->version : KisPageVersion{intent.key, snapshot.nextGeneration};
    write->source = pending ? pending->target : base->authority;
    write->transaction = transaction.id;
    write->operation = ownerLedger->nextOperationId();
    write->writer = ownerLedger->nextWriterToken();
    write->writeMode = removed || baseVersion.isDefaultPixel() ? KisPageWriteMode::DiscardContents : intent.mode;
    if (!descriptor->isValid() || !write->version.isValid() || !write->operation.isValid() || !write->writer.isValid())
        return fail(QStringLiteral("write identity or layout is unavailable"));
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageTransitionResult KisPageWriteCoordinator::preparePrivateWrite(KisPageTransition &write, bool initialized)
{
    // A reader can materialize a virtual before-image during the unlocked
    // provider allocation. Acquire its actual authority pin, not a stale null
    // source; the target still initializes directly from the exact default.
    if (write.baseVersion.isDefaultPixel() && !write.source.isValid()) {
        KisPageStateSnapshot current;
        if (!metadata->versionSnapshot(write.baseVersion, &current) || current.versions.isEmpty()) {
            KisPageTransitionResult rejected;
            rejected.rejectionReason = QStringLiteral("write default base disappeared");
            return rejected;
        }
        write.source = current.versions.first().authority;
    }
    if (!initialized)
        return metadata->applyOwner(write.version.key, write);
    auto prepare = write;
    prepare.kind = KisPageTransitionKind::PrepareWrite;
    return metadata->applyOwnerSequence(write.version.key, {write, prepare});
}

KisPageTransitionResult KisPageWriteCoordinator::publishPrivateWrite(KisPageTransition write)
{
    write.kind = KisPageTransitionKind::BeginPublish;
    auto publish = write;
    publish.kind = KisPageTransitionKind::PublishWrite;
    return metadata->applyOwnerSequence(write.version.key, {write, publish});
}

KisBackingBudgetReservation KisPageWriteCoordinator::reserveBacking(
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisBackingBudgetClass budgetClass,
    QString *error,
    const KisPageVersion &target)
{
    if (!descriptor.isValid() || domain == KisPageAccessDomain::Unknown
        || static_cast<size_t>(budgetClass) >= budgetClassCount
        || (budgetClass == KisBackingBudgetClass::ActivePending && !target.isValid())) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation layout is invalid"));
        return {};
    }
    if (!ownerLedger->synchronizeBackingDomains(error))
        return {};
    const quint64 bytes = descriptor.minimumByteSize();
    if (bytes > quint64(std::numeric_limits<qint64>::max())) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation exceeds signed delta range"));
        return {};
    }
    const quint32 transientLimit = budget->maxTransientVersionsPerPage();
    if (budgetClass == KisBackingBudgetClass::ActivePending
        && transientLimit != std::numeric_limits<quint32>::max()
        && !metadata->canAddTransientVersion(target, transientLimit)) {
        KisPageStoreDetail::setError(error, QStringLiteral("page transient-version budget is exhausted"));
        return {};
    }
    KisBackingBudgetDelta delta;
    auto &bucket = delta.buckets[static_cast<size_t>(budgetClass)];
    qint64 *reserved = kisPageDomainComponent(bucket, domain);
    if (!reserved) {
        KisPageStoreDetail::setError(error, QStringLiteral("backing reservation domain is invalid"));
        return {};
    }
    *reserved = qint64(bytes);
    // A fresh physical allocation is not allowed to become unaccounted debt
    // if provider acceptance or metadata adoption fails. Keep one composite
    // reservation through that interval; registerBacking() commits the target
    // class while retaining only this Debt fallback.
    if (budgetClass != KisBackingBudgetClass::RetirementDebt
        && budgetClass != KisBackingBudgetClass::OptionalCache
        && budgetClass != KisBackingBudgetClass::MetadataArena) {
        auto &debt = delta.buckets[static_cast<size_t>(KisBackingBudgetClass::RetirementDebt)];
        *kisPageDomainComponent(debt, domain) = qint64(bytes);
    }
    return budget->reserve(delta, error);
}

KisPageTransitionResult KisPageWriteCoordinator::cancelPrivateWrite(KisPageTransition write)
{
    KisPageStateSnapshot snapshot;
    if (metadata->versionSnapshot(write.version, &snapshot)) {
        if (const auto *version = snapshot.findVersion(write.version)) {
            write.kind = version->publication == KisPagePublicationState::Prepared
                ? KisPageTransitionKind::AbortPreparedVersion : KisPageTransitionKind::CancelWrite;
            return metadata->applyOwner(write.version.key, write);
        }
    }
    KisPageTransitionResult rejected;
    rejected.rejectionReason = QStringLiteral("private write version is absent");
    return rejected;
}

KisPageWritePlanKind KisPageWriteCoordinator::select(
    const KisPageWriteIntent &intent,
    const KisMutationPageEntry *pending) const
{
    if (intent.inputKind == KisPageWriteInputKind::MutableGuard
        && !(intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))
        && intent.mode == KisPageWriteMode::PreserveContents) {
        return KisPageWritePlanKind::FreshCow;
    }
    if (intent.inputKind == KisPageWriteInputKind::MutableGuard) {
        if (intent.flags & quint8(KisPageWriteIntentFlag::InputBytesReady))
            return KisPageWritePlanKind::FreshPayload;
        return intent.mode == KisPageWriteMode::DiscardContents
            ? KisPageWritePlanKind::FreshDiscard
            : KisPageWritePlanKind::FreshCow;
    }
    return pending && pending->preparedTargetVersion().isValid()
        ? KisPageWritePlanKind::ReusePending
        : KisPageWritePlanKind::SemanticOnly;
}

KisReplicaOperation KisPageWriteCoordinator::prepareFreshReplica(
    const KisPageWriteIntent &intent, KisPageReplicaProvider &provider,
    const KisPageTransition &write, const KisPageAllocationDescriptor &descriptor,
    KisPageAccessRequirement access, KisPagePriority priority,
    const KisCpuPagePayload *payload,
    const QSharedPointer<const KisPageReplicaSource> &initialization,
    bool *synchronousCopy) const
{
    if (synchronousCopy) *synchronousCopy = false;
    if (intent.inputKind != KisPageWriteInputKind::MutableGuard)
        return {};
    const auto plan = select(intent);
    if (plan == KisPageWritePlanKind::FreshPayload)
        return payload ? provider.prepareSynchronousCpuPayload(
            write.operation, write.version, descriptor, *payload, priority) : KisReplicaOperation{};
    if (initialization)
        return provider.prepareSynchronousSource(write.operation, initialization,
            write.version, descriptor, KisReplicaSourceUse::WritableCopy, priority);
    if (plan == KisPageWritePlanKind::FreshCow) {
        const bool copy = !(intent.flags & quint8(KisPageWriteIntentFlag::AsyncLease))
            || (access.kind == KisPageAccessKind::CpuPointer
                && write.source.domain == access.domain
                && write.source.provider == provider.providerId()
                && write.source.providerEpoch == provider.providerEpoch()
                && provider.capabilities().synchronousWriteCopy);
        if (copy) {
            if (synchronousCopy) *synchronousCopy = true;
            return provider.prepareSynchronousWriteCopy(write.operation, write.source,
                write.version, descriptor, priority);
        }
    }
    return provider.prepareWrite(write.operation, write.version, descriptor,
        access.domain, write.writeMode, priority);
}

bool KisPageWriteCoordinator::registerTransferBridge(
    const QSharedPointer<KisPageReplicaTransferBridge> &bridge)
{
    if (!bridge || !bridge->synchronousOperations() || transferBridges.contains(bridge))
        return false;
    transferBridges.append(bridge);
    return true;
}

KisCompletionTicket KisPageWriteCoordinator::initializeFreshReplica(
    const KisPageWriteIntent &intent, bool initializedDuringAllocation,
    const KisReplicaHandle &source, const KisReplicaHandle &target,
    const KisPageAllocationDescriptor &descriptor,
    const QSharedPointer<KisPageReplicaProvider> &targetProvider,
    KisPagePriority priority, const KisCompletionTicket &allocationReadiness,
    QString *error) const
{
    if (select(intent) != KisPageWritePlanKind::FreshCow || initializedDuringAllocation) {
        KisPageStoreDetail::setError(error, {});
        return allocationReadiness;
    }
    const auto sourceProvider = ownerLedger->provider(source.provider, source.providerEpoch);
    if (!sourceProvider || !targetProvider) {
        KisPageStoreDetail::setError(error, QStringLiteral("write transfer provider is unavailable"));
        return {};
    }
    KisReplicaTransferRequest request;
    request.operation = ownerLedger->nextOperationId();
    request.source = source;
    request.target = target;
    request.descriptor = descriptor;
    request.kind = KisReplicaTransferKind::WriteGenerationInitialization;
    if (!request.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("write transfer request is invalid"));
        return {};
    }

    KisReplicaOperation transfer;
    if (request.isSameProviderTransfer()) {
        transfer = targetProvider->transfer(request, priority);
    } else {
        QSharedPointer<KisPageReplicaTransferBridge> selected;
        for (const auto &bridge : std::as_const(transferBridges)) {
            if (!bridge->supports(request))
                continue;
            if (selected) {
                KisPageStoreDetail::setError(error, QStringLiteral("write transfer bridge is ambiguous"));
                return {};
            }
            selected = bridge;
        }
        if (!selected) {
            KisPageStoreDetail::setError(error, QStringLiteral("write transfer bridge is unavailable"));
            return {};
        }
        transfer = selected->transfer(request, sourceProvider, targetProvider, priority);
    }

    if (!transfer.isValid() || !(transfer.replica == target)) {
        KisPageStoreDetail::setError(error, transfer.error.isEmpty()
            ? QStringLiteral("write generation initialization failed") : transfer.error);
        return {};
    }
    const auto completion = ownerLedger->consumeTerminalProviderOperation(
        transfer.operation, transfer, error);
    return completion.succeeded() ? transfer.completion : KisCompletionTicket{};
}

void KisPageWriteCoordinator::recordPrepared(KisMutationPageEntry &entry,
                                             const KisPageWriteIntent &intent,
                                             const KisPageTransition &write)
{
    Q_ASSERT(entry.key() == intent.key && entry.state == KisMutationPageEntryState::IntentOnly);
    Q_ASSERT(write.version.key == entry.key() && !write.version.isDefaultPixel());
    const quint8 semanticRemoval = entry.intent.flags & quint8(KisPageWriteIntentFlag::SemanticRemoval);
    entry.intent = intent;
    entry.intent.flags |= semanticRemoval;
    entry.baseVersion = write.baseVersion;
    entry.targetGeneration = write.version.generation;
    entry.operation = write.operation;
    entry.writer = write.writer;
    entry.state = KisMutationPageEntryState::Prepared;
}

void KisPageWriteCoordinator::recordExposure(KisMutationPageEntry &entry, bool exposed)
{
    Q_ASSERT(entry.state == (exposed ? KisMutationPageEntryState::Prepared : KisMutationPageEntryState::Exposed));
    entry.state = exposed ? KisMutationPageEntryState::Exposed : KisMutationPageEntryState::Prepared;
}

KisPageTransition KisPageWriteCoordinator::writeTransition(const KisMutationPageEntry &entry,
                                                           KisPageTransactionId transaction,
                                                           const KisReplicaHandle &source,
                                                           const KisReplicaHandle &target) const
{
    KisPageTransition write;
    write.baseVersion = entry.baseVersion;
    write.version = entry.preparedTargetVersion();
    write.operation = entry.operation;
    write.writer = entry.writer;
    write.writeMode = entry.intent.mode;
    write.transaction = transaction;
    write.source = source;
    write.target = target;
    return write;
}

void KisPageWriteCoordinator::beginSessionActivity(KisPageTransactionId transaction)
{
    Q_ASSERT(transaction.isValid());
    ++transactionActivities[transaction.value].sessions;
}

void KisPageWriteCoordinator::endSessionActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::sessions);
}

void KisPageWriteCoordinator::beginPreparationActivity(KisPageTransactionId transaction)
{
    Q_ASSERT(transaction.isValid());
    ++transactionActivities[transaction.value].preparations;
}

void KisPageWriteCoordinator::endPreparationActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::preparations);
}

void KisPageWriteCoordinator::beginGenericActivity(KisPageTransactionId transaction)
{
    Q_ASSERT(transaction.isValid());
    ++transactionActivities[transaction.value].genericWrites;
}

void KisPageWriteCoordinator::endGenericActivity(KisPageTransactionId transaction)
{
    endActivity(transaction, &TransactionActivity::genericWrites);
}

bool KisPageWriteCoordinator::transactionHasMutationActivity(KisPageTransactionId transaction) const
{
    return transactionActivities.contains(transaction.value);
}

bool KisPageWriteCoordinator::transactionHasSessionOrPreparation(KisPageTransactionId transaction) const
{
    const auto found = transactionActivities.constFind(transaction.value);
    return found != transactionActivities.constEnd() && (found->sessions > 0 || found->preparations > 0);
}

bool KisPageWriteCoordinator::transactionHasSession(KisPageTransactionId transaction) const
{
    const auto found = transactionActivities.constFind(transaction.value);
    return found != transactionActivities.constEnd() && found->sessions > 0;
}

void KisPageWriteCoordinator::endActivity(KisPageTransactionId transaction, qsizetype TransactionActivity::*member)
{
    auto found = transactionActivities.find(transaction.value);
    Q_ASSERT(found != transactionActivities.end() && found.value().*member > 0);
    if (found == transactionActivities.end() || found.value().*member <= 0)
        return;
    --(found.value().*member);
    if (found->isEmpty())
        transactionActivities.erase(found);
}
