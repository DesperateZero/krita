/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_SURFACE_CODEC_REGISTRY_H
#define KIS_SURFACE_CODEC_REGISTRY_H

#include <QByteArray>
#include <QScopedPointer>
#include <QSharedPointer>

#include "KisPageStoreTypes.h"

class KRITAIMAGE_EXPORT KisSurfaceCodec
{
public:
    virtual ~KisSurfaceCodec();

    virtual QByteArray id() const = 0;
    virtual quint32 version() const = 0;
    virtual bool supports(const KisSurfaceFormat &format) const = 0;
};

/**
 * Registry for explicit canonical format codecs. It never infers semantics
 * from byte count or silently treats arbitrary pixels as RGBA8.
 */
class KRITAIMAGE_EXPORT KisSurfaceCodecRegistry
{
public:
    KisSurfaceCodecRegistry();
    ~KisSurfaceCodecRegistry();

    bool isOperational() const;
    bool registerCodec(const QSharedPointer<KisSurfaceCodec> &codec);
    QSharedPointer<const KisSurfaceCodec> codecFor(const KisSurfaceFormat &format) const;
    int codecCount() const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_SURFACE_CODEC_REGISTRY_H
