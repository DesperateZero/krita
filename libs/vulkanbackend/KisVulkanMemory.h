/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_MEMORY_H
#define KIS_VULKAN_MEMORY_H

#include <QScopedPointer>
#include <QString>

#include "KisPageStoreTypes.h"

enum class KisVulkanMemoryClass : quint8 {
    UmaCanonical,
    DeviceLocalPage,
    HostBacking,
    Staging,
    DerivedImage,
    ImmutableResource,
    Transient
};

struct KisVulkanMemoryBudget
{
    quint64 hardLimitBytes = 0;
    quint64 highWatermarkBytes = 0;
    quint64 lowWatermarkBytes = 0;

    bool isValid() const
    {
        return hardLimitBytes != 0 &&
               lowWatermarkBytes < highWatermarkBytes &&
               highWatermarkBytes <= hardLimitBytes;
    }
};

struct KisVulkanMemoryReservationRequest
{
    KisVulkanMemoryClass memoryClass = KisVulkanMemoryClass::Transient;
    quint64 bytes = 0;
    quint64 alignment = 0;
    quint64 ownerSession = 0;
    bool evictable = false;
    QString debugName;

    bool isValid() const
    {
        return bytes != 0 && alignment != 0 &&
               (alignment & (alignment - 1)) == 0 &&
               ownerSession != 0 && !debugName.isEmpty();
    }
};

struct KisVulkanMemoryReservation
{
    quint64 reservationId = 0;
    quint64 ledgerGeneration = 0;
    KisVulkanMemoryClass memoryClass = KisVulkanMemoryClass::Transient;
    quint64 bytes = 0;
    quint64 ownerSession = 0;
    bool evictable = false;

    bool isValid() const
    {
        return reservationId != 0 && ledgerGeneration != 0 && bytes != 0 && ownerSession != 0;
    }
};

struct KisVulkanMemoryUsageSnapshot
{
    quint64 ledgerGeneration = 0;
    quint64 reservedBytes = 0;
    quint64 evictableBytes = 0;
    quint64 highWatermarkBytes = 0;
    quint64 hardLimitBytes = 0;

    bool aboveHighWatermark() const
    {
        return highWatermarkBytes != 0 && reservedBytes >= highWatermarkBytes;
    }
};

/**
 * Allocation-independent accounting gate. A future VMA allocator must obtain
 * a reservation before creating native memory and release it only after the
 * allocation's last-use ticket has completed.
 */
class KisVulkanMemoryBudgetLedger
{
public:
    KisVulkanMemoryBudgetLedger();
    ~KisVulkanMemoryBudgetLedger();

    bool configure(quint64 generation, const KisVulkanMemoryBudget &budget);
    KisVulkanMemoryReservation reserve(const KisVulkanMemoryReservationRequest &request,
                                       QString *error = nullptr);
    bool release(const KisVulkanMemoryReservation &reservation);
    KisVulkanMemoryUsageSnapshot usage() const;
    bool isConfigured() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

struct KisVulkanNativeAllocation
{
    quint64 allocationId = 0;
    quint64 deviceGeneration = 0;
    KisVulkanMemoryReservation reservation;
    quint64 byteOffset = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return allocationId != 0 && deviceGeneration != 0 &&
               reservation.isValid() && byteSize != 0;
    }
};

class KisVulkanMemoryAllocator
{
public:
    virtual ~KisVulkanMemoryAllocator() = default;

    virtual quint64 deviceGeneration() const = 0;
    virtual KisVulkanNativeAllocation allocate(
        const KisVulkanMemoryReservation &reservation,
        QString *error) = 0;
    virtual bool retire(const KisVulkanNativeAllocation &allocation,
                        const KisCompletionTicket &lastUse) = 0;
};

#endif // KIS_VULKAN_MEMORY_H
