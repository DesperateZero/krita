/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_DOCUMENT_GPU_SESSION_H
#define KIS_DOCUMENT_GPU_SESSION_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include "KisEvaluationGraphTypes.h"

enum class KisAccelerationSessionState : quint8 {
    Disabled,
    Initializing,
    Ready,
    Draining,
    Failed,
    DeviceLost,
    Shutdown
};

enum class KisAccelerationFeature : quint8 {
    PageReplicaAccess,
    GpuStroke,
    GpuEvaluation,
    PointerBypass,
    LevelOfDetail,
    Display,
    Recovery
};

struct KRITAIMAGE_EXPORT KisEvaluationPortCapability
{
    KisEvaluationValueType type;
    quint64 surfaceFormatId = 0;

    bool isValid() const
    {
        return type.isValid() &&
               (type.isSurfaceBacked() ? surfaceFormatId != 0 : surfaceFormatId == 0);
    }
};

inline bool operator==(const KisEvaluationPortCapability &lhs,
                       const KisEvaluationPortCapability &rhs)
{
    return lhs.type == rhs.type && lhs.surfaceFormatId == rhs.surfaceFormatId;
}

struct KRITAIMAGE_EXPORT KisEvaluationOperationCapability
{
    QByteArray operationId;
    quint64 semanticVersion = 0;
    QVector<KisEvaluationPortCapability> inputPorts;
    QVector<KisEvaluationPortCapability> outputPorts;
    bool supportsPageRegions = false;
    bool requiresWholeValue = false;

    bool isValid() const
    {
        if (operationId.isEmpty() || semanticVersion == 0 || outputPorts.isEmpty()) {
            return false;
        }
        for (const KisEvaluationPortCapability &port : inputPorts) {
            if (!port.isValid()) return false;
        }
        for (const KisEvaluationPortCapability &port : outputPorts) {
            if (!port.isValid()) return false;
        }
        return true;
    }
};

struct KRITAIMAGE_EXPORT KisAccelerationCapabilities
{
    quint64 serviceId = 0;
    quint64 deviceGeneration = 0;
    quint64 sessionGeneration = 0;
    quint64 featureMask = 0;
    QVector<quint64> supportedFormatIds;
    QVector<KisEvaluationOperationCapability> evaluationOperations;
    QByteArray capabilityDigest;

    bool isValid() const
    {
        if (serviceId == 0 || deviceGeneration == 0 || sessionGeneration == 0 ||
            capabilityDigest.isEmpty()) {
            return false;
        }
        for (const KisEvaluationOperationCapability &capability : evaluationOperations) {
            if (!capability.isValid()) return false;
        }
        return true;
    }

    bool supports(KisAccelerationFeature feature) const
    {
        return (featureMask & (quint64(1) << quint8(feature))) != 0;
    }

    bool supportsFormat(quint64 formatId) const
    {
        return formatId != 0 && supportedFormatIds.contains(formatId);
    }

    bool supportsEvaluationOperation(
        const QByteArray &operationId,
        quint64 semanticVersion,
        const QVector<KisEvaluationPortCapability> &inputPorts,
        const QVector<KisEvaluationPortCapability> &outputPorts) const
    {
        if (!supports(KisAccelerationFeature::GpuEvaluation)) {
            return false;
        }
        for (const KisEvaluationOperationCapability &capability : evaluationOperations) {
            if (capability.isValid() &&
                capability.operationId == operationId &&
                capability.semanticVersion == semanticVersion &&
                capability.inputPorts == inputPorts &&
                capability.outputPorts == outputPorts) {
                return true;
            }
        }
        return false;
    }
};

/**
 * Proof that a device service completed all mandatory initialization. Public
 * construction yields only an invalid token.
 */
class KRITAIMAGE_EXPORT KisBackendReadyToken
{
public:
    KisBackendReadyToken() = default;

    bool isValid() const
    {
        return m_serviceId != 0 && m_deviceGeneration != 0 && m_shaderAbi != 0;
    }

    quint64 serviceId() const { return m_serviceId; }
    quint64 deviceGeneration() const { return m_deviceGeneration; }
    quint64 shaderAbi() const { return m_shaderAbi; }

private:
    KisBackendReadyToken(quint64 serviceId,
                         quint64 deviceGeneration,
                         quint64 shaderAbi)
        : m_serviceId(serviceId)
        , m_deviceGeneration(deviceGeneration)
        , m_shaderAbi(shaderAbi)
    {
    }

    quint64 m_serviceId = 0;
    quint64 m_deviceGeneration = 0;
    quint64 m_shaderAbi = 0;

    friend class KisBackendServiceRegistry;
};

/**
 * Backend-neutral, document-owned acceleration session interface. It contains
 * no canvas, window, Vulkan object, or process-global active document state.
 */
class KRITAIMAGE_EXPORT KisDocumentGpuSession
{
public:
    virtual ~KisDocumentGpuSession();

    virtual quint64 documentId() const = 0;
    virtual quint64 backendServiceId() const = 0;
    virtual quint64 deviceGeneration() const = 0;
    virtual quint64 sessionGeneration() const = 0;
    virtual KisAccelerationSessionState state() const = 0;
    virtual KisAccelerationCapabilities capabilities() const = 0;
    virtual bool acceptsNewWork() const = 0;
    virtual QString failureReason() const = 0;
    virtual KisCompletionTicket beginShutdown() = 0;
};

#endif // KIS_DOCUMENT_GPU_SESSION_H
