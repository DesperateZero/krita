/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_VULKAN_SHADER_CATALOG_H
#define KIS_VULKAN_SHADER_CATALOG_H

#include <QByteArray>
#include <QScopedPointer>
#include <QString>
#include <QStringList>
#include <QVector>

enum class KisVulkanShaderStage : quint8 {
    Compute,
    Vertex,
    Fragment
};

struct KisVulkanShaderAsset
{
    QString logicalId;
    KisVulkanShaderStage stage = KisVulkanShaderStage::Compute;
    QByteArray entryPoint = QByteArrayLiteral("main");
    quint64 semanticVersion = 0;
    quint64 passAbiVersion = 0;
    QByteArray sourceDependencyHash;
    QByteArray spirvHash;
    QByteArray reflectionDigest;
    QByteArray spirv;
    QStringList requiredFeatures;
    bool requiredForFoundation = false;

    QString validationError() const;
    bool isValid() const { return validationError().isEmpty(); }
};

enum class KisVulkanShaderCatalogState : quint8 {
    Open,
    Sealed,
    Failed
};

/**
 * Immutable-after-seal shader manifest. It performs identity, hash, ABI and
 * required-asset validation before any pipeline repository may consume it.
 */
class KisVulkanShaderCatalog
{
public:
    KisVulkanShaderCatalog();
    ~KisVulkanShaderCatalog();

    KisVulkanShaderCatalogState state() const;
    bool registerAsset(const KisVulkanShaderAsset &asset, QString *error = nullptr);
    bool seal(quint64 requiredFoundationAbi, QString *error = nullptr);

    bool contains(const QString &logicalId) const;
    KisVulkanShaderAsset asset(const QString &logicalId) const;
    QStringList assetIds() const;
    QByteArray manifestDigest() const;
    quint64 foundationAbi() const;
    QString failureReason() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_VULKAN_SHADER_CATALOG_H
