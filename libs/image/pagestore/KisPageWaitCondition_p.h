/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_WAIT_CONDITION_P_H
#define KIS_PAGE_WAIT_CONDITION_P_H

#include <QMutex>
#include <QScopeGuard>
#include <QtGlobal>

#include <condition_variable>
#include <mutex>

/**
 * Waiting storage lives inside the original, budgeted PageStore owner.
 *
 * Call wait with the owner's non-recursive mutex locked, and recheck the
 * owner's predicate afterwards. The notification gate closes the interval
 * between releasing that mutex and sleeping. Wake callers may hold the owner
 * mutex, so wait must release the notification gate before reacquiring it.
 * Owner shutdown must drain its waiters before destroying this member.
 */
class KisPageWaitCondition final
{
public:
    void wait(QMutex *ownerMutex)
    {
        std::unique_lock<std::mutex> notificationLock(m_notificationMutex);
        const quint64 observed = m_notifications;
        ownerMutex->unlock();
        auto restoreOwner = qScopeGuard([&] {
            notificationLock.unlock();
            ownerMutex->lock();
        });
        m_changed.wait(notificationLock, [&] { return observed != m_notifications; });
    }

    void wakeAll()
    {
        {
            std::lock_guard<std::mutex> notificationLock(m_notificationMutex);
            ++m_notifications;
        }
        m_changed.notify_all();
    }

private:
    std::mutex m_notificationMutex;
    std::condition_variable m_changed;
    quint64 m_notifications = 0;
};

#endif
