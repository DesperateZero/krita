/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_TYPES_H
#define KIS_PAGE_STORE_TYPES_H

#include <QByteArray>
#include <QHashFunctions>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <atomic>
#include <limits>
#include <memory>

#include "kritaimage_export.h"

struct KRITAIMAGE_EXPORT KisSurfaceId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisSurfaceId lhs, KisSurfaceId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisLogicalPageId
{
    qint32 column = 0;
    qint32 row = 0;
};

inline bool operator==(KisLogicalPageId lhs, KisLogicalPageId rhs)
{
    return lhs.column == rhs.column && lhs.row == rhs.row;
}

struct KRITAIMAGE_EXPORT KisPageGeneration
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

struct KRITAIMAGE_EXPORT KisSurfaceGeneration
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisSurfaceGeneration lhs, KisSurfaceGeneration rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisSurfaceVersion
{
    KisSurfaceId surface;
    KisSurfaceGeneration generation;

    bool isValid() const
    {
        return surface.isValid() && generation.isValid();
    }
};

inline bool operator==(const KisSurfaceVersion &lhs, const KisSurfaceVersion &rhs)
{
    return lhs.surface == rhs.surface && lhs.generation == rhs.generation;
}

struct KRITAIMAGE_EXPORT KisPageKey
{
    KisSurfaceId surface;
    KisLogicalPageId page;

    bool isValid() const { return surface.isValid(); }
};

inline bool operator==(const KisPageKey &lhs, const KisPageKey &rhs)
{
    return lhs.surface == rhs.surface && lhs.page == rhs.page;
}

/** Stable across a document session; unlike QHash's seed it is suitable for
 * assigning a PageKey to a metadata shard. */
inline quint64 kisStablePageKeyHash(const KisPageKey &key) noexcept
{
    quint64 hash = 1469598103934665603ULL;
    auto mix = [&hash](quint64 value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= value & 0xffU;
            hash *= 1099511628211ULL;
            value >>= 8;
        }
    };
    mix(key.surface.value);
    mix(quint32(key.page.column));
    mix(quint32(key.page.row));
    return hash;
}

inline size_t qHash(const KisLogicalPageId &page, size_t seed = 0) noexcept
{
    return qHash((quint64(quint32(page.column)) << 32) | quint32(page.row), seed);
}

inline size_t qHash(const KisPageKey &key, size_t seed = 0) noexcept
{
    return qHash(kisStablePageKeyHash(key), seed);
}

inline bool operator==(KisPageGeneration lhs, KisPageGeneration rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageVersion
{
    KisPageKey key;
    KisPageGeneration generation;
    /**
     * Non-zero only when this version denotes the immutable default-pixel
     * sentinel of a surface epoch. Sentinel versions deliberately keep a
     * normal page generation so they can participate in the same lease and
     * transition machinery as allocated pages; the metadata revision keeps
     * two historical defaults from aliasing.
     */
    quint64 defaultPixelRevision = 0;

    bool isValid() const { return key.isValid() && generation.isValid(); }
    bool isDefaultPixel() const { return defaultPixelRevision != 0; }
};

inline bool operator==(const KisPageVersion &lhs, const KisPageVersion &rhs)
{
    return lhs.key == rhs.key && lhs.generation == rhs.generation &&
           lhs.defaultPixelRevision == rhs.defaultPixelRevision;
}

inline size_t qHash(const KisPageVersion &version, size_t seed = 0) noexcept
{
    seed = qHash(version.key, seed);
    seed = qHash(version.generation.value, seed);
    return qHash(version.defaultPixelRevision, seed);
}

struct KRITAIMAGE_EXPORT KisImageEpochId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisImageEpochId lhs, KisImageEpochId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisImageEpochSnapshotToken
{
    quint64 value = 0;
    bool isValid() const { return value != 0; }
};

inline bool operator==(KisImageEpochSnapshotToken lhs,
                       KisImageEpochSnapshotToken rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageTransactionId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisPageTransactionId lhs, KisPageTransactionId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageRequestId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisPageRequestId lhs, KisPageRequestId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageOperationId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisPageOperationId lhs, KisPageOperationId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageWriterToken
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisPageWriterToken lhs, KisPageWriterToken rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageLeaseId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisPageLeaseId lhs, KisPageLeaseId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisReplicaProviderId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisReplicaProviderId lhs, KisReplicaProviderId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisReplicaProviderEpoch
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisReplicaProviderEpoch lhs, KisReplicaProviderEpoch rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisReplicaAllocationToken
{
    quint64 slot = 0;
    quint64 generation = 0;

    bool isValid() const { return slot != 0 && generation != 0; }
};

inline bool operator==(const KisReplicaAllocationToken &lhs,
                       const KisReplicaAllocationToken &rhs)
{
    return lhs.slot == rhs.slot && lhs.generation == rhs.generation;
}

/** Provider-scoped allocation identity without logical version/layout state. */
struct KisReplicaAllocationIdentity
{
    KisReplicaProviderId provider;
    KisReplicaProviderEpoch providerEpoch;
    KisReplicaAllocationToken allocation;

    bool isValid() const
    {
        return provider.isValid() && providerEpoch.isValid() &&
               allocation.isValid();
    }
};

inline bool operator==(const KisReplicaAllocationIdentity &lhs,
                       const KisReplicaAllocationIdentity &rhs)
{
    return lhs.provider == rhs.provider &&
           lhs.providerEpoch == rhs.providerEpoch &&
           lhs.allocation == rhs.allocation;
}

inline size_t qHash(const KisReplicaAllocationIdentity &identity,
                    size_t seed = 0) noexcept
{
    seed = ::qHash(identity.provider.value, seed);
    seed = ::qHash(identity.providerEpoch.value, seed);
    seed = ::qHash(identity.allocation.slot, seed);
    return ::qHash(identity.allocation.generation, seed);
}

/** Stable physical slot identity; allocation generation is intentionally excluded. */
struct KisReplicaPhysicalSlotIdentity
{
    KisReplicaProviderId provider;
    KisReplicaProviderEpoch providerEpoch;
    quint64 slot = 0;
};

inline bool operator==(const KisReplicaPhysicalSlotIdentity &lhs,
                       const KisReplicaPhysicalSlotIdentity &rhs)
{
    return lhs.provider == rhs.provider && lhs.providerEpoch == rhs.providerEpoch
        && lhs.slot == rhs.slot;
}

inline size_t qHash(const KisReplicaPhysicalSlotIdentity &identity,
                    size_t seed = 0) noexcept
{
    return ::qHash(identity.slot, ::qHash(identity.providerEpoch.value,
                                          ::qHash(identity.provider.value, seed)));
}

enum class KisPageAccessDomain : quint8 {
    Unknown,
    CpuRam,
    UmaShared,
    DiscreteVram,
    Ssd
};

enum class KisPageAccessMode : quint8 {
    Read,
    Write
};

// Outcome of a resident-only try, not a materialization request status.
// In particular, Busy/BindingUnavailable/UnsupportedProvider are not faults.
enum class KisCpuResidentReadStatus : quint8 {
    Ready,
    InvalidView,
    InvalidIdentity,
    VirtualDefault,
    BindingUnavailable,
    UnsupportedProvider,
    Busy,
    NonResident,
    Retired,
    ResourceUnavailable
};

enum class KisPageAccessKind : quint8 {
    Unknown,
    CpuPointer,
    GpuBinding
};

struct KRITAIMAGE_EXPORT KisPageAccessRequirement
{
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    KisPageAccessKind kind = KisPageAccessKind::Unknown;

    bool isValid() const
    {
        if (kind == KisPageAccessKind::CpuPointer) {
            return domain == KisPageAccessDomain::CpuRam ||
                   domain == KisPageAccessDomain::UmaShared;
        }
        if (kind == KisPageAccessKind::GpuBinding) {
            return domain == KisPageAccessDomain::UmaShared ||
                   domain == KisPageAccessDomain::DiscreteVram;
        }
        return false;
    }
};

inline bool operator==(KisPageAccessRequirement lhs,
                       KisPageAccessRequirement rhs)
{
    return lhs.domain == rhs.domain && lhs.kind == rhs.kind;
}

enum class KisPageWriteMode : quint8 {
    PreserveContents,
    DiscardContents
};

enum class KisPagePriority : quint8 {
    Background,
    Normal,
    Interactive,
    Critical
};

enum class KisPageRequestStatus : quint8 {
    Unsupported,
    Pending,
    Ready,
    Failed,
    Cancelled
};

enum class KisSurfaceAlphaSemantic : quint8 {
    Unspecified,
    Premultiplied,
    Straight,
    Opaque,
    SelectionMask
};

enum class KisSurfaceEndianness : quint8 {
    Unspecified,
    LittleEndian,
    BigEndian,
    NativeEndian
};

struct KRITAIMAGE_EXPORT KisSurfaceFormat
{
    quint64 formatId = 0;
    QByteArray colorModelId;
    QByteArray colorDepthId;
    QByteArray profileFingerprint;
    QByteArray channelOrder;
    QByteArray packing;
    QByteArray defaultPixel;
    quint32 channelCount = 0;
    quint32 pixelStride = 0;
    quint32 pixelAlignment = 0;
    bool hasAlpha = false;
    KisSurfaceAlphaSemantic alphaSemantic = KisSurfaceAlphaSemantic::Unspecified;
    KisSurfaceEndianness endianness = KisSurfaceEndianness::Unspecified;
    quint32 codecVersion = 0;

    bool isValid() const
    {
        return formatId != 0 &&
               !colorModelId.isEmpty() &&
               !colorDepthId.isEmpty() &&
               !profileFingerprint.isEmpty() &&
               !channelOrder.isEmpty() &&
               !packing.isEmpty() &&
               channelCount > 0 &&
               pixelStride > 0 &&
               pixelAlignment > 0 &&
               (pixelAlignment & (pixelAlignment - 1)) == 0 &&
               pixelStride % pixelAlignment == 0 &&
               defaultPixel.size() == qsizetype(pixelStride) &&
               alphaSemantic != KisSurfaceAlphaSemantic::Unspecified &&
               endianness != KisSurfaceEndianness::Unspecified &&
               codecVersion != 0;
    }
};

inline bool operator==(const KisSurfaceFormat &lhs,
                       const KisSurfaceFormat &rhs)
{
    return lhs.formatId == rhs.formatId &&
           lhs.colorModelId == rhs.colorModelId &&
           lhs.colorDepthId == rhs.colorDepthId &&
           lhs.profileFingerprint == rhs.profileFingerprint &&
           lhs.channelOrder == rhs.channelOrder &&
           lhs.packing == rhs.packing &&
           lhs.defaultPixel == rhs.defaultPixel &&
           lhs.channelCount == rhs.channelCount &&
           lhs.pixelStride == rhs.pixelStride &&
           lhs.pixelAlignment == rhs.pixelAlignment &&
           lhs.hasAlpha == rhs.hasAlpha &&
           lhs.alphaSemantic == rhs.alphaSemantic &&
           lhs.endianness == rhs.endianness &&
           lhs.codecVersion == rhs.codecVersion;
}

enum class KisPageInitialization : quint8 {
    Uninitialized,
    DefaultPixel
};

/**
 * Canonical logical allocation request. Providers may choose a larger physical
 * row stride, but may not change the format, logical extent, valid rectangle,
 * or layout revision. DefaultPixel is the only legal implicit page content.
 */
struct KRITAIMAGE_EXPORT KisPageAllocationDescriptor
{
    quint64 layoutRevision = 0;
    KisSurfaceFormat format;
    QSize pageExtent;
    QRect validRect;
    quint32 rowAlignment = 1;
    KisPageInitialization initialization = KisPageInitialization::Uninitialized;

    quint64 minimumRowBytes() const
    {
        return pageExtent.width() > 0
            ? quint64(pageExtent.width()) * format.pixelStride : 0;
    }

    quint64 minimumByteSize() const
    {
        if (pageExtent.height() <= 0) return 0;
        const quint64 rowBytes = minimumRowBytes();
        const quint64 height = quint64(pageExtent.height());
        return rowBytes <= std::numeric_limits<quint64>::max() / height
            ? rowBytes * height : 0;
    }

    bool isValid() const
    {
        if (layoutRevision == 0 || !format.isValid() ||
            !pageExtent.isValid() || validRect.isEmpty() ||
            rowAlignment == 0 ||
            (rowAlignment & (rowAlignment - 1)) != 0) {
            return false;
        }
        const QRect pageBounds(QPoint(0, 0), pageExtent);
        if (!pageBounds.contains(validRect)) return false;

        const quint64 rowBytes = minimumRowBytes();
        const quint64 byteSize = minimumByteSize();
        return rowBytes >= format.pixelStride &&
               rowBytes <= quint64(std::numeric_limits<quint32>::max()) &&
               byteSize != 0 &&
               byteSize / quint64(pageExtent.height()) == rowBytes;
    }
};

inline bool operator==(const KisPageAllocationDescriptor &lhs,
                       const KisPageAllocationDescriptor &rhs)
{
    return lhs.layoutRevision == rhs.layoutRevision &&
           lhs.format == rhs.format &&
           lhs.pageExtent == rhs.pageExtent &&
           lhs.validRect == rhs.validRect &&
           lhs.rowAlignment == rhs.rowAlignment &&
           lhs.initialization == rhs.initialization;
}

/** Provider-selected physical layout, always qualified by canonical format. */
struct KRITAIMAGE_EXPORT KisReplicaLayout
{
    quint64 layoutRevision = 0;
    quint64 formatId = 0;
    QSize pageExtent;
    QRect validRect;
    quint32 rowStride = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        if (layoutRevision == 0 || formatId == 0 || !pageExtent.isValid() ||
            validRect.isEmpty() || rowStride == 0 || byteSize == 0) {
            return false;
        }
        const QRect pageBounds(QPoint(0, 0), pageExtent);
        return pageBounds.contains(validRect) &&
               byteSize >= quint64(rowStride) * quint64(pageExtent.height());
    }

    bool matches(const KisPageAllocationDescriptor &descriptor) const
    {
        return isValid() && descriptor.isValid() &&
               layoutRevision == descriptor.layoutRevision &&
               formatId == descriptor.format.formatId &&
               pageExtent == descriptor.pageExtent &&
               validRect == descriptor.validRect &&
               rowStride >= descriptor.minimumRowBytes() &&
               rowStride % descriptor.rowAlignment == 0 &&
               rowStride % descriptor.format.pixelAlignment == 0;
    }
};

inline bool operator==(const KisReplicaLayout &lhs,
                       const KisReplicaLayout &rhs)
{
    return lhs.layoutRevision == rhs.layoutRevision &&
           lhs.formatId == rhs.formatId &&
           lhs.pageExtent == rhs.pageExtent &&
           lhs.validRect == rhs.validRect &&
           lhs.rowStride == rhs.rowStride &&
           lhs.byteSize == rhs.byteSize;
}

struct KRITAIMAGE_EXPORT KisReplicaHandle
{
    KisReplicaProviderId provider;
    KisReplicaProviderEpoch providerEpoch;
    KisReplicaAllocationToken allocation;
    KisPageVersion version;
    KisPageAccessDomain domain = KisPageAccessDomain::Unknown;
    KisReplicaLayout layout;

    KisReplicaAllocationIdentity allocationIdentity() const { return {provider, providerEpoch, allocation}; }
    KisReplicaPhysicalSlotIdentity physicalSlotIdentity() const { return {provider, providerEpoch, allocation.slot}; }

    bool isValid() const
    {
        return allocationIdentity().isValid() && version.isValid() &&
               domain != KisPageAccessDomain::Unknown && layout.isValid();
    }
};

inline bool operator==(const KisReplicaHandle &lhs, const KisReplicaHandle &rhs)
{
    return lhs.allocationIdentity() == rhs.allocationIdentity() &&
           lhs.version == rhs.version && lhs.domain == rhs.domain &&
           lhs.layout == rhs.layout;
}

enum class KisPageReadViewKind : quint8 {
    CurrentCommittedEpoch,
    CommittedEpoch,
    TransactionBaseEpoch,
    TransactionOverlay,
    ExactVersion
};

/**
 * Immutable selector captured by acquireRead(). A request always resolves one
 * page to one exact generation before any asynchronous materialization starts.
 * Multi-page consistency is provided by a shared epoch/transaction selector,
 * not by allowing a request to chase each page's latest head independently.
 */
struct KRITAIMAGE_EXPORT KisPageReadView
{
    KisPageReadViewKind kind = KisPageReadViewKind::CurrentCommittedEpoch;
    KisImageEpochId epoch;
    KisImageEpochSnapshotToken retention;
    KisPageTransactionId transaction;
    KisPageVersion exactVersion;

    static KisPageReadView transactionBase(KisPageTransactionId transaction)
    {
        return {KisPageReadViewKind::TransactionBaseEpoch, {}, {}, transaction, {}};
    }

    static KisPageReadView transactionOverlay(KisPageTransactionId transaction)
    {
        return {KisPageReadViewKind::TransactionOverlay, {}, {}, transaction, {}};
    }

    bool isValidFor(KisPageKey key) const
    {
        switch (kind) {
        case KisPageReadViewKind::CurrentCommittedEpoch:
            return key.isValid() && !epoch.isValid() && !retention.isValid() &&
                   !transaction.isValid() && !exactVersion.isValid();
        case KisPageReadViewKind::CommittedEpoch:
            return key.isValid() && epoch.isValid() && retention.isValid() &&
                   !transaction.isValid() && !exactVersion.isValid();
        case KisPageReadViewKind::TransactionBaseEpoch:
        case KisPageReadViewKind::TransactionOverlay:
            return key.isValid() && !epoch.isValid() && !retention.isValid() &&
                   transaction.isValid() && !exactVersion.isValid();
        case KisPageReadViewKind::ExactVersion:
            return key.isValid() && epoch.isValid() && retention.isValid() &&
                   !transaction.isValid() &&
                   exactVersion.isValid() && exactVersion.key == key;
        }
        return false;
    }
};

/**
 * Backend-neutral, operation-scoped GPU access description. The values are
 * identifiers understood only by the replica provider that issued the lease;
 * they are never native pointers, VkBuffer handles, or global page-table keys.
 */
struct KRITAIMAGE_EXPORT KisGpuPageAccessView
{
    quint64 providerId = 0;
    quint64 providerEpoch = 0;
    quint64 operationId = 0;
    quint64 bindingTableGeneration = 0;
    quint64 bindingIndex = 0;
    quint64 byteOffset = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return providerId != 0 &&
               providerEpoch != 0 &&
               operationId != 0 &&
               bindingTableGeneration != 0 &&
               byteSize != 0;
    }
};

/**
 * Backend-neutral completion identity. Public code can only create an invalid
 * ticket; only KisCompletionRegistry may allocate a registered source/value.
 */
enum class KisCompletionDomain : quint8 {
    Unknown,
    CpuJob,
    GpuTimeline,
    IoOperation,
    HostLogical
};

class KRITAIMAGE_EXPORT KisCompletionTicket
{
public:
    KisCompletionTicket() = default;

    bool isValid() const
    {
        return m_domain != KisCompletionDomain::Unknown &&
               m_registry != 0 && m_source != 0 && m_value != 0;
    }
    KisCompletionDomain domain() const { return m_domain; }
    quint64 registry() const { return m_registry; }
    quint64 source() const { return m_source; }
    quint64 value() const { return m_value; }

    bool isOrderedWith(const KisCompletionTicket &other) const
    {
        return isValid() && other.isValid() &&
               m_domain == KisCompletionDomain::GpuTimeline &&
               other.m_domain == KisCompletionDomain::GpuTimeline &&
               m_registry == other.m_registry &&
               m_source == other.m_source;
    }

private:
    KisCompletionTicket(KisCompletionDomain domain,
                        quint64 registry,
                        quint64 source,
                        quint64 value)
        : m_domain(domain)
        , m_registry(registry)
        , m_source(source)
        , m_value(value)
    {
    }

    KisCompletionDomain m_domain = KisCompletionDomain::Unknown;
    quint64 m_registry = 0;
    quint64 m_source = 0;
    quint64 m_value = 0;

    friend class KisCompletionRegistry;
};

inline bool operator==(const KisCompletionTicket &lhs,
                       const KisCompletionTicket &rhs)
{
    return lhs.domain() == rhs.domain() &&
           lhs.registry() == rhs.registry() &&
           lhs.source() == rhs.source() && lhs.value() == rhs.value();
}

struct KRITAIMAGE_EXPORT KisReadRequest
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageRequestId id;
    KisPageVersion version;
    KisPageAccessRequirement access;
    KisCompletionTicket readiness;
    QString error;

    bool isValid() const
    {
        return id.isValid() && version.isValid() &&
               access.isValid() &&
               readiness.isValid() &&
               (status == KisPageRequestStatus::Pending ||
                status == KisPageRequestStatus::Ready);
    }
};

struct KRITAIMAGE_EXPORT KisWriteRequest
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageRequestId id;
    KisPageTransactionId transaction;
    KisPageVersion baseVersion;
    KisPageVersion writeVersion;
    KisPageWriterToken writer;
    KisPageAccessRequirement access;
    KisPageWriteMode mode = KisPageWriteMode::PreserveContents;
    KisCompletionTicket readiness;
    QString error;

    bool isValid() const
    {
        return id.isValid() && transaction.isValid() && baseVersion.isValid() &&
               writeVersion.isValid() && baseVersion.key == writeVersion.key &&
               writeVersion.generation.value > baseVersion.generation.value &&
               writer.isValid() && access.isValid() &&
               readiness.isValid() &&
               (status == KisPageRequestStatus::Pending ||
                status == KisPageRequestStatus::Ready);
    }
};

namespace KisPageStoreDetail
{
template<typename Id>
inline Id allocateMonotonicId(std::atomic<quint64> *next)
{
    quint64 candidate = next->load(std::memory_order_relaxed);
    while (candidate != 0 && candidate != std::numeric_limits<quint64>::max()) {
        if (next->compare_exchange_weak(candidate, candidate + 1,
                                        std::memory_order_relaxed,
                                        std::memory_order_relaxed)) {
            return Id{candidate};
        }
    }
    return {};
}

inline void setError(QString *error, const QString &value)
{
    if (error) *error = value;
}

inline bool leaseAccessIsValid(const KisPageAccessRequirement &access,
                               const void *cpuData,
                               const KisGpuPageAccessView &gpuAccess,
                               quint32 rowStride,
                               quint64 byteSize)
{
    return access.isValid() && rowStride != 0 && byteSize != 0 &&
           rowStride <= byteSize &&
           (access.kind == KisPageAccessKind::CpuPointer
                ? cpuData != nullptr
                : gpuAccess.isValid() && gpuAccess.byteSize == byteSize);
}
}

/**
 * Leases are move-only capabilities. Copying a read lease would duplicate an
 * untracked retirement reference just as copying a write lease would duplicate
 * a writer. The PageStore release path consumes the lease and records last use.
 */
class KRITAIMAGE_EXPORT KisReadLease
{
public:
    KisReadLease() = default;
    KisReadLease(const KisReadLease &) = delete;
    KisReadLease &operator=(const KisReadLease &) = delete;
    KisReadLease(KisReadLease &&) noexcept = default;
    KisReadLease &operator=(KisReadLease &&) = delete;

    bool isValid() const
    {
        return m_lifetime && m_leaseId.isValid() && m_version.isValid() &&
               KisPageStoreDetail::leaseAccessIsValid(
                   m_access, m_cpuData, m_gpuAccess, m_rowStride, m_byteSize);
    }
    KisPageLeaseId leaseId() const { return m_lifetime ? m_leaseId : KisPageLeaseId{}; }
    KisPageVersion version() const { return m_lifetime ? m_version : KisPageVersion{}; }
    const void *cpuData() const { return m_lifetime ? m_cpuData : nullptr; }
    quint32 rowStride() const { return m_lifetime ? m_rowStride : 0; }
    quint64 byteSize() const { return m_lifetime ? m_byteSize : 0; }

private:
    KisPageLeaseId m_leaseId;
    KisPageVersion m_version;
    KisPageAccessRequirement m_access;
    const void *m_cpuData = nullptr;
    KisGpuPageAccessView m_gpuAccess;
    quint32 m_rowStride = 0;
    quint64 m_byteSize = 0;
    // Keeps the provider access record alive if the PageStore is destroyed
    // before a caller consumes this move-only capability.
    std::shared_ptr<void> m_lifetime;

    friend class KisPageReadCoordinator;
    friend class KisPageStore;
};

/**
 * A write lease is deliberately move-only: a page generation may never gain
 * multiple writers by copying a lease.
 */
class KRITAIMAGE_EXPORT KisWriteLease
{
public:
    KisWriteLease() = default;
    KisWriteLease(const KisWriteLease &) = delete;
    KisWriteLease &operator=(const KisWriteLease &) = delete;
    KisWriteLease(KisWriteLease &&) noexcept = default;
    KisWriteLease &operator=(KisWriteLease &&) = delete;

    bool isValid() const
    {
        return m_lifetime && m_leaseId.isValid() && m_baseVersion.isValid() &&
               m_version.isValid() && m_baseVersion.key == m_version.key &&
               m_writer.isValid() && m_transaction.isValid() && m_layout.isValid() &&
               KisPageStoreDetail::leaseAccessIsValid(
                   m_access, m_cpuData, m_gpuAccess,
                   m_layout.rowStride, m_layout.byteSize);
    }
    KisPageLeaseId leaseId() const { return m_lifetime ? m_leaseId : KisPageLeaseId{}; }
    KisPageVersion version() const { return m_lifetime ? m_version : KisPageVersion{}; }
    KisPageAccessKind accessKind() const { return m_lifetime ? m_access.kind : KisPageAccessKind::Unknown; }
    void *cpuData() const { return m_lifetime ? m_cpuData : nullptr; }
    quint32 rowStride() const { return m_lifetime ? m_layout.rowStride : 0; }
    quint64 byteSize() const { return m_lifetime ? m_layout.byteSize : 0; }
    KisReplicaLayout layout() const { return m_lifetime ? m_layout : KisReplicaLayout{}; }

private:
    KisPageLeaseId m_leaseId;
    KisPageVersion m_baseVersion;
    KisPageVersion m_version;
    KisPageWriterToken m_writer;
    KisPageTransactionId m_transaction;
    KisPageAccessRequirement m_access;
    void *m_cpuData = nullptr;
    KisGpuPageAccessView m_gpuAccess;
    KisReplicaLayout m_layout;
    std::shared_ptr<void> m_lifetime;

    friend class KisPageStore;
};

struct KRITAIMAGE_EXPORT KisPageTransaction
{
    KisPageTransactionId id;
    KisImageEpochId baseEpoch;

    bool isValid() const { return id.isValid() && baseEpoch.isValid(); }
};

inline bool operator==(const KisPageTransaction &lhs, const KisPageTransaction &rhs)
{ return lhs.id == rhs.id && lhs.baseEpoch == rhs.baseEpoch; }

/**
 * Immutable surface metadata carried by an ImageEpoch root.  An absent
 * PageKey resolves to this surface's default-pixel sentinel, so historical
 * reads and undo never consult mutable out-of-band defaults or extents.
 */
struct KRITAIMAGE_EXPORT KisSurfaceEpochState
{
    KisSurfaceId surface;
    KisSurfaceFormat format;
    /**
     * Exact sparse content bounds in surface coordinates.  A QSize is not
     * sufficient here: tiles3 extents may start at a negative coordinate and
     * undo/save must retain that origin together with the dimensions.
     * QRect() denotes an empty surface.
     */
    QRect contentExtent;
    QSize logicalPageExtent;
    quint64 layoutRevision = 0;
    quint32 rowAlignment = 1;
    quint64 defaultPixelRevision = 0;
    quint64 extentRevision = 0;

    bool isValid() const
    {
        const bool extentIsValid = contentExtent.isNull() ||
                                   contentExtent.isValid();
        return surface.isValid() && format.isValid() && extentIsValid &&
               logicalPageExtent.isValid() && layoutRevision != 0 &&
               rowAlignment != 0 &&
               (rowAlignment & (rowAlignment - 1)) == 0 &&
               defaultPixelRevision != 0 && extentRevision != 0;
    }

    KisPageAllocationDescriptor allocationDescriptor() const
    {
        KisPageAllocationDescriptor descriptor;
        descriptor.layoutRevision = layoutRevision;
        descriptor.format = format;
        descriptor.pageExtent = logicalPageExtent;
        descriptor.validRect = QRect(QPoint(0, 0), logicalPageExtent);
        descriptor.rowAlignment = rowAlignment;
        descriptor.initialization = KisPageInitialization::DefaultPixel;
        return descriptor;
    }
};

inline bool operator==(const KisSurfaceEpochState &lhs,
                       const KisSurfaceEpochState &rhs)
{
    return lhs.surface == rhs.surface && lhs.format == rhs.format &&
           lhs.contentExtent == rhs.contentExtent &&
           lhs.logicalPageExtent == rhs.logicalPageExtent &&
           lhs.layoutRevision == rhs.layoutRevision &&
           lhs.rowAlignment == rhs.rowAlignment &&
           lhs.defaultPixelRevision == rhs.defaultPixelRevision &&
           lhs.extentRevision == rhs.extentRevision;
}

struct KRITAIMAGE_EXPORT KisPreparedPageProof
{
    KisPageTransactionId transaction;
    KisReplicaHandle authority;
    KisCompletionTicket producerCompletion;
    quint64 providerValidationStamp = 0;

    bool isValid() const
    {
        return transaction.isValid() && authority.isValid() &&
               producerCompletion.isValid() && providerValidationStamp != 0;
    }
};

inline bool operator==(const KisPreparedPageProof &lhs,
                       const KisPreparedPageProof &rhs)
{
    return lhs.transaction == rhs.transaction
        && lhs.authority == rhs.authority
        && lhs.producerCompletion == rhs.producerCompletion
        && lhs.providerValidationStamp == rhs.providerValidationStamp;
}

struct KRITAIMAGE_EXPORT KisSurfaceEpochChange
{
    KisSurfaceEpochState before;
    KisSurfaceEpochState after;

    bool isValid() const
    {
        if (!before.isValid() || !after.isValid() ||
            !(before.surface == after.surface)) {
            return false;
        }
        // Layout and format identity are immutable for a SurfaceId. The
        // default bytes and pixel extent are epoch metadata and may advance
        // only with their matching monotonically increasing revision.
        KisSurfaceFormat expectedFormat = before.format;
        expectedFormat.defaultPixel = after.format.defaultPixel;
        if (!(expectedFormat == after.format) ||
            before.logicalPageExtent != after.logicalPageExtent ||
            before.layoutRevision != after.layoutRevision ||
            before.rowAlignment != after.rowAlignment) {
            return false;
        }
        // A sealed overlay may have changed A -> B -> A. Its final identity
        // still advances: captured B and old A readers retain their identities.
        const bool defaultChanged =
            before.defaultPixelRevision != after.defaultPixelRevision;
        if (before.format.defaultPixel != after.format.defaultPixel && !defaultChanged)
            return false;
        const bool extentChanged = before.contentExtent != after.contentExtent;
        return (defaultChanged || extentChanged) &&
               (defaultChanged
                    ? after.defaultPixelRevision > before.defaultPixelRevision
                    : after.defaultPixelRevision == before.defaultPixelRevision) &&
               (extentChanged
                    ? after.extentRevision > before.extentRevision
                    : after.extentRevision == before.extentRevision);
    }
};

inline bool operator==(const KisSurfaceEpochChange &lhs,
                       const KisSurfaceEpochChange &rhs)
{
    return lhs.before == rhs.before && lhs.after == rhs.after;
}

struct KRITAIMAGE_EXPORT KisPreparedPageSet
{
    KisPageTransactionId transaction;
    QVector<KisPreparedPageProof> proofs;
    QVector<KisSurfaceEpochChange> surfaceChanges;
    QVector<KisPageKey> removedPages;

    bool isValid() const
    {
        if (!transaction.isValid() ||
            (proofs.isEmpty() && surfaceChanges.isEmpty() &&
             removedPages.isEmpty())) return false;
        QSet<KisPageKey> proofKeys;
        proofKeys.reserve(proofs.size());
        for (const KisPreparedPageProof &proof : proofs) {
            if (!proof.isValid() || !(proof.transaction == transaction)) return false;
            if (proofKeys.contains(proof.authority.version.key)) return false;
            proofKeys.insert(proof.authority.version.key);
        }
        QSet<quint64> changedSurfaces;
        changedSurfaces.reserve(surfaceChanges.size());
        for (const KisSurfaceEpochChange &change : surfaceChanges) {
            if (!change.isValid() ||
                changedSurfaces.contains(change.after.surface.value)) {
                return false;
            }
            changedSurfaces.insert(change.after.surface.value);
        }
        QSet<KisPageKey> removedKeys;
        removedKeys.reserve(removedPages.size());
        for (const KisPageKey &removed : removedPages) {
            if (!removed.isValid() || proofKeys.contains(removed) ||
                removedKeys.contains(removed)) return false;
            removedKeys.insert(removed);
        }
        return true;
    }
};

struct KRITAIMAGE_EXPORT KisImageEpochCommitTicket
{
    KisImageEpochId epoch;
    KisCompletionTicket completion;

    bool isValid() const { return epoch.isValid() && completion.isValid(); }
};

struct KRITAIMAGE_EXPORT KisImageEpochSnapshot
{
    KisImageEpochId epoch;
    quint64 graphRevision = 0;
    quint64 defaultPixelRevision = 0;
    quint64 extentRevision = 0;
    quint64 propertyRevision = 0;
    QVector<KisPageVersion> manifest;
    QVector<KisSurfaceEpochState> surfaces;

    bool isValid() const
    {
        if (!epoch.isValid() || graphRevision == 0 ||
            defaultPixelRevision == 0 || extentRevision == 0 ||
            propertyRevision == 0) {
            return false;
        }
        QSet<KisPageKey> manifestKeys;
        manifestKeys.reserve(manifest.size());
        for (const KisPageVersion &version : manifest) {
            if (!version.isValid() || manifestKeys.contains(version.key)) {
                return false;
            }
            manifestKeys.insert(version.key);
        }
        QSet<quint64> surfaceIds;
        surfaceIds.reserve(surfaces.size());
        for (const KisSurfaceEpochState &surface : surfaces) {
            if (!surface.isValid() ||
                surfaceIds.contains(surface.surface.value)) {
                return false;
            }
            surfaceIds.insert(surface.surface.value);
        }
        return true;
    }
};

struct KRITAIMAGE_EXPORT KisRetainedImageEpochSnapshot
{
    KisImageEpochSnapshotToken token;
    KisImageEpochSnapshot snapshot;
    // The token-owned immutable root is authoritative for validation and
    // restore. A complete manifest is an optional cold-path export payload,
    // not part of the hot retention capability. A negative pageCount denotes
    // a complete manifest; structurally-shared root capabilities store the
    // count and leave the manifest empty.
    qsizetype pageCount = -1;

    bool hasCompleteManifest() const { return pageCount < 0; }

    bool isValid() const
    {
        if (!token.isValid() || !snapshot.isValid()) return false;
        return hasCompleteManifest() || snapshot.manifest.isEmpty();
    }

    qsizetype retainedPageCount() const
    {
        return pageCount >= 0 ? pageCount : snapshot.manifest.size();
    }
};

struct KRITAIMAGE_EXPORT KisPageStoreSessionStats
{
    bool configured = false;
    bool operational = false;
    bool closed = false;
    qsizetype registeredPages = 0;
    qsizetype pageVersions = 0;
    qsizetype replicas = 0;
    qsizetype immutableRoots = 0;
    qsizetype activeTransactions = 0;
    qsizetype retainedSnapshots = 0;
    qsizetype pendingRequests = 0;
    qsizetype activeReadLeases = 0;
    qsizetype activeWriteLeases = 0;
    // Owner-gated per-page reservations, including preparation before the
    // first generic request exists. Claims are not pixel-access capabilities.
    qsizetype activeControlWritePages = 0;
    qsizetype activeCpuWritePages = 0;
    // Native claims include semantic removals; this is not a RAM-pin count.
    qsizetype pendingLastUses = 0;
    qsizetype preparedPageProofs = 0;
    qsizetype preparedSurfaceChanges = 0;
    qsizetype stagedPageRemovals = 0;
    qsizetype activeProviderCalls = 0;
    // All bound provider operations, including detached retirement. This is
    // a lifecycle/debt count, not the number of publication-blocking actions.
    qsizetype providerOperations = 0;
    qsizetype sealedPreparedProofs = 0;
    qsizetype pendingArchiveOperations = 0;
    qsizetype pendingShutdownReplicas = 0;
    // Logically detached replicas still awaiting successful physical release.
    // This is retirement debt, not live history or permission to reuse bytes.
    qsizetype pendingRetiredReplicas = 0;
    qsizetype activeRetirementReplicas = 0;
    quint64 pendingRetiredBytes = 0;
    // Reclamation work/debt, not reader/writer authority. Deferred history may
    // still be logically reachable or pinned; it is not all immediately freeable.
    qsizetype pendingHistoricalPages = 0;
    qsizetype deferredHistoricalPages = 0;
    qsizetype activeHistoryScans = 0;
    // Derived conservative reachability entries, not live version ownership
    // or byte/RSS accounting. Completed sweep caches are destroyed off owner.
    quint64 cachedHistoryReachableVersions = 0;
    qsizetype scheduledReclamationJobs = 0;
    quint64 backgroundRetirementPasses = 0;
    quint64 maximumReplicasPerRetirementPass = 0;
    qsizetype peakRetiredReplicas = 0;
    quint64 peakRetiredBytes = 0;
    quint64 retirementRetryRequeues = 0;
    quint64 retirementCloseDrainedReplicas = 0;
    qsizetype cachedDefaultReadBuffers = 0;
    quint64 cachedDefaultReadBytes = 0;
    quint64 liveDefaultReadBytes = 0;
    quint64 defaultReadBuffersCreated = 0;
    quint64 defaultReadInitializedBytes = 0;
    quint64 defaultReadCacheEvictions = 0;
    quint64 defaultReadCacheOversizeBypasses = 0;
    // Default materialization admission is owner-gated and bounded. Active
    // preparations participate in close eligibility; peak/waits are monotonic
    // observability for the service's hard entry budget and backpressure.
    qsizetype activeDefaultPreparations = 0;
    qsizetype peakDefaultPreparations = 0;
    quint64 defaultPreparationWaits = 0;

    // Monotonic diagnostics. Unlike the capability counts above, these
    // counters are not part of close-session eligibility. They make it
    // possible to prove that a resident CPU fast path did not silently fall
    // back to the generic request/materialization machinery.
    quint64 readRequestsCreated = 0;
    quint64 writeRequestsCreated = 0;
    quint64 defaultMaterializationRequests = 0;
    quint64 synchronousHostWrites = 0;
    quint64 committedTransactions = 0;
    bool hasOutstandingCapabilities() const
    {
        return activeTransactions != 0 || retainedSnapshots != 0 ||
               pendingRequests != 0 || activeReadLeases != 0 ||
               activeWriteLeases != 0 || activeControlWritePages != 0 ||
               activeCpuWritePages != 0 || pendingLastUses != 0 ||
               preparedPageProofs != 0 || preparedSurfaceChanges != 0 ||
               stagedPageRemovals != 0 ||
               activeDefaultPreparations != 0 ||
               activeProviderCalls != 0 || providerOperations != 0 ||
               sealedPreparedProofs != 0 || pendingArchiveOperations != 0 ||
               pendingShutdownReplicas != 0 || pendingRetiredReplicas != 0;
    }
};

#endif // KIS_PAGE_STORE_TYPES_H
