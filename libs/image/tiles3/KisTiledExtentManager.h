/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *  SPDX-FileCopyrightText: 2018 Andrey Kamakin <a.kamakin@icloud.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISTILEDEXTENTMANAGER_H
#define KISTILEDEXTENTMANAGER_H

#include <QReadWriteLock>
#include <QMap>
#include <QRect>
#include <memory>
#include <memory_resource>
#include "kritaimage_export.h"


class KRITAIMAGE_EXPORT KisTiledExtentManager
{
    static constexpr qint32 InitialBufferSize = 256;

    class KRITAIMAGE_EXPORT Data
    {
    public:
        Data();
        ~Data();

        bool add(qint32 index);
        bool remove(qint32 index);
        void replace(const QVector<QPoint> &indexes, bool columns);
        void takePrepared(Data &prepared) noexcept;
        void clear();
        bool isEmpty();
        qint32 min();
        qint32 max();

    public:
        QReadWriteLock m_extentLock;

    private:
        // Storage only. Current counts are copied at installation, under the
        // original migration gate, rather than frozen during allocation.
        struct Growth {
            qint32 capacity = 0;
            qint32 offset = 0;
            struct DeleteBuffer {
                std::shared_ptr<std::pmr::memory_resource> resource;
                size_t count = 0;
                void operator()(QAtomicInt *data) const noexcept;
            };
            using Buffer = std::unique_ptr<QAtomicInt[], DeleteBuffer>;
            Buffer buffer{nullptr, DeleteBuffer{}};
            void allocate();
        };
        bool covers(qint32 first, qint32 last) const;
        Growth planGrowth(qint32 first, qint32 last) const;
        bool canInstall(const Growth &growth, qint32 first, qint32 last) const;
        void install(Growth &growth, qint32 first, qint32 last) noexcept;
        inline void unsafeAdd(qint32 index);
        void prepare(qint32 first, qint32 last);
        inline void updateMin();
        inline void updateMax();
        friend class KisTiledExtentManager;
        friend class KisTiledDataManagerTest;

    private:
        qint32 m_min;
        qint32 m_max;
        qint32 m_offset;
        qint32 m_capacity;
        qint32 m_count;
        Growth::Buffer m_buffer{nullptr, Growth::DeleteBuffer{}};
        QReadWriteLock m_migrationLock;
    };

public:
    KisTiledExtentManager();
    static void *operator new(size_t bytes);
    static void operator delete(void *data) noexcept;
    // Cold composition only, before either axis owns capacity or live counts.
    bool configureStorage(std::shared_ptr<std::pmr::memory_resource> resource);

    // Prepare both axes before any tile delta or pixels. This may grow retained
    // capacity, but never changes counts or the visible extent. Allocation and
    // old-buffer destruction occur outside the extent/migration gates.
    bool prepareTileRange(const QRect &tileRange);
    void notifyTileAdded(qint32 col, qint32 row);
    void notifyTileRemoved(qint32 col, qint32 row);
    void replaceTileStats(const QVector<QPoint> &indexes);
    // Caller excludes mutations; prepared is private and has no readers.
    // Exchanges already allocated storage, leaving old storage for deferred destruction.
    void takePrepared(KisTiledExtentManager &prepared) noexcept;
    void clear();
    QRect extent() const;

private:
    void updateExtent();
    friend class KisTiledDataManagerTest;

private:
    mutable QReadWriteLock m_extentLock;
    QRect m_currentExtent;
    Data m_colsData;
    Data m_rowsData;
};

#endif // KISTILEDEXTENTMANAGER_H
