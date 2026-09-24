/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PIXEL_WRITE_CURSOR_H
#define KIS_PIXEL_WRITE_CURSOR_H

#include <QtGlobal>
#include "kritaimage_export.h"

/** Borrowed destination-only cursor for one synchronous pixel operation.
 * A move to another page invalidates old pointers, even if the move fails.
 * Input/current/before readers are captured separately: there is deliberately
 * no oldRawData contract that could change with the selected write route.
 */
class KRITAIMAGE_EXPORT KisPixelWriteCursor
{
public:
    virtual ~KisPixelWriteCursor() = default;
    virtual void moveTo(qint32 x, qint32 y) = 0;
    virtual quint8 *rawData() = 0;
    virtual qint32 numContiguousColumns(qint32 x) const = 0;
    virtual qint32 numContiguousRows(qint32 y) const = 0;
    virtual qint32 rowStride(qint32 x, qint32 y) const = 0;
    virtual qint32 x() const = 0;
    virtual qint32 y() const = 0;
};

#endif
