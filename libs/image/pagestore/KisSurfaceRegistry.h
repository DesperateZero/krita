/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_SURFACE_REGISTRY_H
#define KIS_SURFACE_REGISTRY_H

#include <QSize>
#include <QScopedPointer>
#include <QString>

#include <optional>

#include "KisPageStoreTypes.h"

struct KRITAIMAGE_EXPORT KisSurfaceDescriptor
{
    QString debugName;
    KisSurfaceFormat format;
    QSize pixelExtent;
    QSize logicalPageExtent;
    quint64 ownerDocument = 0;

    bool isValid() const
    {
        return ownerDocument != 0 &&
               format.isValid() &&
               pixelExtent.isValid() &&
               logicalPageExtent.isValid();
    }
};

/**
 * Document-scoped identity registry. BR0 intentionally exposes no global
 * singleton and does not register surfaces until the BR1 CPU PageStore exists.
 */
class KRITAIMAGE_EXPORT KisSurfaceRegistry
{
public:
    KisSurfaceRegistry();
    ~KisSurfaceRegistry();

    KisSurfaceId registerSurface(const KisSurfaceDescriptor &descriptor);
    bool unregisterSurface(KisSurfaceId surface);
    std::optional<KisSurfaceDescriptor> descriptor(KisSurfaceId surface) const;
    bool contains(KisSurfaceId surface) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_SURFACE_REGISTRY_H
