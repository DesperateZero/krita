/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanPipelineRepository.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

QString KisVulkanPipelineKey::validationError() const
{
    if (shaderId.isEmpty() || shaderSemanticVersion == 0 || passAbiVersion == 0) {
        return QStringLiteral("Pipeline shader identity or ABI is incomplete");
    }
    if (surfaceFormatId == 0 || alphaSemantic == 0) {
        return QStringLiteral("Pipeline format or alpha semantic is unspecified");
    }
    if (workingColorClass.isEmpty() || programId.isEmpty() ||
        deviceFeatureClass.isEmpty() || specializationDigest.isEmpty()) {
        return QStringLiteral("Pipeline program/color/device specialization is incomplete");
    }
    return {};
}

QByteArray KisVulkanPipelineKey::stableKey() const
{
    if (!isValid()) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(shaderId.toUtf8());
    hash.addData(QByteArray::number(shaderSemanticVersion));
    hash.addData(QByteArray::number(passAbiVersion));
    hash.addData(QByteArray::number(surfaceFormatId));
    hash.addData(QByteArray::number(alphaSemantic));
    hash.addData(workingColorClass);
    hash.addData(programId);
    hash.addData(deviceFeatureClass);
    hash.addData(specializationDigest);
    return hash.result();
}

struct PipelineRecord
{
    KisVulkanPipelineKey key;
    KisVulkanPipelineState state = KisVulkanPipelineState::Declared;
    QString failureReason;
};

class KisVulkanPipelineRepository::Private
{
public:
    mutable QMutex mutex;
    quint64 generation = 0;
    quint64 deviceGeneration = 0;
    quint64 nextPipeline = 1;
    QByteArray shaderManifestDigest;
    QHash<quint64, PipelineRecord> records;
    QHash<QByteArray, quint64> keyToPipeline;
};

KisVulkanPipelineRepository::KisVulkanPipelineRepository()
    : d(new Private)
{
}

KisVulkanPipelineRepository::~KisVulkanPipelineRepository() = default;

bool KisVulkanPipelineRepository::configure(quint64 repositoryGeneration,
                                            quint64 deviceGeneration,
                                            const QByteArray &shaderManifestDigest)
{
    if (repositoryGeneration == 0 || deviceGeneration == 0 || shaderManifestDigest.isEmpty()) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    if (d->generation != 0 || !d->records.isEmpty()) {
        return false;
    }
    d->generation = repositoryGeneration;
    d->deviceGeneration = deviceGeneration;
    d->shaderManifestDigest = shaderManifestDigest;
    return true;
}

KisVulkanPipelineHandle KisVulkanPipelineRepository::declarePipeline(
    const KisVulkanPipelineKey &key,
    QString *error)
{
    const QByteArray stableKey = key.stableKey();
    QMutexLocker locker(&d->mutex);
    if (d->generation == 0) {
        if (error) *error = QStringLiteral("Pipeline repository is not configured");
        return {};
    }
    if (stableKey.isEmpty()) {
        if (error) *error = key.validationError();
        return {};
    }
    if (d->keyToPipeline.contains(stableKey)) {
        return {d->generation, d->keyToPipeline.value(stableKey)};
    }

    const quint64 pipelineId = d->nextPipeline++;
    PipelineRecord record;
    record.key = key;
    d->records.insert(pipelineId, record);
    d->keyToPipeline.insert(stableKey, pipelineId);
    return {d->generation, pipelineId};
}

bool KisVulkanPipelineRepository::beginPreparation(const KisVulkanPipelineHandle &handle)
{
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) return false;
    auto it = d->records.find(handle.pipelineId);
    if (it == d->records.end() || it->state != KisVulkanPipelineState::Declared) return false;
    it->state = KisVulkanPipelineState::Preparing;
    return true;
}

bool KisVulkanPipelineRepository::markPrepared(const KisVulkanPipelineHandle &handle)
{
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) return false;
    auto it = d->records.find(handle.pipelineId);
    if (it == d->records.end() || it->state != KisVulkanPipelineState::Preparing) return false;
    it->state = KisVulkanPipelineState::Prepared;
    return true;
}

bool KisVulkanPipelineRepository::markFailed(const KisVulkanPipelineHandle &handle,
                                             const QString &reason)
{
    if (reason.isEmpty()) return false;
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) return false;
    auto it = d->records.find(handle.pipelineId);
    if (it == d->records.end() || it->state == KisVulkanPipelineState::Retired) return false;
    it->state = KisVulkanPipelineState::Failed;
    it->failureReason = reason;
    return true;
}

bool KisVulkanPipelineRepository::retire(const KisVulkanPipelineHandle &handle)
{
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) return false;
    auto it = d->records.find(handle.pipelineId);
    if (it == d->records.end() || it->state == KisVulkanPipelineState::Retired) return false;
    d->keyToPipeline.remove(it->key.stableKey());
    it->state = KisVulkanPipelineState::Retired;
    return true;
}

KisVulkanPipelineState KisVulkanPipelineRepository::state(
    const KisVulkanPipelineHandle &handle) const
{
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) {
        return KisVulkanPipelineState::Failed;
    }
    const auto it = d->records.constFind(handle.pipelineId);
    return it == d->records.constEnd()
        ? KisVulkanPipelineState::Failed
        : it->state;
}

QString KisVulkanPipelineRepository::failureReason(
    const KisVulkanPipelineHandle &handle) const
{
    QMutexLocker locker(&d->mutex);
    if (!handle.isValid() || handle.repositoryGeneration != d->generation) {
        return QStringLiteral("Pipeline handle is invalid or stale");
    }
    const auto it = d->records.constFind(handle.pipelineId);
    return it == d->records.constEnd()
        ? QStringLiteral("Pipeline handle is unknown")
        : it->failureReason;
}

bool KisVulkanPipelineRepository::isPrepared(const KisVulkanPipelineKey &key) const
{
    const QByteArray stableKey = key.stableKey();
    if (stableKey.isEmpty()) return false;
    QMutexLocker locker(&d->mutex);
    const quint64 pipelineId = d->keyToPipeline.value(stableKey, 0);
    return pipelineId != 0 &&
           d->records.value(pipelineId).state == KisVulkanPipelineState::Prepared;
}
