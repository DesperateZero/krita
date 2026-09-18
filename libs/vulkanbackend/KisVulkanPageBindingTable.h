/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_PAGE_BINDING_TABLE_H
#define KIS_VULKAN_PAGE_BINDING_TABLE_H

#include <QScopedPointer>
#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

struct KisVulkanPageBinding
{
    KisPageVersion version;
    KisGpuPageAccessView access;
    quint64 resourceId = 0;
    quint64 resourceGeneration = 0;

    bool isValid() const
    {
        return version.isValid() && access.isValid() &&
               resourceId != 0 && resourceGeneration != 0;
    }
};

struct KisVulkanPageBindingSnapshot
{
    quint64 providerId = 0;
    quint64 operationId = 0;
    quint64 tableGeneration = 0;
    QVector<KisVulkanPageBinding> bindings;
    KisCompletionTicket ready;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
};

/**
 * Immutable operation-scoped software page table. A completed snapshot is
 * published atomically; lookup always requires an exact page generation.
 */
class KisVulkanPageBindingTable
{
public:
    KisVulkanPageBindingTable();
    ~KisVulkanPageBindingTable();

    bool configure(quint64 providerId);
    bool isOperational() const;
    bool publishCompleted(const KisVulkanPageBindingSnapshot &snapshot,
                          const KisCompletionTicket &completedTicket,
                          QString *error = nullptr);
    KisVulkanPageBinding lookup(const KisPageVersion &version,
                                quint64 operationId) const;
    quint64 currentTableGeneration() const;
    void invalidateOperation(quint64 operationId);

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_PAGE_BINDING_TABLE_H
