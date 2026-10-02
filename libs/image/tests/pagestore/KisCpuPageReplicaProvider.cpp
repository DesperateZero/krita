/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisCpuPageReplicaProvider.h"
#include "KisCpuResidentBinding_p.h"

#include <QMutexLocker>
#include <QScopeGuard>
#include <cstring>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>

namespace {

quint64 alignedRowStride(const KisPageAllocationDescriptor &descriptor)
{
    const quint64 rowBytes = descriptor.minimumRowBytes();
    const quint64 alignment = qMax<quint64>(
        descriptor.rowAlignment, descriptor.format.pixelAlignment);
    if (rowBytes > std::numeric_limits<quint64>::max() - (alignment - 1)) {
        return 0;
    }
    return (rowBytes + alignment - 1) & ~(alignment - 1);
}

}

struct CpuAllocation
{
    struct AlignedBytes
    {
        ~AlignedBytes()
        {
            if (data) {
                ::operator delete(data, std::align_val_t(alignment));
            }
        }

        void *data = nullptr;
        size_t alignment = alignof(std::max_align_t);
    };

    QSharedPointer<KisCpuResidentBinding> binding;
    KisCompletionTicket retirementCompletion;
};

class CpuResidentBinding final : public KisCpuResidentBinding
{
public:
    CpuResidentBinding(const std::shared_ptr<CpuAllocation::AlignedBytes> &bytes,
                       const KisReplicaHandle &handle)
        : KisCpuResidentBinding(handle), m_bytes(bytes) {}
private:
    void *pinStorage(bool, KisCpuResidentReadStatus *status) override
    {
        if (status) *status = m_bytes ? KisCpuResidentReadStatus::Ready : KisCpuResidentReadStatus::Retired;
        return m_bytes ? m_bytes->data : nullptr;
    }
    void unpinStorage() override {}
    void releaseStorage() override { m_bytes.reset(); }
    std::shared_ptr<CpuAllocation::AlignedBytes> m_bytes;
};

class KisCpuPageReplicaProvider::Private : public KisCpuResidentAllocationIndex<CpuAllocation>
{
public:
    KisReplicaOperation allocate(KisPageOperationId operation,
                                 const KisPageVersion &version,
                                 const KisPageAllocationDescriptor &descriptor,
                                 KisPageAccessDomain domain)
    {
        if (!config.isValid()) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU provider is not configured"));
        }
        if (!version.isValid() || !descriptor.isValid() ||
            domain != KisPageAccessDomain::CpuRam ||
            !operationAvailable(operation)) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU allocation request is invalid"));
        }
        consumeOperation(operation);

        const quint64 rowStride = alignedRowStride(descriptor);
        if (rowStride == 0 ||
            rowStride > std::numeric_limits<quint32>::max() ||
            quint64(descriptor.pageExtent.height()) >
                std::numeric_limits<quint64>::max() / rowStride) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU allocation layout overflows"));
        }
        const quint64 byteSize = rowStride * quint64(descriptor.pageExtent.height());
        if (byteSize > quint64(std::numeric_limits<qsizetype>::max()) ||
            byteSize > config.budgetBytes - qMin(config.budgetBytes, committedBytes) ||
            nextSlot == 0 || nextSlot == std::numeric_limits<quint64>::max()) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU page budget or allocation identity is exhausted"));
        }

        const KisCompletionTicket completion =
            completions->allocatePending(completionSource);
        if (!completion.isValid()) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU completion allocation failed"));
        }
        const auto retirementCompletion = completions->allocatePending(completionSource);
        auto failCompletion = qScopeGuard([&] {
            completions->completePrepared(completion, KisCompletionStatus::Failed);
            if (retirementCompletion.isValid()) completions->completePrepared(retirementCompletion, KisCompletionStatus::Failed);
        });
        if (!retirementCompletion.isValid())
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU retirement completion preparation failed"));

        KisReplicaHandle handle;
        handle.provider = config.provider;
        handle.providerEpoch = config.providerEpoch;
        handle.allocation = {nextSlot++, 1};
        handle.version = version;
        handle.domain = domain;
        handle.layout = {descriptor.layoutRevision,
                         descriptor.format.formatId,
                         descriptor.pageExtent,
                         descriptor.validRect,
                         quint32(rowStride),
                         byteSize};

        CpuAllocation allocation;
        allocation.retirementCompletion = retirementCompletion;
        auto bytes = std::make_shared<CpuAllocation::AlignedBytes>();
        bytes->alignment = qMax<size_t>(
            size_t(descriptor.format.pixelAlignment), alignof(std::max_align_t));
        bytes->data = ::operator new(
            size_t(byteSize), std::align_val_t(bytes->alignment),
            std::nothrow);
        if (!bytes->data) {
            return KisReplicaOperation::failed(operation, QStringLiteral("CPU page allocation failed"));
        }
        std::memset(bytes->data, 0, size_t(byteSize));
        if (descriptor.initialization == KisPageInitialization::DefaultPixel) {
            const qsizetype pixelStride = qsizetype(descriptor.format.pixelStride);
            const qsizetype width = descriptor.pageExtent.width();
            const qsizetype height = descriptor.pageExtent.height();
            for (qsizetype y = 0; y < height; ++y) {
                char *row = static_cast<char *>(bytes->data) +
                            y * qsizetype(rowStride);
                for (qsizetype x = 0; x < width; ++x) {
                    std::memcpy(row + x * pixelStride,
                                descriptor.format.defaultPixel.constData(),
                                size_t(pixelStride));
                }
            }
        }

        allocation.binding = QSharedPointer<CpuResidentBinding>::create(bytes, handle);
        allocations.emplace(handle.allocation.slot, allocation);
        committedBytes += byteSize;
        completions->completePrepared(completion, KisCompletionStatus::Succeeded);

        failCompletion.dismiss();
        KisReplicaOperation result;
        result.status = KisPageRequestStatus::Ready;
        result.operation = operation;
        result.replica = handle;
        result.completion = completion;
        return result;
    }

};

KisCpuPageReplicaProvider::KisCpuPageReplicaProvider()
    : d(new Private)
{
}

KisCpuPageReplicaProvider::~KisCpuPageReplicaProvider()
{
    d->revokeBindings();
}

bool KisCpuPageReplicaProvider::configure(
    const KisCpuResidentReplicaProviderConfig &config,
    const QSharedPointer<KisCompletionRegistry> &completions,
    QString *error)
{
    return d->configure(config, completions, QStringLiteral("CPU"), error);
}

QString KisCpuPageReplicaProvider::name() const
{
    return QStringLiteral("Krita CPU RAM page replica provider");
}

KisReplicaProviderId KisCpuPageReplicaProvider::providerId() const
{
    return d->providerId();
}

KisReplicaProviderEpoch KisCpuPageReplicaProvider::providerEpoch() const
{
    return d->providerEpoch();
}

KisReplicaCapabilities KisCpuPageReplicaProvider::capabilities() const
{
    static const KisReplicaCapabilities result{{KisPageAccessDomain::CpuRam},
        {{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer}}, true};
    return result;
}

KisReplicaOperation KisCpuPageReplicaProvider::requestReplica(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageAccessMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    return d->allocate(operation, version, descriptor, domain);
}

KisReplicaOperation KisCpuPageReplicaProvider::prepareWrite(
    KisPageOperationId operation,
    const KisPageVersion &version,
    const KisPageAllocationDescriptor &descriptor,
    KisPageAccessDomain domain,
    KisPageWriteMode mode,
    KisPagePriority priority)
{
    Q_UNUSED(mode);
    return requestReplica(operation, version, descriptor, domain,
                          KisPageAccessMode::Write, priority);
}

KisReplicaOperation KisCpuPageReplicaProvider::transfer(
    const KisReplicaTransferRequest &request,
    KisPagePriority priority)
{
    Q_UNUSED(priority);
    QMutexLocker locker(&d->mutex);
    return d->transfer(request, QStringLiteral("CPU"));
}

KisReplicaAccess KisCpuPageReplicaProvider::resolveAccess(
    KisPageLeaseId lease,
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    KisPageAccessRequirement requirement,
    KisPageAccessMode mode)
{
    return d->resolveAccess(lease, operation, replica, requirement, mode);
}

void KisCpuPageReplicaProvider::releaseAccess(
    KisReplicaAccess access,
    const KisCompletionTicket &lastUse)
{
    Q_UNUSED(lastUse);
    d->releaseAccess(access);
}

bool KisCpuPageReplicaProvider::validate(
    const KisReplicaHandle &replica,
    const KisPageAllocationDescriptor &descriptor) const
{
    if (beforeValidate) beforeValidate();
    QMutexLocker locker(&d->mutex);
    const auto allocationIt = d->findExactAllocation(replica);
    return descriptor.isValid() &&
           replica.layout.matches(descriptor) &&
           allocationIt != d->allocations.end();
}

KisReplicaOperation KisCpuPageReplicaProvider::retire(
    KisPageOperationId operation,
    const KisReplicaHandle &replica,
    const KisCompletionTicket &lastUse)
{
    QMutexLocker locker(&d->mutex);
    auto retirement = d->beginRetirement(operation, replica, lastUse);
    if (retirement.failure)
        return KisReplicaOperation::failed(operation,
            QStringLiteral("CPU retirement %1").arg(QString::fromLatin1(retirement.failure)));
    if (!retirement.allocation->second.binding->retire(replica.allocationIdentity())) {
        return KisReplicaOperation::failed(operation, QStringLiteral("CPU allocation is still pinned"));
    }
    d->consumeOperation(operation);
    d->committedBytes -= replica.layout.byteSize;
    d->allocations.erase(retirement.allocation);
    d->completions->completePrepared(retirement.completion, KisCompletionStatus::Succeeded);
    return {KisPageRequestStatus::Ready, operation, replica, retirement.completion, {}};
}

KisReplicaMemoryUsage KisCpuPageReplicaProvider::memoryUsage() const
{
    QMutexLocker locker(&d->mutex);
    return {d->committedBytes, d->committedBytes, d->config.budgetBytes};
}

QSharedPointer<KisCpuResidentBinding> KisCpuPageReplicaProvider::cpuResidentBinding(
    const KisReplicaHandle &replica, KisCpuResidentReadStatus *status) const
{
    return d->binding(replica, status);
}
