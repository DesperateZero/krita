/* This file is part of the KDE project
 *  SPDX-FileCopyrightText: 2010 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _KISBASEACCESSOR_H_
#define _KISBASEACCESSOR_H_

#include <kritaimage_export.h>
#include <kis_shared.h>

class KRITAIMAGE_EXPORT KisBaseConstAccessor : public KisShared
{
    Q_DISABLE_COPY(KisBaseConstAccessor)
public:
    KisBaseConstAccessor() {}
    virtual ~KisBaseConstAccessor();
    /**
     * @return a pointer to the pixel data as it was at the moment of the last memento creation.
     */
    virtual const quint8 * oldRawData() const = 0;

    /**
     * @return the current pixel selection of this access operation. PageStore
     * immutable const traversals keep the view captured at construction, even
     * across cache eviction/reset; create a new accessor for a new operation.
     * Writable accessors return the same bytes as rawData(). A legacy read
     * overlapping a live same-thread writer remains a compatibility observer,
     * not an immutable snapshot; constructing it must not revoke write pointers.
     */
    virtual const quint8 * rawDataConst() const = 0;

    virtual qint32 x() const = 0;
    virtual qint32 y() const = 0;
};

class KRITAIMAGE_EXPORT KisBaseAccessor
{
    Q_DISABLE_COPY(KisBaseAccessor)
public:
    KisBaseAccessor() {}
    virtual ~KisBaseAccessor();
    /**
     * @return a pointer to the pixel data. Do NOT interpret the data - leave that to a colorspace
     */
    virtual quint8 * rawData() = 0;
};

#endif
