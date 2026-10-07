/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_ORDERED_PIXEL_OPERATION_P_H
#define KIS_ORDERED_PIXEL_OPERATION_P_H

#include <QRect>
#include <QVector>
#include <functional>

class KisPixelWriteCursor;

// Synchronously borrowed pixel input, not mutation authority. The caller
// certifies independent destination pixels; overlapping rectangles retain
// their input order and multiplicity. Storage/permissions stay in the manager.
struct KisOrderedPixelRect {
    QRect area;
    qsizetype sourceOrdinal;
};
struct KisOrderedPixelOperation {
    const QVector<KisOrderedPixelRect> &rects;
    std::function<bool(KisPixelWriteCursor *, qsizetype, const QRect &)> apply;
};

#endif
