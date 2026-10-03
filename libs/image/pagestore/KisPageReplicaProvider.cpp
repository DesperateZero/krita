/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageReplicaProvider.h"

bool KisCpuPagePayload::isValidFor(const KisPageAllocationDescriptor &descriptor) const
{
    if (!data || !descriptor.isValid() || rowStride <= 0 || byteSize <= 0) return false;
    const quint64 rowBytes = descriptor.minimumRowBytes();
    const quint64 precedingRows = quint64(descriptor.pageExtent.height() - 1);
    const quint64 maximum = quint64(std::numeric_limits<qsizetype>::max());
    if (quint64(rowStride) < rowBytes || rowBytes > maximum ||
        (precedingRows && quint64(rowStride) > (maximum - rowBytes) / precedingRows)) return false;
    const quint64 required = precedingRows * quint64(rowStride) + rowBytes;
    return quint64(byteSize) >= required &&
        quintptr(data) <= std::numeric_limits<quintptr>::max() - (required - 1);
}

KisReplicaOperation KisPageReplicaProvider::prepareSynchronousCpuPayload(
    KisPageOperationId operation, const KisPageVersion &, const KisPageAllocationDescriptor &,
    const KisCpuPagePayload &, KisPagePriority)
{
    return {KisPageRequestStatus::Unsupported, operation, {}, {},
            QStringLiteral("synchronous CPU payload preparation is unsupported")};
}

std::shared_ptr<KisCpuResidentBinding> KisPageReplicaProvider::cpuResidentBinding(
    const KisReplicaHandle &, KisCpuResidentReadStatus *status) const
{
    if (status) *status = KisCpuResidentReadStatus::UnsupportedProvider;
    return {};
}

KisPageReplicaProvider::~KisPageReplicaProvider() = default;
KisPageReplicaSource::~KisPageReplicaSource() = default;
KisReplicaBackingFootprint KisPageReplicaProvider::backingFootprint(
    const KisReplicaHandle &replica) const
{
    return replica.isValid()
        ? KisReplicaBackingFootprint{replica.allocation.slot, replica.domain,
                                     replica.layout.byteSize, 1}
        : KisReplicaBackingFootprint{};
}

KisReplicaBackingDomainChanges KisPageReplicaProvider::backingDomainChanges(KisBackingBudgetController *budget) const
{
    return KisReplicaBackingDomainChanges(KisMutationStorageAllocator<KisReplicaBackingDomainChange>(budget));
}

bool KisPageReplicaProvider::mayHaveBackingDomainChanges() const noexcept
{
    return true;
}

void KisPageReplicaProvider::acknowledgeBackingDomainChange(quint64, quint64)
{
}
bool KisPageReplicaProvider::registerBackingDomainAdmission(
    const std::shared_ptr<KisReplicaBackingDomainAdmission> &admission,
    QString *error)
{
    const bool valid = bool(admission);
    KisPageStoreDetail::setError(
        error, valid ? QString{} : QStringLiteral("backing-domain admission is invalid"));
    return valid;
}
bool KisPageReplicaProvider::copySynchronousSourceToCpu(
    const std::shared_ptr<const KisPageReplicaSource> &, const KisPageAllocationDescriptor &,
    void *, quint32, quint64)
{
    return false;
}
KisReplicaOperation KisPageReplicaProvider::prepareSynchronousSource(
    KisPageOperationId operation, const std::shared_ptr<const KisPageReplicaSource> &,
    const KisPageVersion &, const KisPageAllocationDescriptor &,
    KisReplicaSourceUse, KisPagePriority)
{
    return {KisPageRequestStatus::Unsupported, operation, {}, {},
            QStringLiteral("immutable source preparation is unsupported")};
}
KisReplicaOperation KisPageReplicaProvider::prepareSynchronousWriteCopy(
    KisPageOperationId operation,
    const KisReplicaHandle &source,
    const KisPageVersion &targetVersion,
    const KisPageAllocationDescriptor &descriptor,
    KisPagePriority priority)
{
    Q_UNUSED(source);
    Q_UNUSED(targetVersion);
    Q_UNUSED(descriptor);
    Q_UNUSED(priority);
    return {KisPageRequestStatus::Unsupported, operation, {}, {},
            QStringLiteral("native write copy is unsupported")};
}
KisPageReplicaTransferBridge::~KisPageReplicaTransferBridge() = default;
KisExactGenerationArchive::~KisExactGenerationArchive() = default;
