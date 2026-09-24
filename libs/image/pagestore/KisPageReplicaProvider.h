/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_REPLICA_PROVIDER_H
#define KIS_PAGE_REPLICA_PROVIDER_H

#include <QString>
#include <QSharedPointer>
#include <QVector>

#include "KisPageStoreTypes.h"

class KisCpuResidentBinding;

struct KRITAIMAGE_EXPORT KisCpuResidentReplicaProviderConfig
{
    KisReplicaProviderId provider;
    KisReplicaProviderEpoch providerEpoch;
    quint64 budgetBytes = 0;

    bool isValid() const
    { return provider.isValid() && providerEpoch.isValid() && budgetBytes != 0; }
};

/** Provider-issued immutable input, independent of target PageKey/generation.
 * Keeps the exact backing alive without requiring its bytes to stay resident.
 * It grants initialization/aliasing only, never canonical version authority.
 * Each provider validates its own mint identity in prepareSynchronousSource().
 */
class KRITAIMAGE_EXPORT KisPageReplicaSource
{
public:
    virtual ~KisPageReplicaSource();
    KisReplicaProviderId provider() const { return m_provider; }
    KisReplicaProviderEpoch providerEpoch() const { return m_epoch; }
    const KisPageAllocationDescriptor &descriptor() const { return m_descriptor; }
protected:
    KisPageReplicaSource(KisReplicaProviderId provider, KisReplicaProviderEpoch epoch,
                         const KisPageAllocationDescriptor &descriptor)
        : m_provider(provider), m_epoch(epoch), m_descriptor(descriptor) {}
private:
    const KisReplicaProviderId m_provider;
    const KisReplicaProviderEpoch m_epoch;
    const KisPageAllocationDescriptor m_descriptor;
};

enum class KisReplicaSourceUse : quint8 { ImmutableAlias, WritableCopy };

struct KRITAIMAGE_EXPORT KisReplicaCapabilities
{
    // domains describe allocatable storage; consumerAccess lists exact views
    // available for every allocation in the matching domain. Keeping the
    // pairs explicit avoids inventing a domain x access-kind cross product.
    QVector<KisPageAccessDomain> domains;
    QVector<KisPageAccessRequirement> consumerAccess;
    // Every operation returns a registry-terminal completion before returning.
    // False means the provider requires the async coordinator path.
    bool synchronousOperations = false;
    // Optional same-provider, same-domain CPU preserve initialization. The
    // returned independent allocation already contains the exact source bytes.
    bool synchronousWriteCopy = false;
    // Stronger, explicit capability: native writable bindings and independent
    // first-write backing, with generic/native readers, copies and retirement
    // sharing the same physical gate. Copy support alone is not sufficient.
    bool nativeCpuMutation = false;
    // Immutable provider-issued inputs can be aliased or copied into an
    // independent writable target. No PageKey-indexed mutable seed mailbox.
    bool synchronousSourceAdoption = false;
    // Explicit opt-in: retire/validate/completion bookkeeping can run on the
    // reclamation worker concurrently with foreground allocation and access.
    // Synchronous completion alone does not imply this threading contract.
    bool backgroundRetirement = false;
    // Independent CPU backing initialized directly from a complete borrowed
    // host payload, synchronously, before it can be exposed or swapped.
    bool synchronousCpuPayload = false;

    bool isValid() const
    {
        if (domains.isEmpty()) return false;
        for (qsizetype i = 0; i < domains.size(); ++i) {
            const KisPageAccessDomain domain = domains.at(i);
            if (domain == KisPageAccessDomain::Unknown) return false;
            for (qsizetype j = 0; j < i; ++j) {
                if (domain == domains.at(j)) return false;
            }

            bool hasConsumer = false;
            for (const KisPageAccessRequirement access : consumerAccess) {
                if (access.domain == domain) hasConsumer = true;
            }
            if (domain == KisPageAccessDomain::Ssd) {
                if (hasConsumer) return false;
            } else if (!hasConsumer) {
                return false;
            }
        }
        for (qsizetype i = 0; i < consumerAccess.size(); ++i) {
            const KisPageAccessRequirement access = consumerAccess.at(i);
            if (!access.isValid() || !domains.contains(access.domain)) return false;
            for (qsizetype j = 0; j < i; ++j) {
                if (access == consumerAccess.at(j)) return false;
            }
        }
        const KisPageAccessRequirement umaCpu{
            KisPageAccessDomain::UmaShared, KisPageAccessKind::CpuPointer};
        if (synchronousWriteCopy &&
            (!synchronousOperations ||
             (!consumerAccess.contains({KisPageAccessDomain::CpuRam,
                                         KisPageAccessKind::CpuPointer}) &&
              !consumerAccess.contains(umaCpu)))) return false;
        if (nativeCpuMutation && (!synchronousWriteCopy ||
            !consumerAccess.contains({KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}))) return false;
        if (backgroundRetirement && !synchronousOperations) return false;
        if (synchronousSourceAdoption && !nativeCpuMutation) return false;
        if (synchronousCpuPayload && (!synchronousOperations ||
            !consumerAccess.contains({KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}))) return false;
        return true;
    }

    bool supports(KisPageAccessRequirement access) const
    {
        return access.isValid() && consumerAccess.contains(access);
    }
};

// Borrowed rows for one complete logical page, not an alias/retention or a
// promise to initialize later. The caller keeps these bytes stable and readable
// until the synchronous call returns; providers must never retain this pointer.
// Padding between source rows is not payload. Bounds include the last row only.
struct KRITAIMAGE_EXPORT KisCpuPagePayload
{
    const void *data = nullptr;
    qsizetype rowStride = 0;
    qsizetype byteSize = 0;
    bool isValidFor(const KisPageAllocationDescriptor &descriptor) const;
};

/**
 * Provider-resolved access for one PageStore lease. It never owns canonical
 * identity; PageStore keeps the corresponding replica pinned until
 * releaseAccess() consumes the same lease identity and last-use ticket.
 */
struct KRITAIMAGE_EXPORT KisReplicaAccess
{
    KisReplicaAccess() = default;
    KisReplicaAccess(const KisReplicaAccess &) = delete;
    KisReplicaAccess &operator=(const KisReplicaAccess &) = delete;
    KisReplicaAccess(KisReplicaAccess &&rhs) noexcept
    {
        moveFrom(std::move(rhs));
    }
    KisReplicaAccess &operator=(KisReplicaAccess &&) = delete;

    KisPageLeaseId lease;
    KisPageOperationId operation;
    KisReplicaHandle replica;
    KisPageAccessMode mode = KisPageAccessMode::Read;
    const void *cpuReadData = nullptr;
    void *cpuWriteData = nullptr;
    KisGpuPageAccessView gpuAccess;

    bool isValid(KisPageAccessRequirement expected = {}) const
    {
        if (!lease.isValid() || !operation.isValid() || !replica.isValid()) {
            return false;
        }
        if (!expected.isValid()) {
            if (mode == KisPageAccessMode::Read ? cpuReadData : cpuWriteData)
                expected = {replica.domain, KisPageAccessKind::CpuPointer};
            else if (gpuAccess.isValid())
                expected = {replica.domain, KisPageAccessKind::GpuBinding};
        }
        if (!expected.isValid() || replica.domain != expected.domain) return false;
        if (expected.kind == KisPageAccessKind::CpuPointer) {
            return mode == KisPageAccessMode::Read
                ? cpuReadData != nullptr : cpuWriteData != nullptr;
        }
        return gpuAccess.isValid() &&
               gpuAccess.providerId == replica.provider.value &&
               gpuAccess.providerEpoch == replica.providerEpoch.value &&
               gpuAccess.operationId == operation.value &&
               gpuAccess.byteSize == replica.layout.byteSize;
    }

private:
    void moveFrom(KisReplicaAccess &&rhs) noexcept
    {
        lease = rhs.lease;
        operation = rhs.operation;
        replica = rhs.replica;
        mode = rhs.mode;
        cpuReadData = rhs.cpuReadData;
        cpuWriteData = rhs.cpuWriteData;
        gpuAccess = rhs.gpuAccess;
        rhs.lease = {};
        rhs.operation = {};
        rhs.replica = {};
        rhs.cpuReadData = nullptr;
        rhs.cpuWriteData = nullptr;
        rhs.gpuAccess = {};
    }
};

struct KRITAIMAGE_EXPORT KisReplicaOperation
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageOperationId operation;
    KisReplicaHandle replica;
    KisCompletionTicket completion;
    QString error;

    static KisReplicaOperation failed(KisPageOperationId operation, const QString &error)
    {
        return {KisPageRequestStatus::Failed, operation, {}, {}, error};
    }

    bool isValid() const
    {
        return operation.isValid() && replica.isValid() && completion.isValid() &&
               (status == KisPageRequestStatus::Pending ||
                status == KisPageRequestStatus::Ready);
    }
};

enum class KisReplicaTransferKind : quint8 {
    ExactVersionMaterialization,
    WriteGenerationInitialization
};

struct KRITAIMAGE_EXPORT KisReplicaTransferRequest
{
    KisPageOperationId operation;
    KisReplicaHandle source;
    KisReplicaHandle target;
    KisPageAllocationDescriptor descriptor;
    KisReplicaTransferKind kind = KisReplicaTransferKind::ExactVersionMaterialization;

    bool isValid() const
    {
        if (!operation.isValid() || !source.isValid() || !target.isValid() ||
            !descriptor.isValid() || source == target ||
            !source.layout.matches(descriptor) ||
            !target.layout.matches(descriptor)) {
            return false;
        }
        if (kind == KisReplicaTransferKind::ExactVersionMaterialization) {
            return source.version == target.version;
        }
        return source.version.key == target.version.key &&
               target.version.generation.value > source.version.generation.value;
    }

    bool isSameProviderTransfer() const
    {
        return isValid() && source.provider == target.provider &&
               source.providerEpoch == target.providerEpoch;
    }
};

struct KRITAIMAGE_EXPORT KisReplicaMemoryUsage
{
    quint64 residentBytes = 0;
    quint64 committedBytes = 0;
    quint64 budgetBytes = 0;
};

/** Provider-scoped physical payload identity. Multiple logical replica
 * handles may report the same identity when immutable aliasing shares bytes.
 * The owner ledger charges that payload exactly once.
 */
struct KRITAIMAGE_EXPORT KisReplicaBackingFootprint
{
    quint64 physicalSlot = 0;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 bytes = 0;
    quint64 revision = 1;

    bool isValid() const
    {
        return physicalSlot != 0 && domain != KisPageAccessDomain::Unknown
            && bytes != 0 && revision != 0;
    }
};

enum class KisBackingRevisionOrder : quint8 {
    Older,
    Same,
    Newer,
    Ambiguous
};

/** RFC-1982 style comparison permits the storage revision to skip zero on
 * wrap. More than half the 64-bit sequence space cannot remain unobserved. */
inline KisBackingRevisionOrder kisCompareBackingRevision(quint64 candidate,
                                                         quint64 installed)
{
    if (!candidate || !installed || candidate == installed)
        return candidate == installed && candidate
            ? KisBackingRevisionOrder::Same
            : KisBackingRevisionOrder::Ambiguous;
    constexpr quint64 halfRange = quint64(1) << 63;
    const quint64 distance = candidate - installed;
    if (distance == halfRange)
        return KisBackingRevisionOrder::Ambiguous;
    return distance < halfRange ? KisBackingRevisionOrder::Newer
                                : KisBackingRevisionOrder::Older;
}

/** Provider-coalesced physical-domain change. Revision is minted at the
 * physical storage transition, not when an observer callback arrives. An
 * acknowledgement applies only to this exact revision; a newer change for
 * the same physical slot remains pending. */
struct KRITAIMAGE_EXPORT KisReplicaBackingDomainChange
{
    quint64 revision = 0;
    quint64 physicalSlot = 0;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    quint64 bytes = 0;

    bool isValid() const
    {
        return revision != 0 && physicalSlot != 0
            && domain != KisPageAccessDomain::Unknown && bytes != 0;
    }
};

/**
 * Backend-neutral physical replica interface. PageStore is the only intended
 * caller; consumers receive leases instead of provider-owned allocations.
 */
class KRITAIMAGE_EXPORT KisPageReplicaProvider
{
public:
    virtual ~KisPageReplicaProvider();

    virtual QString name() const = 0;
    virtual KisReplicaProviderId providerId() const = 0;
    virtual KisReplicaProviderEpoch providerEpoch() const = 0;
    virtual KisReplicaCapabilities capabilities() const = 0;

    virtual KisReplicaOperation requestReplica(KisPageOperationId operation,
                                                const KisPageVersion &version,
                                                const KisPageAllocationDescriptor &descriptor,
                                                KisPageAccessDomain domain,
                                                KisPageAccessMode mode,
                                                KisPagePriority priority) = 0;
    virtual KisReplicaOperation prepareWrite(KisPageOperationId operation,
                                              const KisPageVersion &version,
                                              const KisPageAllocationDescriptor &descriptor,
                                              KisPageAccessDomain domain,
                                              KisPageWriteMode mode,
                                              KisPagePriority priority) = 0;
    // Optional direct initialization. Return independent mutable storage with
    // every logical pixel initialized, not an alias of caller memory. Ordinary
    // prepareWrite/default/discard semantics are deliberately unchanged.
    virtual KisReplicaOperation prepareSynchronousCpuPayload(
        KisPageOperationId operation, const KisPageVersion &version,
        const KisPageAllocationDescriptor &descriptor, const KisCpuPagePayload &payload,
        KisPagePriority priority);
    virtual KisReplicaOperation transfer(const KisReplicaTransferRequest &request,
                                          KisPagePriority priority) = 0;
    virtual KisReplicaAccess resolveAccess(KisPageLeaseId lease,
                                           KisPageOperationId operation,
                                           const KisReplicaHandle &replica,
                                           KisPageAccessRequirement requirement,
                                           KisPageAccessMode mode) = 0;
    virtual void releaseAccess(KisReplicaAccess access,
                               const KisCompletionTicket &lastUse) = 0;
    virtual bool validate(const KisReplicaHandle &replica,
                          const KisPageAllocationDescriptor &descriptor) const = 0;
    virtual KisReplicaOperation retire(KisPageOperationId operation,
                                       const KisReplicaHandle &replica,
                                       const KisCompletionTicket &lastUse) = 0;
    virtual KisReplicaMemoryUsage memoryUsage() const = 0;
    virtual KisReplicaBackingFootprint backingFootprint(
        const KisReplicaHandle &replica) const;
    // Mutable backing domains report only changed physical slots. Fixed-domain
    // providers keep the empty default. A change is retained until the owner
    // acknowledges its exact sequence after budget installation.
    virtual QVector<KisReplicaBackingDomainChange> backingDomainChanges() const;
    virtual void acknowledgeBackingDomainChange(quint64 physicalSlot,
                                                quint64 revision);

    /**
     * Optional fused allocation + preserve initialization. The owner protects
     * the exact immutable source lifetime/order; the provider pins its backing
     * during copying. Only a terminal ready result with an independent writable
     * target is success. No publication, source mutation or second transfer is
     * implied. On failure no target may escape. Unsupported does no work; the
     * owner selects fallback before dispatch using synchronousWriteCopy.
     */
    virtual KisReplicaOperation prepareSynchronousWriteCopy(
        KisPageOperationId operation,
        const KisReplicaHandle &source,
        const KisPageVersion &targetVersion,
        const KisPageAllocationDescriptor &descriptor,
        KisPagePriority priority);

    // Optional immutable-input preparation. The owner supplies the new target
    // generation; the provider checks its mint identity and exact layout.
    // ImmutableAlias retains input sharing, never exposes writable bytes and
    // need not fault in storage. WritableCopy must be independent before return.
    // Only terminal success is usable; no visibility/root change is implied.
    virtual KisReplicaOperation prepareSynchronousSource(
        KisPageOperationId operation,
        const QSharedPointer<const KisPageReplicaSource> &source,
        const KisPageVersion &targetVersion,
        const KisPageAllocationDescriptor &descriptor,
        KisReplicaSourceUse use, KisPagePriority priority);
    // Synchronous initialization of an already owner-authorized CPU span.
    // The caller holds its writable guard throughout; no lease/publication.
    virtual bool copySynchronousSourceToCpu(
        const QSharedPointer<const KisPageReplicaSource> &source,
        const KisPageAllocationDescriptor &descriptor, void *destination,
        quint32 rowStride, quint64 byteSize);

    // Optional stable CPU allocation record. Retrieval is cold binding setup,
    // not pixel access; consumers still need an owner-issued root capability.
    // Unsupported providers return null and retain the generic lease path.
    virtual QSharedPointer<KisCpuResidentBinding> cpuResidentBinding(
        const KisReplicaHandle &replica, KisCpuResidentReadStatus *status = nullptr) const;
};

/**
 * Explicit cross-provider copy seam. Provider::transfer() is intentionally
 * restricted to allocations owned by one provider epoch; a bridge must be
 * registered for CPU staging, UMA sharing, Vulkan copies, or SSD I/O.
 */
class KRITAIMAGE_EXPORT KisPageReplicaTransferBridge
{
public:
    virtual ~KisPageReplicaTransferBridge();

    // The BR1 CPU facade accepts only terminal-at-return bridges. Async
    // bridges are wired later through coordinator-owned logical readiness.
    virtual bool synchronousOperations() const = 0;
    virtual bool supports(const KisReplicaTransferRequest &request) const = 0;
    virtual KisReplicaOperation transfer(
        const KisReplicaTransferRequest &request,
        const QSharedPointer<KisPageReplicaProvider> &sourceProvider,
        const QSharedPointer<KisPageReplicaProvider> &targetProvider,
        KisPagePriority priority) = 0;
};

struct KRITAIMAGE_EXPORT KisExactPageArchiveWrite
{
    KisPageOperationId operation;
    KisPageVersion version;
    KisPageAllocationDescriptor descriptor;
    KisReplicaLayout sourceLayout;
    const void *sourceData = nullptr;

    bool isValid() const
    {
        return operation.isValid() && version.isValid() &&
               descriptor.isValid() && sourceLayout.matches(descriptor) &&
               sourceData != nullptr;
    }
};

struct KRITAIMAGE_EXPORT KisPageArchiveOperation
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageOperationId operation;
    KisPageVersion version;
    KisCompletionTicket completion;
    QString error;

    bool isValid() const
    {
        return operation.isValid() && version.isValid() &&
               completion.isValid() &&
               (status == KisPageRequestStatus::Pending ||
                status == KisPageRequestStatus::Ready);
    }
};

/**
 * BR1 seam for evolving the existing swap store without claiming BR3 crash
 * durability. Archive records are always keyed by exact PageVersion and do
 * not become authority merely because a write succeeds.
 */
class KRITAIMAGE_EXPORT KisExactGenerationArchive
{
public:
    virtual ~KisExactGenerationArchive();

    virtual QString name() const = 0;
    virtual KisReplicaProviderId archiveId() const = 0;
    virtual KisReplicaProviderEpoch archiveEpoch() const = 0;
    virtual bool isOperational() const = 0;
    virtual bool synchronousOperations() const = 0;
    virtual KisPageArchiveOperation storeExact(
        const KisExactPageArchiveWrite &write,
        KisPagePriority priority) = 0;
    // Requests cancellation of a still-pending storeExact operation. A true
    // return guarantees that its completion ticket will become terminal
    // Cancelled; already-terminal operations return false.
    virtual bool cancelStoreExact(KisPageOperationId operation) = 0;
    virtual bool loadExact(const KisPageVersion &version,
                           const KisPageAllocationDescriptor &descriptor,
                           QByteArray *bytes,
                           KisReplicaLayout *layout = nullptr) const = 0;
    virtual bool contains(const KisPageVersion &version) const = 0;
    virtual bool forget(const KisPageVersion &version) = 0;
};

#endif // KIS_PAGE_REPLICA_PROVIDER_H
