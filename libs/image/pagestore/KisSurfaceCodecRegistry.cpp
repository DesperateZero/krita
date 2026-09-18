/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisSurfaceCodecRegistry.h"

#include <QMutex>
#include <QMutexLocker>
#include <QVector>

KisSurfaceCodec::~KisSurfaceCodec() = default;

class KisSurfaceCodecRegistry::Private
{
public:
    mutable QMutex mutex;
    QVector<QSharedPointer<KisSurfaceCodec>> codecs;
};

KisSurfaceCodecRegistry::KisSurfaceCodecRegistry()
    : d(new Private)
{
}

KisSurfaceCodecRegistry::~KisSurfaceCodecRegistry() = default;

bool KisSurfaceCodecRegistry::isOperational() const
{
    return true;
}

bool KisSurfaceCodecRegistry::registerCodec(const QSharedPointer<KisSurfaceCodec> &codec)
{
    if (!codec || codec->id().isEmpty() || codec->version() == 0) {
        return false;
    }
    QMutexLocker locker(&d->mutex);
    for (const QSharedPointer<KisSurfaceCodec> &registered : d->codecs) {
        if (registered->id() == codec->id() &&
            registered->version() == codec->version()) {
            return false;
        }
    }
    d->codecs.append(codec);
    return true;
}

QSharedPointer<const KisSurfaceCodec> KisSurfaceCodecRegistry::codecFor(
    const KisSurfaceFormat &format) const
{
    if (!format.isValid()) {
        return {};
    }
    QMutexLocker locker(&d->mutex);
    QSharedPointer<const KisSurfaceCodec> match;
    for (const QSharedPointer<KisSurfaceCodec> &codec : d->codecs) {
        if (!codec->supports(format)) continue;
        // More than one claimant is ambiguous and must fail closed.
        if (match) return {};
        match = codec;
    }
    return match;
}

int KisSurfaceCodecRegistry::codecCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->codecs.size();
}
