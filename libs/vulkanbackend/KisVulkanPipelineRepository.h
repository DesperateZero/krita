/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_PIPELINE_REPOSITORY_H
#define KIS_VULKAN_PIPELINE_REPOSITORY_H

#include <QByteArray>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QString>
#include <QStringList>

struct KisVulkanPipelineKey
{
    QString shaderId;
    quint64 shaderSemanticVersion = 0;
    quint64 passAbiVersion = 0;
    quint64 surfaceFormatId = 0;
    quint32 alphaSemantic = 0;
    QByteArray workingColorClass;
    QByteArray programId;
    QByteArray deviceFeatureClass;
    QByteArray specializationDigest;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
    QByteArray stableKey() const;
};

enum class KisVulkanPipelineState : quint8 {
    Declared,
    Preparing,
    Prepared,
    Failed,
    Retired
};

struct KisVulkanPipelineHandle
{
    quint64 repositoryGeneration = 0;
    quint64 pipelineId = 0;

    bool isValid() const
    {
        return repositoryGeneration != 0 && pipelineId != 0;
    }
};

/**
 * Native-handle-free pipeline identity and state repository. Actual VkPipeline
 * objects remain private to the future factory implementation.
 */
class KisVulkanPipelineRepository
{
public:
    KisVulkanPipelineRepository();
    ~KisVulkanPipelineRepository();

    bool configure(quint64 repositoryGeneration,
                   quint64 deviceGeneration,
                   const QByteArray &shaderManifestDigest);
    KisVulkanPipelineHandle declarePipeline(const KisVulkanPipelineKey &key,
                                             QString *error = nullptr);
    bool beginPreparation(const KisVulkanPipelineHandle &handle);
    bool markPrepared(const KisVulkanPipelineHandle &handle);
    bool markFailed(const KisVulkanPipelineHandle &handle, const QString &reason);
    bool retire(const KisVulkanPipelineHandle &handle);

    KisVulkanPipelineState state(const KisVulkanPipelineHandle &handle) const;
    QString failureReason(const KisVulkanPipelineHandle &handle) const;
    bool isPrepared(const KisVulkanPipelineKey &key) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

inline bool kisConfigureVulkanPipelineExecutor(
    quint64 deviceGeneration,
    const QSharedPointer<KisVulkanPipelineRepository> &pipelines,
    quint64 &configuredGeneration,
    QSharedPointer<KisVulkanPipelineRepository> &configuredPipelines)
{
    if (configuredGeneration != 0 || deviceGeneration == 0 || !pipelines) return false;
    configuredGeneration = deviceGeneration;
    configuredPipelines = pipelines;
    return true;
}

#endif // KIS_VULKAN_PIPELINE_REPOSITORY_H
