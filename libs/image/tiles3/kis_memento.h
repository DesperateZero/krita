/*
 *  SPDX-FileCopyrightText: 2005 C. Boemann <cbo@boemann.dk>
 *            (c) 2009 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_MEMENTO_H_
#define KIS_MEMENTO_H_

#include <QtGlobal>
#include <QRect>

#include "kis_global.h"
#include "kritaimage_export.h"

#include <kis_shared.h>
#include <kis_shared_ptr.h>
#include <cstddef>


class KisMementoManager;

class KisMemento;
typedef KisSharedPtr<KisMemento> KisMementoSP;


class KRITAIMAGE_EXPORT KisMemento : public KisShared
{
public:
    static KisMemento *createPageStoreMemento();
    static void *operator new(std::size_t bytes);
    static void operator delete(void *data) noexcept;
    static void operator delete(void *data, std::size_t bytes) noexcept;

    inline KisMemento(KisMementoManager* mementoManager)
        : KisMemento(mementoManager, false) {}
    ~KisMemento();

    inline void extent(qint32 &x, qint32 &y, qint32 &w, qint32 &h) {
        const bool extentIsValid =
            m_extentMaxX >= m_extentMinX && m_extentMaxY >= m_extentMinY;

        if (extentIsValid) {
            x = m_extentMinX;
            y = m_extentMinY;
            w = m_extentMaxX - m_extentMinX + 1;
            h = m_extentMaxY - m_extentMinY + 1;
        } else {
            x = 0;
            y = 0;
            w = 0;
            h = 0;
        }
    }

    inline QRect extent() {
        qint32 x, y, w, h;
        extent(x, y, w, h);
        return QRect(x, y, w, h);
    }

    // Canonical history adapters publish the completed transaction's dirty
    // bounds before this memento reaches the original undo/redo consumers.
    void setExtent(const QRect &extent) {
        m_extentMinX = extent.isEmpty() ? qint32_MAX : extent.left();
        m_extentMinY = extent.isEmpty() ? qint32_MAX : extent.top();
        m_extentMaxX = extent.isEmpty() ? qint32_MIN : extent.right();
        m_extentMaxY = extent.isEmpty() ? qint32_MIN : extent.bottom();
    }

    void saveOldDefaultPixel(const quint8* pixel, quint32 pixelSize);
    void saveNewDefaultPixel(const quint8* pixel, quint32 pixelSize);

    const quint8* oldDefaultPixel() const {
        return m_oldDefaultPixel;
    }

    const quint8* newDefaultPixel() const {
        return m_newDefaultPixel;
    }

private:
    KisMemento(KisMementoManager* /*mementoManager*/, bool pageStoreStorage)
        : m_pageStoreStorage(pageStoreStorage) {
        m_extentMinX = qint32_MAX;
        m_extentMinY = qint32_MAX;
        m_extentMaxX = qint32_MIN;
        m_extentMaxY = qint32_MIN;

        m_oldDefaultPixel = 0;
        m_newDefaultPixel = 0;
    }

    friend class KisMementoManager;

    inline void updateExtent(qint32 col, qint32 row, QMutex *currentMementoExtentLock) {
        const qint32 tileMinX = col * KisTileData::WIDTH;
        const qint32 tileMinY = row * KisTileData::HEIGHT;
        const qint32 tileMaxX = tileMinX + KisTileData::WIDTH - 1;
        const qint32 tileMaxY = tileMinY + KisTileData::HEIGHT - 1;

        {
            /**
             * HACK ALERT: the lock is stored in the memento
             * manager to avoid too many locks to be created.
             * Anyway, a memento manager can have only one
             * "current memento". And it would not be nice to
             * do KisTileData::WIDTH/HEIGHT multiplication
             * under the lock held.
             */
            QMutexLocker l(currentMementoExtentLock);
            m_extentMinX = qMin(m_extentMinX, tileMinX);
            m_extentMaxX = qMax(m_extentMaxX, tileMaxX);
            m_extentMinY = qMin(m_extentMinY, tileMinY);
            m_extentMaxY = qMax(m_extentMaxY, tileMaxY);
        }
    }

private:
    quint8 *m_oldDefaultPixel;
    quint8 *m_newDefaultPixel;

    qint32 m_extentMinX;
    qint32 m_extentMaxX;
    qint32 m_extentMinY;
    qint32 m_extentMaxY;
    bool m_pageStoreStorage = false;
};

#endif // KIS_MEMENTO_H_
