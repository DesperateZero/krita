/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_CPU_ACCESS_SESSION_H
#define KIS_PAGE_STORE_CPU_ACCESS_SESSION_H

#include <QScopedPointer>

#include "KisPageStore.h"

enum class KisCpuAccessLifetime {
    OperationSpans,
    // A returned span is invalidated on access to a different PageKey.
    // Native writes park pending backing; generic writes keep their control
    // lease until finish, but before-image/read pins follow the cursor.
    CursorPage
};

struct KisCpuPageReadSpan
{
    KisPageVersion version;
    const quint8 *data = nullptr;
    quint32 rowStride = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return version.isValid() && data && rowStride != 0 &&
               byteSize >= rowStride;
    }
};

struct KisCpuPageWriteSpan
{
    KisPageVersion baseVersion;
    KisPageVersion version;
    quint8 *data = nullptr;
    const quint8 *oldData = nullptr;
    quint32 rowStride = 0;
    quint64 byteSize = 0;

    bool isValid() const
    {
        return baseVersion.isValid() && version.isValid() &&
               baseVersion.key == version.key && data && oldData &&
               rowStride != 0 && byteSize >= rowStride;
    }
};

/**
 * Test-only operation-scoped CPU facade for standalone Store coverage.
 *
 * Every read selector is captured once at beginRead(), including surface
 * metadata/default revision. TransactionOverlay freezes its sealed delta over
 * the protected base; later writes, removals, commit and abort do not change it.
 * Resident pixel acquisition first tries an exact native guard. Missing native
 * support or actual nonresident bindings retain the generic lease fallback.
 * Local mutex/swap barriers may be waited on without initiating swap-in;
 * writer busy, invalid identity and resource failure are distinct errors.
 *
 * Returned spans are non-owning. OperationSpans (the default) keeps them until
 * finish/cancel/destruction; CursorPage also invalidates them on an attempted
 * access to a different PageKey, including a failed acquisition.
 * A read session owns exact native guards/read leases. A
 * write session captures its transaction base once for oldData and uses a
 * native mutation segment when its provider supports that route; otherwise it
 * selects generic writes before starting work. It owns one native guard or
 * lease per touched PageKey in OperationSpans mode. A native CursorPage keeps
 * only the current guard/before pin; the segment retains all pending versions.
 * Generic write leases cannot be dropped early without a provider suspension
 * contract and remain until finish. finish seals, never commits the epoch root.
 * A native operation failure is never replayed through the generic path.
 * The PageStore facade must outlive this session; only a standalone captured
 * view or native guard supports outliving that facade.
 */
class KisPageStoreCpuAccessSession
{
public:
    KisPageStoreCpuAccessSession();
    explicit KisPageStoreCpuAccessSession(KisCpuAccessLifetime lifetime);
    ~KisPageStoreCpuAccessSession();

    KisPageStoreCpuAccessSession(const KisPageStoreCpuAccessSession &) = delete;
    KisPageStoreCpuAccessSession &operator=(
        const KisPageStoreCpuAccessSession &) = delete;

    bool beginRead(KisPageStore *store,
                   KisSurfaceId surface,
                   const KisPageReadView &view,
                   KisPagePriority priority,
                   QString *error = nullptr);
    bool beginWrite(KisPageStore *store,
                    KisSurfaceId surface,
                    const KisPageTransaction &transaction,
                    KisPageWriteMode mode,
                    KisPagePriority priority,
                    QString *error = nullptr);

    bool isActive() const;
    // Uses the same captured epoch as readPage(), never a fresh current head.
    bool resolveReadSurfaceState(KisSurfaceEpochState *state) const;
    KisCpuPageReadSpan readPage(qint32 column,
                                qint32 row,
                                QString *error = nullptr);
    KisCpuPageWriteSpan writePage(qint32 column,
                                  qint32 row,
                                  QString *error = nullptr);

    // Publishes touched write generations but does not commit the surrounding
    // multi-page transaction. The caller remains responsible for
    // preparedPages()/commit() or abort().
    bool finish(QString *error = nullptr);
    void cancel();

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_PAGE_STORE_CPU_ACCESS_SESSION_H
