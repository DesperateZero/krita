/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_BACKEND_SERVICE_REGISTRY_H
#define KIS_BACKEND_SERVICE_REGISTRY_H

#include <QByteArray>
#include <QScopedPointer>
#include <QString>

#include "KisDocumentGpuSession.h"

struct KRITAIMAGE_EXPORT KisBackendValidationReport
{
    quint64 serviceId = 0;
    quint64 deviceGeneration = 0;
    quint64 allocatorGeneration = 0;
    quint64 coordinatorGeneration = 0;
    quint64 replicaProviderGeneration = 0;
    quint64 shaderAbi = 0;
    QByteArray queueTopologyDigest;
    QByteArray capabilityDigest;
    QByteArray shaderManifestDigest;
    QByteArray smokeProbeDigest;
    bool deviceReady = false;
    bool queuesReady = false;
    bool shadersReady = false;
    bool coordinatorReady = false;
    bool replicaProviderReady = false;
    QString failureReason;

    bool isComplete() const
    {
        return serviceId != 0 &&
               deviceGeneration != 0 &&
               allocatorGeneration != 0 &&
               coordinatorGeneration != 0 &&
               replicaProviderGeneration != 0 &&
               shaderAbi != 0 &&
               !queueTopologyDigest.isEmpty() &&
               !capabilityDigest.isEmpty() &&
               !shaderManifestDigest.isEmpty() &&
               !smokeProbeDigest.isEmpty() &&
               deviceReady &&
               queuesReady &&
               shadersReady &&
               coordinatorReady &&
               replicaProviderReady &&
               failureReason.isEmpty();
    }
};

/**
 * Immutable device-service discovery/validation boundary. It is the only core
 * class allowed to construct BackendReadyToken; document/canvas state is never
 * stored here.
 */
class KRITAIMAGE_EXPORT KisBackendServiceRegistry
{
public:
    KisBackendServiceRegistry();
    ~KisBackendServiceRegistry();

    bool isOperational() const;
    KisBackendReadyToken acceptValidatedService(const KisBackendValidationReport &report);
    bool isCurrent(const KisBackendReadyToken &token) const;
    bool revokeService(quint64 serviceId,
                       quint64 deviceGeneration,
                       const QString &reason);
    QString revocationReason(quint64 serviceId) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_BACKEND_SERVICE_REGISTRY_H
