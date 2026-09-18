/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_TYPES_H
#define KIS_PAGE_STORE_TYPES_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

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

inline bool operator==(KisPageGeneration lhs, KisPageGeneration rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisPageVersion
{
    KisPageKey key;
    KisPageGeneration generation;

    bool isValid() const { return key.isValid() && generation.isValid(); }
};

inline bool operator==(const KisPageVersion &lhs, const KisPageVersion &rhs)
{
    return lhs.key == rhs.key && lhs.generation == rhs.generation;
}

struct KRITAIMAGE_EXPORT KisImageEpochId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

struct KRITAIMAGE_EXPORT KisPageTransactionId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

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
               alphaSemantic != KisSurfaceAlphaSemantic::Unspecified &&
               endianness != KisSurfaceEndianness::Unspecified &&
               codecVersion != 0;
    }
};

struct KRITAIMAGE_EXPORT KisPageRange
{
    QVector<KisPageKey> pages;
    KisPageGeneration exactGeneration;

    bool isEmpty() const { return pages.isEmpty(); }
};

/**
 * Backend-neutral, operation-scoped GPU access description. The values are
 * identifiers understood only by the replica provider that issued the lease;
 * they are never native pointers, VkBuffer handles, or global page-table keys.
 */
struct KRITAIMAGE_EXPORT KisGpuPageAccessView
{
    quint64 providerId = 0;
    quint64 operationId = 0;
    quint64 bindingTableGeneration = 0;
    quint64 bindingIndex = 0;
    quint64 byteOffset = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return providerId != 0 &&
               operationId != 0 &&
               bindingTableGeneration != 0 &&
               bindingIndex != 0 &&
               byteSize != 0;
    }
};

/**
 * Backend-neutral completion identity. Public code can only create an invalid
 * ticket; only KisCompletionRegistry may allocate a registered source/value.
 */
class KRITAIMAGE_EXPORT KisCompletionTicket
{
public:
    KisCompletionTicket() = default;

    bool isValid() const { return m_source != 0 && m_value != 0; }
    quint64 source() const { return m_source; }
    quint64 value() const { return m_value; }

private:
    KisCompletionTicket(quint64 source, quint64 value)
        : m_source(source)
        , m_value(value)
    {
    }

    quint64 m_source = 0;
    quint64 m_value = 0;

    friend class KisCompletionRegistry;
};

struct KRITAIMAGE_EXPORT KisReadRequest
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageRange range;
    KisPageAccessDomain requestedDomain = KisPageAccessDomain::Unknown;
    KisCompletionTicket readiness;
    QString error;
};

struct KRITAIMAGE_EXPORT KisWriteRequest
{
    KisPageRequestStatus status = KisPageRequestStatus::Unsupported;
    KisPageRange range;
    KisPageAccessDomain requestedDomain = KisPageAccessDomain::Unknown;
    KisPageWriteMode mode = KisPageWriteMode::PreserveContents;
    KisCompletionTicket readiness;
    QString error;
};

class KRITAIMAGE_EXPORT KisReadLease
{
public:
    KisReadLease() = default;

    bool isValid() const { return m_leaseId != 0 && m_version.isValid(); }
    KisPageVersion version() const { return m_version; }
    KisPageAccessDomain domain() const { return m_domain; }
    const void *cpuData() const { return m_cpuData; }
    KisGpuPageAccessView gpuAccess() const { return m_gpuAccess; }
    quint32 rowStride() const { return m_rowStride; }

private:
    quint64 m_leaseId = 0;
    KisPageVersion m_version;
    KisPageAccessDomain m_domain = KisPageAccessDomain::Unknown;
    const void *m_cpuData = nullptr;
    KisGpuPageAccessView m_gpuAccess;
    quint32 m_rowStride = 0;

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
    KisWriteLease &operator=(KisWriteLease &&) noexcept = default;

    bool isValid() const { return m_leaseId != 0 && m_version.isValid(); }
    KisPageVersion version() const { return m_version; }
    KisPageAccessDomain domain() const { return m_domain; }
    void *cpuData() const { return m_cpuData; }
    KisGpuPageAccessView gpuAccess() const { return m_gpuAccess; }
    quint32 rowStride() const { return m_rowStride; }

private:
    quint64 m_leaseId = 0;
    KisPageVersion m_version;
    KisPageAccessDomain m_domain = KisPageAccessDomain::Unknown;
    void *m_cpuData = nullptr;
    KisGpuPageAccessView m_gpuAccess;
    quint32 m_rowStride = 0;

    friend class KisPageStore;
};

struct KRITAIMAGE_EXPORT KisPageTransaction
{
    KisPageTransactionId id;
    KisImageEpochId baseEpoch;

    bool isValid() const { return id.isValid() && baseEpoch.isValid(); }
};

struct KRITAIMAGE_EXPORT KisPreparedPageSet
{
    QVector<KisPageVersion> pages;
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
    QVector<KisPageVersion> manifest;

    bool isValid() const { return epoch.isValid(); }
};

#endif // KIS_PAGE_STORE_TYPES_H
