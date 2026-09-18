/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisVulkanShaderCatalog.h"

#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

#include <algorithm>

QString KisVulkanShaderAsset::validationError() const
{
    if (logicalId.isEmpty() || entryPoint.isEmpty()) {
        return QStringLiteral("Shader asset identity or entry point is empty");
    }
    if (semanticVersion == 0 || passAbiVersion == 0) {
        return QStringLiteral("Shader semantic or pass ABI version is zero");
    }
    if (sourceDependencyHash.isEmpty() || spirvHash.isEmpty() || reflectionDigest.isEmpty()) {
        return QStringLiteral("Shader hashes or reflection digest are incomplete");
    }
    if (spirv.size() < 4 || spirv.size() % 4 != 0) {
        return QStringLiteral("Shader SPIR-V payload is empty or not word aligned");
    }
    const QByteArray spirvMagic("\x03\x02\x23\x07", 4);
    if (spirv.left(4) != spirvMagic) {
        return QStringLiteral("Shader payload does not start with the SPIR-V magic word");
    }
    const QByteArray calculated = QCryptographicHash::hash(spirv, QCryptographicHash::Sha256);
    if (calculated != spirvHash) {
        return QStringLiteral("Shader SPIR-V hash does not match its payload");
    }
    return {};
}

class KisVulkanShaderCatalog::Private
{
public:
    mutable QMutex mutex;
    KisVulkanShaderCatalogState state = KisVulkanShaderCatalogState::Open;
    QHash<QString, KisVulkanShaderAsset> assets;
    QByteArray manifestDigest;
    quint64 foundationAbi = 0;
    QString failureReason;
};

KisVulkanShaderCatalog::KisVulkanShaderCatalog()
    : d(new Private)
{
}

KisVulkanShaderCatalog::~KisVulkanShaderCatalog() = default;

KisVulkanShaderCatalogState KisVulkanShaderCatalog::state() const
{
    QMutexLocker locker(&d->mutex);
    return d->state;
}

bool KisVulkanShaderCatalog::registerAsset(const KisVulkanShaderAsset &asset,
                                           QString *error)
{
    const QString validationError = asset.validationError();
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanShaderCatalogState::Open) {
        if (error) *error = QStringLiteral("Shader catalog is no longer open");
        return false;
    }
    if (!validationError.isEmpty()) {
        if (error) *error = validationError;
        return false;
    }
    if (d->assets.contains(asset.logicalId)) {
        if (error) *error = QStringLiteral("Duplicate shader logical id: %1").arg(asset.logicalId);
        return false;
    }
    d->assets.insert(asset.logicalId, asset);
    return true;
}

bool KisVulkanShaderCatalog::seal(quint64 requiredFoundationAbi, QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (d->state != KisVulkanShaderCatalogState::Open || requiredFoundationAbi == 0) {
        if (error) *error = QStringLiteral("Shader catalog cannot be sealed in its current state");
        return false;
    }

    QStringList requiredIds;
    for (auto it = d->assets.constBegin(); it != d->assets.constEnd(); ++it) {
        if (it->requiredForFoundation) {
            requiredIds.append(it.key());
            if (it->passAbiVersion != requiredFoundationAbi) {
                d->state = KisVulkanShaderCatalogState::Failed;
                d->failureReason = QStringLiteral("Foundation shader ABI mismatch: %1").arg(it.key());
                if (error) *error = d->failureReason;
                return false;
            }
        }
    }
    if (requiredIds.isEmpty()) {
        d->state = KisVulkanShaderCatalogState::Failed;
        d->failureReason = QStringLiteral("Shader catalog contains no required foundation assets");
        if (error) *error = d->failureReason;
        return false;
    }

    std::sort(requiredIds.begin(), requiredIds.end());
    QCryptographicHash manifestHash(QCryptographicHash::Sha256);
    QStringList allIds = d->assets.keys();
    std::sort(allIds.begin(), allIds.end());
    for (const QString &id : allIds) {
        const KisVulkanShaderAsset &asset = d->assets[id];
        manifestHash.addData(id.toUtf8());
        manifestHash.addData(asset.sourceDependencyHash);
        manifestHash.addData(asset.spirvHash);
        manifestHash.addData(asset.reflectionDigest);
        manifestHash.addData(QByteArray::number(asset.semanticVersion));
        manifestHash.addData(QByteArray::number(asset.passAbiVersion));
    }

    d->foundationAbi = requiredFoundationAbi;
    d->manifestDigest = manifestHash.result();
    d->state = KisVulkanShaderCatalogState::Sealed;
    return true;
}

bool KisVulkanShaderCatalog::contains(const QString &logicalId) const
{
    QMutexLocker locker(&d->mutex);
    return d->assets.contains(logicalId);
}

KisVulkanShaderAsset KisVulkanShaderCatalog::asset(const QString &logicalId) const
{
    QMutexLocker locker(&d->mutex);
    return d->assets.value(logicalId);
}

QStringList KisVulkanShaderCatalog::assetIds() const
{
    QMutexLocker locker(&d->mutex);
    QStringList ids = d->assets.keys();
    std::sort(ids.begin(), ids.end());
    return ids;
}

QByteArray KisVulkanShaderCatalog::manifestDigest() const
{
    QMutexLocker locker(&d->mutex);
    return d->manifestDigest;
}

quint64 KisVulkanShaderCatalog::foundationAbi() const
{
    QMutexLocker locker(&d->mutex);
    return d->foundationAbi;
}

QString KisVulkanShaderCatalog::failureReason() const
{
    QMutexLocker locker(&d->mutex);
    return d->failureReason;
}
