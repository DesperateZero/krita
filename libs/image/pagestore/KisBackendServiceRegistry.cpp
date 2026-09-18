/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBackendServiceRegistry.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

struct RegisteredBackendService
{
    KisBackendValidationReport report;
    bool current = true;
    QString revocationReason;
};

class KisBackendServiceRegistry::Private
{
public:
    mutable QMutex mutex;
    QHash<quint64, RegisteredBackendService> services;
};

KisBackendServiceRegistry::KisBackendServiceRegistry()
    : d(new Private)
{
}

KisBackendServiceRegistry::~KisBackendServiceRegistry() = default;

bool KisBackendServiceRegistry::isOperational() const
{
    return true;
}

KisBackendReadyToken KisBackendServiceRegistry::acceptValidatedService(
    const KisBackendValidationReport &report)
{
    if (!report.isComplete()) {
        return {};
    }

    QMutexLocker locker(&d->mutex);
    auto existing = d->services.find(report.serviceId);
    if (existing != d->services.end()) {
        if (existing->current &&
            existing->report.deviceGeneration == report.deviceGeneration &&
            existing->report.allocatorGeneration == report.allocatorGeneration &&
            existing->report.coordinatorGeneration == report.coordinatorGeneration &&
            existing->report.replicaProviderGeneration == report.replicaProviderGeneration &&
            existing->report.shaderAbi == report.shaderAbi &&
            existing->report.queueTopologyDigest == report.queueTopologyDigest &&
            existing->report.capabilityDigest == report.capabilityDigest &&
            existing->report.shaderManifestDigest == report.shaderManifestDigest &&
            existing->report.smokeProbeDigest == report.smokeProbeDigest) {
            return KisBackendReadyToken(report.serviceId,
                                        report.deviceGeneration,
                                        report.shaderAbi);
        }
        if (existing->current ||
            report.deviceGeneration <= existing->report.deviceGeneration) {
            return {};
        }

        RegisteredBackendService replacement;
        replacement.report = report;
        existing.value() = replacement;
        return KisBackendReadyToken(report.serviceId,
                                    report.deviceGeneration,
                                    report.shaderAbi);
    }

    RegisteredBackendService service;
    service.report = report;
    d->services.insert(report.serviceId, service);
    return KisBackendReadyToken(report.serviceId,
                                report.deviceGeneration,
                                report.shaderAbi);
}

bool KisBackendServiceRegistry::isCurrent(const KisBackendReadyToken &token) const
{
    if (!token.isValid()) {
        return false;
    }

    QMutexLocker locker(&d->mutex);
    const auto it = d->services.constFind(token.serviceId());
    return it != d->services.constEnd() &&
           it->current &&
           it->report.deviceGeneration == token.deviceGeneration() &&
           it->report.shaderAbi == token.shaderAbi();
}

bool KisBackendServiceRegistry::revokeService(quint64 serviceId,
                                              quint64 deviceGeneration,
                                              const QString &reason)
{
    if (serviceId == 0 || deviceGeneration == 0 || reason.isEmpty()) {
        return false;
    }

    QMutexLocker locker(&d->mutex);
    auto it = d->services.find(serviceId);
    if (it == d->services.end() ||
        !it->current ||
        it->report.deviceGeneration != deviceGeneration) {
        return false;
    }

    it->current = false;
    it->revocationReason = reason;
    return true;
}

QString KisBackendServiceRegistry::revocationReason(quint64 serviceId) const
{
    QMutexLocker locker(&d->mutex);
    return d->services.value(serviceId).revocationReason;
}
