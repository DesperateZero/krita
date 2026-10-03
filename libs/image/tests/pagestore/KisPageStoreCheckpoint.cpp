/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStoreCheckpoint.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

#include <cstring>

namespace {

constexpr quint32 checkpointSchemaVersion = 2;
const QByteArray checkpointMagic("KRITA-PAGESTORE-BR1");

void writePageKey(QDataStream &stream, const KisPageKey &key)
{
    stream << key.surface.value << key.page.column << key.page.row;
}

void readPageKey(QDataStream &stream, KisPageKey *key)
{
    stream >> key->surface.value >> key->page.column >> key->page.row;
}

void writePageVersion(QDataStream &stream, const KisPageVersion &version)
{
    writePageKey(stream, version.key);
    stream << version.generation.value << version.defaultPixelRevision;
}

void readPageVersion(QDataStream &stream, KisPageVersion *version)
{
    readPageKey(stream, &version->key);
    stream >> version->generation.value >> version->defaultPixelRevision;
}

void writeSurfaceFormat(QDataStream &stream, const KisSurfaceFormat &format)
{
    stream << format.formatId << format.colorModelId << format.colorDepthId
           << format.profileFingerprint << format.channelOrder
           << format.packing << format.defaultPixel << format.channelCount
           << format.pixelStride << format.pixelAlignment
           << quint8(format.hasAlpha ? 1 : 0)
           << quint8(format.alphaSemantic) << quint8(format.endianness)
           << format.codecVersion;
}

void readSurfaceFormat(QDataStream &stream, KisSurfaceFormat *format)
{
    quint8 hasAlpha = 0;
    quint8 alphaSemantic = 0;
    quint8 endianness = 0;
    stream >> format->formatId >> format->colorModelId >> format->colorDepthId
           >> format->profileFingerprint >> format->channelOrder
           >> format->packing >> format->defaultPixel >> format->channelCount
           >> format->pixelStride >> format->pixelAlignment >> hasAlpha
           >> alphaSemantic >> endianness >> format->codecVersion;
    format->hasAlpha = hasAlpha != 0;
    format->alphaSemantic = KisSurfaceAlphaSemantic(alphaSemantic);
    format->endianness = KisSurfaceEndianness(endianness);
}

void writeDescriptor(QDataStream &stream,
                     const KisPageAllocationDescriptor &descriptor)
{
    stream << descriptor.layoutRevision;
    writeSurfaceFormat(stream, descriptor.format);
    stream << descriptor.pageExtent << descriptor.validRect
           << descriptor.rowAlignment << quint8(descriptor.initialization);
}

void readDescriptor(QDataStream &stream,
                    KisPageAllocationDescriptor *descriptor)
{
    quint8 initialization = 0;
    stream >> descriptor->layoutRevision;
    readSurfaceFormat(stream, &descriptor->format);
    stream >> descriptor->pageExtent >> descriptor->validRect
           >> descriptor->rowAlignment >> initialization;
    descriptor->initialization = KisPageInitialization(initialization);
}

void writeSnapshot(QDataStream &stream,
                   const KisImageEpochSnapshot &snapshot)
{
    stream << snapshot.epoch.value << snapshot.graphRevision
           << snapshot.defaultPixelRevision << snapshot.extentRevision
           << snapshot.propertyRevision << quint64(snapshot.manifest.size());
    for (const KisPageVersion &version : snapshot.manifest) {
        writePageVersion(stream, version);
    }
    stream << quint64(snapshot.surfaces.size());
    for (const KisSurfaceEpochState &surface : snapshot.surfaces) {
        stream << surface.surface.value;
        writeSurfaceFormat(stream, surface.format);
        stream << surface.contentExtent << surface.logicalPageExtent
               << surface.layoutRevision << surface.rowAlignment
               << surface.defaultPixelRevision << surface.extentRevision;
    }
}

bool readSnapshot(QDataStream &stream,
                  KisImageEpochSnapshot *snapshot)
{
    quint64 manifestSize = 0;
    quint64 surfaceSize = 0;
    stream >> snapshot->epoch.value >> snapshot->graphRevision
           >> snapshot->defaultPixelRevision >> snapshot->extentRevision
           >> snapshot->propertyRevision >> manifestSize;
    if (manifestSize > 100000000) return false;
    snapshot->manifest.resize(qsizetype(manifestSize));
    for (KisPageVersion &version : snapshot->manifest) {
        readPageVersion(stream, &version);
    }
    stream >> surfaceSize;
    if (surfaceSize > 1000000) return false;
    snapshot->surfaces.resize(qsizetype(surfaceSize));
    for (KisSurfaceEpochState &surface : snapshot->surfaces) {
        stream >> surface.surface.value;
        readSurfaceFormat(stream, &surface.format);
        stream >> surface.contentExtent >> surface.logicalPageExtent
               >> surface.layoutRevision >> surface.rowAlignment
               >> surface.defaultPixelRevision >> surface.extentRevision;
    }
    return stream.status() == QDataStream::Ok;
}

const KisSurfaceEpochState *findSurface(
    const KisImageEpochSnapshot &snapshot,
    KisSurfaceId id)
{
    for (const KisSurfaceEpochState &surface : snapshot.surfaces) {
        if (surface.surface == id) return &surface;
    }
    return nullptr;
}

bool sameSurfaceStorageContract(const KisPageAllocationDescriptor &descriptor,
                                const KisSurfaceEpochState &surface)
{
    KisSurfaceFormat pageFormat = descriptor.format;
    KisSurfaceFormat surfaceFormat = surface.format;
    pageFormat.defaultPixel.clear();
    surfaceFormat.defaultPixel.clear();
    return pageFormat == surfaceFormat &&
           descriptor.layoutRevision == surface.layoutRevision &&
           descriptor.pageExtent == surface.logicalPageExtent &&
           descriptor.rowAlignment == surface.rowAlignment;
}

}

bool KisPageStoreCheckpointPage::isValid() const
{
    return version.isValid() && descriptor.isValid() &&
           canonicalBytes.size() == qsizetype(descriptor.minimumByteSize()) &&
           checksum.size() == QCryptographicHash::hashLength(
                                  QCryptographicHash::Sha256) &&
           checksum == QCryptographicHash::hash(
                           canonicalBytes, QCryptographicHash::Sha256);
}

bool KisPageStoreCheckpoint::isValid() const
{
    if (schemaVersion != checkpointSchemaVersion || !snapshot.isValid() ||
        pages.size() != snapshot.manifest.size()) {
        return false;
    }
    for (qsizetype i = 0; i < pages.size(); ++i) {
        const KisPageStoreCheckpointPage &page = pages.at(i);
        if (!page.isValid()) return false;
        const KisSurfaceEpochState *surface =
            findSurface(snapshot, page.version.key.surface);
        if (!surface || !sameSurfaceStorageContract(page.descriptor, *surface)) {
            return false;
        }
        bool found = false;
        for (const KisPageVersion &manifestVersion : snapshot.manifest) {
            if (manifestVersion == page.version) {
                found = true;
                break;
            }
        }
        if (!found) return false;
        for (qsizetype j = 0; j < i; ++j) {
            if (pages.at(j).version.key == page.version.key) return false;
        }
    }
    return true;
}

bool KisPageStoreCheckpointCodec::capture(
    KisPageStore *store,
    KisPageStoreCheckpoint *checkpoint,
    QString *error)
{
    if (!store || !checkpoint || !store->isOperational()) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint capture source is invalid"));
        return false;
    }
    const KisRetainedImageEpochSnapshot retained = store->captureRetainedEpoch();
    if (!retained.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("committed epoch retention failed"));
        return false;
    }

    KisPageStoreCheckpoint result;
    result.snapshot = retained.snapshot;
    bool succeeded = true;
    QString failure;
    for (const KisPageVersion &version : retained.snapshot.manifest) {
        KisPageStoreCheckpointPage page;
        page.version = version;
        if (!store->pageDescriptor(version, &page.descriptor)) {
            succeeded = false;
            failure = QStringLiteral("checkpoint page descriptor is unavailable");
            break;
        }
        KisPageReadView exact;
        exact.kind = KisPageReadViewKind::ExactVersion;
        exact.epoch = retained.snapshot.epoch;
        exact.retention = retained.token;
        exact.exactVersion = version;
        const KisReadRequest request = store->acquireRead(
            version.key, exact,
            {KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer},
            KisPagePriority::Normal);
        if (!request.isValid()) {
            succeeded = false;
            failure = request.error;
            break;
        }
        KisReadLease lease = store->resolve(request, request.readiness);
        if (!lease.isValid()) {
            store->cancel(request);
            succeeded = false;
            failure = QStringLiteral("checkpoint page read did not resolve");
            break;
        }
        const quint64 rowBytes = page.descriptor.minimumRowBytes();
        const int height = page.descriptor.pageExtent.height();
        page.canonicalBytes = QByteArray(
            qsizetype(page.descriptor.minimumByteSize()), char(0));
        if (lease.rowStride() < rowBytes ||
            lease.byteSize() < quint64(lease.rowStride()) * quint64(height)) {
            store->release(std::move(lease));
            succeeded = false;
            failure = QStringLiteral("checkpoint source layout is truncated");
            break;
        }
        for (int row = 0; row < height; ++row) {
            std::memcpy(page.canonicalBytes.data() + quint64(row) * rowBytes,
                        static_cast<const quint8 *>(lease.cpuData()) +
                            quint64(row) * lease.rowStride(),
                        size_t(rowBytes));
        }
        store->release(std::move(lease));
        page.checksum = QCryptographicHash::hash(
            page.canonicalBytes, QCryptographicHash::Sha256);
        result.pages.append(page);
    }
    if (!store->releaseSnapshot(retained.token) && succeeded) {
        succeeded = false;
        failure = QStringLiteral("checkpoint retention release failed");
    }
    if (!succeeded || !result.isValid()) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("captured checkpoint failed validation") : failure);
        return false;
    }
    *checkpoint = result;
    KisPageStoreDetail::setError(error, {});
    return true;
}

QByteArray KisPageStoreCheckpointCodec::encode(
    const KisPageStoreCheckpoint &checkpoint,
    QString *error)
{
    if (!checkpoint.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint is invalid"));
        return {};
    }
    QByteArray payload;
    QDataStream payloadStream(&payload, QIODevice::WriteOnly);
    payloadStream.setVersion(QDataStream::Qt_6_0);
    payloadStream.setByteOrder(QDataStream::LittleEndian);
    writeSnapshot(payloadStream, checkpoint.snapshot);
    payloadStream << quint64(checkpoint.pages.size());
    for (const KisPageStoreCheckpointPage &page : checkpoint.pages) {
        writePageVersion(payloadStream, page.version);
        writeDescriptor(payloadStream, page.descriptor);
        payloadStream << page.canonicalBytes << page.checksum;
    }
    if (payloadStream.status() != QDataStream::Ok) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint payload encoding failed"));
        return {};
    }

    QByteArray encoded;
    QDataStream stream(&encoded, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << checkpointMagic << checkpointSchemaVersion << payload
           << QCryptographicHash::hash(payload, QCryptographicHash::Sha256);
    if (stream.status() != QDataStream::Ok) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint envelope encoding failed"));
        return {};
    }
    KisPageStoreDetail::setError(error, {});
    return encoded;
}

bool KisPageStoreCheckpointCodec::decode(
    const QByteArray &encoded,
    KisPageStoreCheckpoint *checkpoint,
    QString *error)
{
    if (!checkpoint || encoded.isEmpty() || encoded.size() > 1024 * 1024 * 1024) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint envelope is invalid"));
        return false;
    }
    QByteArray magic;
    QByteArray payload;
    QByteArray checksum;
    quint32 schemaVersion = 0;
    QDataStream stream(encoded);
    stream.setVersion(QDataStream::Qt_6_0);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream >> magic >> schemaVersion >> payload >> checksum;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() ||
        magic != checkpointMagic || schemaVersion != checkpointSchemaVersion ||
        checksum != QCryptographicHash::hash(payload,
                                              QCryptographicHash::Sha256)) {
        KisPageStoreDetail::setError(error, QStringLiteral(
            "checkpoint envelope identity or checksum is invalid"));
        return false;
    }

    KisPageStoreCheckpoint result;
    result.schemaVersion = schemaVersion;
    quint64 pageCount = 0;
    QDataStream payloadStream(payload);
    payloadStream.setVersion(QDataStream::Qt_6_0);
    payloadStream.setByteOrder(QDataStream::LittleEndian);
    if (!readSnapshot(payloadStream, &result.snapshot)) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint snapshot decoding failed"));
        return false;
    }
    payloadStream >> pageCount;
    if (pageCount > 100000000) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint page count is invalid"));
        return false;
    }
    result.pages.resize(qsizetype(pageCount));
    for (KisPageStoreCheckpointPage &page : result.pages) {
        readPageVersion(payloadStream, &page.version);
        readDescriptor(payloadStream, &page.descriptor);
        payloadStream >> page.canonicalBytes >> page.checksum;
    }
    if (payloadStream.status() != QDataStream::Ok || !payloadStream.atEnd() ||
        !result.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint payload is invalid"));
        return false;
    }
    *checkpoint = result;
    KisPageStoreDetail::setError(error, {});
    return true;
}

bool KisPageStoreCheckpointCodec::restore(
    const KisPageStoreCheckpoint &checkpoint,
    KisPageStore *store,
    const std::shared_ptr<KisPageReplicaProvider> &cpuProvider,
    const std::shared_ptr<KisCompletionRegistry> &completions,
    qsizetype metadataShardCount,
    QString *error)
{
    if (!checkpoint.isValid() || !store || !cpuProvider || !completions) {
        KisPageStoreDetail::setError(error, QStringLiteral("checkpoint restore input is invalid"));
        return false;
    }
    QString failure;
    if (!store->configure(checkpoint.snapshot, completions,
                          metadataShardCount, &failure) ||
        !store->registerReplicaProvider(cpuProvider)) {
        KisPageStoreDetail::setError(error, failure.isEmpty()
            ? QStringLiteral("checkpoint restore configuration failed") : failure);
        return false;
    }
    for (const KisPageStoreCheckpointPage &page : checkpoint.pages) {
        if (!store->adoptInitialPageBytes(page.version, page.descriptor,
                                          page.canonicalBytes, &failure)) {
            KisPageStoreDetail::setError(error, failure);
            return false;
        }
    }
    if (!store->finalizeInitialization(&failure)) {
        KisPageStoreDetail::setError(error, failure);
        return false;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}
