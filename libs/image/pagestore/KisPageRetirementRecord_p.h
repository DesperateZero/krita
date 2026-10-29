/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_RETIREMENT_RECORD_P_H
#define KIS_PAGE_RETIREMENT_RECORD_P_H
#include "KisPageRetirementStorage_p.h"
#include "KisPageBackingBudget_p.h"
#include "KisPageReplicaProvider.h"
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <tuple>
#include <atomic>
#include <limits>
#include <QtCore/qalgorithms.h>
struct KisPageRetirementWait;
struct KisPagePreparedBackingChange;
struct KisPagePhysicalBackingRecord;
class KisCpuResidentBinding;

// Original idle ownership plus its stable physical binding. The binding still
// checks the exact generation and live reader/writer state before handoff.
struct KisPageIdleCapacity {
    KisReplicaHandle replica;
    std::shared_ptr<KisCpuResidentBinding> binding;
    // Publication transfers its original detached record to the current
    // operation. Its page/session claim excludes another checkout and close;
    // removing it from the idle list excludes expiry. The caller must consume
    // it by handoff or its existing retirement guard before ending that claim.
    // This location alone grants no ledger/physical write permission.
    KisPageRetirementRecord *original = nullptr;
};

struct KisPageBackingRecord
{
    KisReplicaHandle replica;
    KisBackingBudgetClass budgetClass = KisBackingBudgetClass::ActivePending;
    KisBackingBudgetReservation retirementHeadroom;
    // Non-null only while the ledger owns the enclosing retirement record.
    KisPageRetirementRecord *retirement = nullptr;
    KisPagePhysicalBackingRecord *physicalRecord = nullptr;
    KisPageBackingRecord *previous = nullptr;
    KisPageBackingRecord *next = nullptr;
    // The provider's stable allocation record follows this original backing,
    // including idle/handoff and rejected retirement. No D workspace survives.
    std::shared_ptr<KisCpuResidentBinding> nativeBinding;
};

// Counts are ordered by the actual fee priority, not by the public enum.
// Up to 255 total members fit in eight bytes, so every possible reclassification
// remains allocation-free. Larger groups retain a paid wide extension once.
class KisPageClassCounts
{
public:
    using Class = KisBackingBudgetClass;
    static_assert(size_t(Class::Count) == 8);
    static constexpr quint64 Wide = std::numeric_limits<quint64>::max();
    explicit KisPageClassCounts(quint64 packed = 0) noexcept : m_packed(packed) {}
    KisPageClassCounts(const KisPageClassCounts &other) noexcept : m_packed(other.m_packed)
    { if (isWide()) copyWide(other); }
    KisPageClassCounts &operator=(const KisPageClassCounts &other) noexcept
    { m_packed = other.m_packed; if (isWide()) copyWide(other); return *this; }
    static KisPageClassCounts single(Class cls) noexcept { return KisPageClassCounts(unit(rank(cls))); }
    static bool preservesCharge(Class member, Class charge) noexcept
    {
        Q_ASSERT(member != Class::Count && charge != Class::Count);
        return rank(member) >= rank(charge);
    }
    quint64 packed() const noexcept { return m_packed; }
    bool isWide() const noexcept { return m_packed == Wide; }
    quint32 count(Class cls) const noexcept
    { const auto i = rank(cls); return isWide() ? m_counts[i] : quint32((m_packed >> (8 * i)) & 255); }
    Class chargedClass() const noexcept
    {
        if (isWide()) return m_active ? priority[qCountTrailingZeroBits(quint32(m_active))] : Class::Count;
        return m_packed ? priority[qCountTrailingZeroBits(m_packed) / 8] : Class::Count;
    }
    bool add(Class cls) noexcept
    {
        const auto i = rank(cls);
        if (!isWide()) {
            if (compactTotal() < 255) { m_packed += unit(i); return true; }
            m_active = 0;
            for (size_t j = 0; j < priority.size(); ++j) {
                m_counts[j] = quint32((m_packed >> (8 * j)) & 255);
                if (m_counts[j]) m_active |= quint8(1u << j);
            }
            m_total = 255;
            m_packed = Wide;
        }
        if (m_counts[i] == std::numeric_limits<quint32>::max()) return false;
        ++m_counts[i]; ++m_total; m_active |= quint8(1u << i);
        return true;
    }
    void remove(Class cls) noexcept
    {
        const auto i = rank(cls);
        Q_ASSERT(count(cls));
        if (!isWide()) { m_packed -= unit(i); return; }
        if (!--m_counts[i]) m_active &= quint8(~(1u << i));
        if (--m_total <= 255) {
            m_packed = 0;
            for (size_t j = 0; j < priority.size(); ++j) m_packed |= quint64(m_counts[j]) << (8 * j);
        }
    }
    bool change(Class before, Class after) noexcept
    {
        if (before == after || !count(before)) return false;
        const auto i = rank(before), j = rank(after);
        if (!isWide()) { m_packed = m_packed - unit(i) + unit(j); return true; }
        if (m_counts[j] == std::numeric_limits<quint32>::max()) return false;
        if (!--m_counts[i]) m_active &= quint8(~(1u << i));
        ++m_counts[j]; m_active |= quint8(1u << j);
        return true;
    }
private:
    static constexpr std::array<Class, 8> priority{Class::Current, Class::ActivePending,
        Class::InFlight, Class::RetainedHistory, Class::OptionalCache, Class::IdleCapacity,
        Class::MetadataArena, Class::RetirementDebt};
    static constexpr size_t rank(Class cls) noexcept
    {
        constexpr std::array<quint8, 8> order{0, 3, 1, 2, 7, 4, 5, 6};
        return order[size_t(cls)];
    }
    static constexpr quint64 unit(size_t i) noexcept { return quint64(1) << (8 * i); }
    quint32 compactTotal() const noexcept
    {
        auto sum = (m_packed & 0x00ff00ff00ff00ffULL) + ((m_packed >> 8) & 0x00ff00ff00ff00ffULL);
        sum = (sum & 0x0000ffff0000ffffULL) + ((sum >> 16) & 0x0000ffff0000ffffULL);
        return quint32(sum) + quint32(sum >> 32);
    }
    void copyWide(const KisPageClassCounts &other) noexcept
    { m_counts = other.m_counts; m_total = other.m_total; m_active = other.m_active; }
    quint64 m_packed;
    // Inactive for compact groups: construction/copy/change never touch these.
    std::array<quint32, 8> m_counts;
    quint64 m_total;
    quint8 m_active;
};

struct KisPageWideClassCounts
{
    explicit KisPageWideClassCounts(KisMutationStorageAllocator<KisPageWideClassCounts> allocator)
        : storage(std::move(allocator)) {}
    KisMutationStorageAllocator<KisPageWideClassCounts> storage;
    KisPageClassCounts counts;
};

struct KisPagePhysicalBackingRecord : boost::intrusive::set_base_hook<boost::intrusive::optimize_size<true>>
{
    // One physical/account endpoint in the original resource allocation. Its
    // index hold can outlive that resource's first logical terminal when real
    // aliases remain; it never retains that terminal's provider/permissions.
    KisPageRetirementRecord *allocation = nullptr;
    KisReplicaPhysicalSlotIdentity identity;
    quint64 bytes = 0;
    struct Key {
        using type = KisReplicaPhysicalSlotIdentity;
        const type &operator()(const KisPagePhysicalBackingRecord &record) const { return record.identity; }
    };
    KisPageBackingRecord *accountMember = nullptr;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 domainRevision = 0;
    quint64 domainClaim = 0;
    const KisPagePreparedBackingChange *classClaim = nullptr;
    quint64 classCounts = 0;
    KisPageWideClassCountsPointer classStorage;
    KisPageClassCounts classes() const noexcept
    { return classCounts == KisPageClassCounts::Wide ? classStorage->counts : KisPageClassCounts(classCounts); }
    KisBackingBudgetClass currentClass() const noexcept
    { return classCounts == KisPageClassCounts::Wide ? classStorage->counts.chargedClass()
                                                    : KisPageClassCounts(classCounts).chargedClass(); }
    void installClasses(const KisPageClassCounts &next) noexcept
    {
        if (next.isWide()) { Q_ASSERT(classStorage); classStorage->counts = next; }
        classCounts = next.packed();
    }
    bool projectClassChange(KisBackingBudgetClass before, KisBackingBudgetClass after,
                            KisPageClassCounts &next) const noexcept
    {
        if (!shared()) {
            Q_ASSERT(classCounts == KisPageClassCounts::single(before).packed());
            next = KisPageClassCounts::single(after); return before != after;
        }
        next = classes(); return next.change(before, after);
    }
    void removeClass(KisBackingBudgetClass before) noexcept
    {
        // Membership has already been unlinked under this same ledger gate.
        if (!members) { classCounts = 0; return; }
        if (!shared()) { classCounts = KisPageClassCounts::single(members->budgetClass).packed(); return; }
        if (classCounts == KisPageClassCounts::Wide) {
            classStorage->counts.remove(before); classCounts = classStorage->counts.packed();
        } else {
            KisPageClassCounts next(classCounts); next.remove(before); classCounts = next.packed();
        }
    }
    bool changeClass(KisBackingBudgetClass before, KisBackingBudgetClass after) noexcept
    {
        if (!shared()) {
            Q_ASSERT(classCounts == KisPageClassCounts::single(before).packed());
            classCounts = KisPageClassCounts::single(after).packed(); return before != after;
        }
        if (classCounts == KisPageClassCounts::Wide) return classStorage->counts.change(before, after);
        KisPageClassCounts next(classCounts);
        if (!next.change(before, after)) return false;
        classCounts = next.packed(); return true;
    }
    KisBackingBudgetClass chargedClass = KisBackingBudgetClass::Count;
    KisPageBackingRecord *members = nullptr;

    quint64 byteSize() const { return bytes; }
    bool shared() const { return members && members->next; }
    // Only the original physical record edits membership, under the ledger
    // gate. Budget projections copy this view without editing any links.
    void link(KisPageBackingRecord &record) noexcept
    {
        Q_ASSERT(!record.physicalRecord);
        record.physicalRecord = this;
        if (!accountMember) accountMember = &record;
        record.next = members;
        if (members) members->previous = &record;
        members = &record;
    }
    void unlink(KisPageBackingRecord &record) noexcept
    {
        if (record.previous) record.previous->next = record.next;
        else members = record.next;
        if (record.next) record.next->previous = record.previous;
        if (accountMember == &record) accountMember = members;
        record.previous = record.next = nullptr;
        record.physicalRecord = nullptr;
    }
};

struct KisPageBackingIdentityLess
{
    bool operator()(const KisReplicaAllocationIdentity &a,
                    const KisReplicaAllocationIdentity &b) const noexcept
    {
        return std::tie(a.provider.value, a.providerEpoch.value, a.allocation.slot, a.allocation.generation)
            < std::tie(b.provider.value, b.providerEpoch.value, b.allocation.slot, b.allocation.generation);
    }
    bool operator()(const KisReplicaPhysicalSlotIdentity &a,
                    const KisReplicaPhysicalSlotIdentity &b) const noexcept
    {
        return std::tie(a.provider.value, a.providerEpoch.value, a.slot)
            < std::tie(b.provider.value, b.providerEpoch.value, b.slot);
    }
};
struct KisPageBackingNode : boost::intrusive::set_base_hook<> {
    KisReplicaAllocationIdentity first;
    KisPageBackingRecord second;
    struct Key {
        using type = KisReplicaAllocationIdentity;
        const type &operator()(const KisPageBackingNode &node) const { return node.first; }
    };
};
using KisPageBackingIndex = boost::intrusive::set<KisPageBackingNode,
    boost::intrusive::key_of_value<KisPageBackingNode::Key>,
    boost::intrusive::compare<KisPageBackingIdentityLess>>;
using KisPagePhysicalBackingIndex = boost::intrusive::set<KisPagePhysicalBackingRecord,
    boost::intrusive::key_of_value<KisPagePhysicalBackingRecord::Key>,
    boost::intrusive::compare<KisPageBackingIdentityLess>>;

// Transient ledger preparation in the caller's frozen publication row. The
// physical claim protects these locations; no copied replica or second input
// owns the operation. Generic independent preparations use the same row.
struct KisPagePreparedBackingChange
{
    KisPageBackingIndex::iterator backing;
    KisPagePhysicalBackingRecord *physical = nullptr;
    size_t order = 0;
    KisBackingBudgetClass after = KisBackingBudgetClass::Count;
    // Count means this row does not change the physical fee. In particular,
    // a reference-only group's final row must not freeze its former charge.
    KisBackingBudgetClass changedCharge = KisBackingBudgetClass::Count;
    KisPageRetirementRecord **idleResult = nullptr;
    const void *claim = nullptr;
    const KisPagePreparedBackingChange *next = nullptr;
    KisPageBackingRecord *credit = nullptr;
    KisBackingBudgetClass creditBefore = KisBackingBudgetClass::Count;
};

struct KisPageProviderOperationRecord
{
    KisCompletionTicket completion;
    KisPageRetirementRecord *retirement = nullptr;
};
struct KisPageProviderOperationNode : boost::intrusive::set_base_hook<> {
    quint64 first = 0;
    KisPageProviderOperationRecord second;
    struct Key {
        using type = quint64;
        const quint64 &operator()(const KisPageProviderOperationNode &node) const { return node.first; }
    };
};
using KisPageProviderOperationIndex = boost::intrusive::set<KisPageProviderOperationNode,
    boost::intrusive::key_of_value<KisPageProviderOperationNode::Key>>;

struct KisPageRetirementRecord : boost::intrusive::list_base_hook<>
{
    explicit KisPageRetirementRecord(KisMutationStorageAllocator<KisPageRetirementRecord> allocator)
        : storage(std::move(allocator))
    {
        operationNode.second.retirement = this;
        physical.allocation = this;
    }
    // Transfer a memory hold for the one physical/account endpoint. No second
    // allocation, admission or provider observation follows the original node.
    KRITAIMAGE_EXPORT KisPagePhysicalBackingPointer retainPhysicalEndpoint() noexcept;
    std::atomic<unsigned> storageReferences{1};
    KisMutationStorageAllocator<KisPageRetirementRecord> storage;
    // Registration, idle handoff and terminal consumption share this one
    // identity. Taking terminal ownership freezes it; no second handle is made.
    const KisReplicaHandle &replica() const noexcept { return backingNode.second.replica; }
    std::shared_ptr<KisPageReplicaProvider> provider;
    // Valid only on the non-owning publication idle chain; the ledger owns
    // this same record until checkout or original retirement handoff.
    qint64 idleDeadline = 0;
    // Fixed at original retirement-owner admission; retries never re-query
    // provider code or maintain another wake-eligibility policy.
    bool backgroundRetirement = false;
    KisCompletionTicket lastUse;
    KisPageOperationId retirementOperation;
    KisReplicaOperation retirementResult;
    KisPageProviderOperationNode operationNode;
    KisPageBackingNode backingNode;
    KisPagePhysicalBackingRecord physical;
    // Rare provider-result rejection: keep the preallocation reservation
    // charged until an unregistered physical replica reaches terminal retire.
    KisBackingBudgetReservation orphanReservation;
    // Funded once for this retirement lifetime. The queue distinguishes a
    // cached state from its currently armed subscription/permission.
    std::shared_ptr<KisPageRetirementWait> wait;
    KisBackingBudgetReservation admissionReservation;
    bool readinessRechecked = false;
    bool orphanDebtAdmitted = false;
};

// These containers own already-admitted nodes. Splice and close transfer the
// same storage; no partition or pass creates another record allocation.
struct KisPageRetirementRecords : boost::intrusive::list<KisPageRetirementRecord> {
    KisPageRetirementRecords() = default;
    KisPageRetirementRecords(KisPageRetirementRecords &&other) noexcept { swap(other); }
    KisPageRetirementRecords &operator=(KisPageRetirementRecords &&other) noexcept
    { if (this != &other) { clear_and_dispose(KisPageRetirementRecordDeleter{}); swap(other); } return *this; }
    ~KisPageRetirementRecords() { clear_and_dispose(KisPageRetirementRecordDeleter{}); }
};
struct KisPageBackingPreparation {
    KisBackingBudgetReservation reservation;
    KisPageRetirementRecordPointer retirement;
};

#endif
