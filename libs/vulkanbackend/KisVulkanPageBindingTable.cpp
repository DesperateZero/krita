/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanPageBindingTable.h"

#include <QMutex>
#include <QMutexLocker>

QString KisVulkanPageBindingSnapshot::validationError() const
{
    if (providerId == 0 || operationId == 0 || tableGeneration == 0) {
        return QStringLiteral("Binding snapshot identity or generation is missing");
    }
    if (bindings.isEmpty() || !ready.isValid()) {
        return QStringLiteral("Binding snapshot has no bindings or readiness ticket");
    }
    for (const KisVulkanPageBinding &binding : bindings) {
        if (!binding.isValid() ||
            binding.access.providerId != providerId ||
            binding.access.operationId != operationId ||
            binding.access.bindingTableGeneration != tableGeneration) {
            return QStringLiteral("Binding snapshot contains an invalid or foreign binding");
        }
    }
    return {};
}

class KisVulkanPageBindingTable::Private
{
public:
    mutable QMutex mutex;
    quint64 providerId = 0;
    KisVulkanPageBindingSnapshot current;
};

KisVulkanPageBindingTable::KisVulkanPageBindingTable()
    : d(new Private)
{
}

KisVulkanPageBindingTable::~KisVulkanPageBindingTable() = default;

bool KisVulkanPageBindingTable::configure(quint64 providerId)
{
    if (providerId == 0) return false;
    QMutexLocker locker(&d->mutex);
    if (d->providerId != 0) return false;
    d->providerId = providerId;
    return true;
}

bool KisVulkanPageBindingTable::isOperational() const
{
    QMutexLocker locker(&d->mutex);
    return d->providerId != 0;
}

bool KisVulkanPageBindingTable::publishCompleted(
    const KisVulkanPageBindingSnapshot &snapshot,
    const KisCompletionTicket &completedTicket,
    QString *error)
{
    const QString snapshotError = snapshot.validationError();
    QMutexLocker locker(&d->mutex);
    if (!snapshotError.isEmpty()) {
        if (error) *error = snapshotError;
        return false;
    }
    if (snapshot.providerId != d->providerId) {
        if (error) *error = QStringLiteral("Binding snapshot belongs to another provider");
        return false;
    }
    if (!completedTicket.isValid() ||
        completedTicket.source() != snapshot.ready.source() ||
        completedTicket.value() < snapshot.ready.value()) {
        if (error) *error = QStringLiteral("Binding snapshot producer has not completed");
        return false;
    }
    if (d->current.isValid() && snapshot.tableGeneration <= d->current.tableGeneration) {
        if (error) *error = QStringLiteral("Binding table generation is not monotonic");
        return false;
    }
    d->current = snapshot;
    return true;
}

KisVulkanPageBinding KisVulkanPageBindingTable::lookup(
    const KisPageVersion &version,
    quint64 operationId) const
{
    if (!version.isValid() || operationId == 0) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->current.isValid() || d->current.operationId != operationId) return {};
    for (const KisVulkanPageBinding &binding : d->current.bindings) {
        if (binding.version.key == version.key &&
            binding.version.generation.value == version.generation.value) {
            return binding;
        }
    }
    return {};
}

quint64 KisVulkanPageBindingTable::currentTableGeneration() const
{
    QMutexLocker locker(&d->mutex);
    return d->current.tableGeneration;
}

void KisVulkanPageBindingTable::invalidateOperation(quint64 operationId)
{
    QMutexLocker locker(&d->mutex);
    if (operationId != 0 && d->current.operationId == operationId) {
        d->current = {};
    }
}
