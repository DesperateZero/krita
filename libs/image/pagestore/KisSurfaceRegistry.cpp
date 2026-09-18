/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisSurfaceRegistry.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>

class KisSurfaceRegistry::Private
{
public:
    mutable QMutex mutex;
    quint64 nextSurface = 1;
    QHash<quint64, KisSurfaceDescriptor> descriptors;
};

KisSurfaceRegistry::KisSurfaceRegistry()
    : d(new Private)
{
}

KisSurfaceRegistry::~KisSurfaceRegistry() = default;

bool KisSurfaceRegistry::isOperational() const
{
    return true;
}

KisSurfaceId KisSurfaceRegistry::registerSurface(const KisSurfaceDescriptor &descriptor)
{
    if (!descriptor.isValid()) {
        return {};
    }

    QMutexLocker locker(&d->mutex);
    KisSurfaceId id;
    id.value = d->nextSurface++;
    d->descriptors.insert(id.value, descriptor);
    return id;
}

bool KisSurfaceRegistry::unregisterSurface(KisSurfaceId surface)
{
    if (!surface.isValid()) {
        return false;
    }

    QMutexLocker locker(&d->mutex);
    // PageStore must prove that no page/lease/epoch references remain before
    // reaching this identity-only registry boundary.
    return d->descriptors.remove(surface.value) == 1;
}

std::optional<KisSurfaceDescriptor> KisSurfaceRegistry::descriptor(KisSurfaceId surface) const
{
    if (!surface.isValid()) {
        return std::nullopt;
    }

    QMutexLocker locker(&d->mutex);
    const auto it = d->descriptors.constFind(surface.value);
    if (it == d->descriptors.constEnd()) {
        return std::nullopt;
    }
    return it.value();
}

bool KisSurfaceRegistry::contains(KisSurfaceId surface) const
{
    QMutexLocker locker(&d->mutex);
    return surface.isValid() && d->descriptors.contains(surface.value);
}

int KisSurfaceRegistry::surfaceCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->descriptors.size();
}
