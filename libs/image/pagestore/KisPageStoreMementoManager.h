/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_STORE_MEMENTO_MANAGER_H
#define KIS_PAGE_STORE_MEMENTO_MANAGER_H

#include <QScopedPointer>

#include "KisPageStore.h"

struct KRITAIMAGE_EXPORT KisPageStoreHistoryTransaction
{
    quint64 capability = 0;
    KisPageTransaction transaction;

    bool isValid() const
    {
        return capability != 0 && transaction.isValid();
    }
};

struct KRITAIMAGE_EXPORT KisPageStoreMemento
{
    quint64 capability = 0;

    bool isValid() const { return capability != 0; }
};

/**
 * Retention-owning undo boundary for a canonical PageStore.
 *
 * A history transaction retains its exact before-root before returning the
 * PageStore transaction capability. commit() retains the exact after-root and
 * converts both roots into one opaque memento. rollback()/rollforward() only
 * accept capabilities minted by this instance, so callers never restore from
 * a naked epoch number or snapshot token.
 *
 * The PageStore must outlive this manager. close() is explicit so document
 * shutdown can prove that every retained root has been released.
 */
class KRITAIMAGE_EXPORT KisPageStoreMementoManager
{
public:
    explicit KisPageStoreMementoManager(
        const KisMutationStorageAllocator<KisPageStoreMementoManager> &storage = KisMutationStorageAllocator<KisPageStoreMementoManager>{});
    ~KisPageStoreMementoManager();

    KisPageStoreMementoManager(const KisPageStoreMementoManager &) = delete;
    KisPageStoreMementoManager &operator=(
        const KisPageStoreMementoManager &) = delete;

    bool configure(KisPageStore *store, QString *error = nullptr);
    KisPageStoreHistoryTransaction begin(QString *error = nullptr);
    KisPageStoreMemento commit(
        const KisPageStoreHistoryTransaction &transaction,
        QString *error = nullptr);
    bool abort(const KisPageStoreHistoryTransaction &transaction,
               QString *error = nullptr);

    KisImageEpochCommitTicket rollback(const KisPageStoreMemento &memento,
                                       QString *error = nullptr);
    KisImageEpochCommitTicket rollforward(const KisPageStoreMemento &memento,
                                          QString *error = nullptr);
    bool purge(const KisPageStoreMemento &memento,
               QString *error = nullptr);

    bool close(QString *error = nullptr);

private:
    KisImageEpochCommitTicket restore(
        const KisPageStoreMemento &memento,
        bool before,
        QString *error);
    class Private;
    struct PrivateReleaser { static void cleanup(Private *); };
    QScopedPointer<Private, PrivateReleaser> d;
};

#endif // KIS_PAGE_STORE_MEMENTO_MANAGER_H
