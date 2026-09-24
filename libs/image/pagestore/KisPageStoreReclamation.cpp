/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageStoreReclamation_p.h"

#include <QRunnable>
#include <QThreadPool>
#include <QCoreApplication>

namespace {
thread_local bool onReclaimer = false;

class ReclamationExecutor
{
public:
    ReclamationExecutor()
    {
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(-1);
        // Drain before Qt/application teardown reaches tile-store and other
        // process statics, irrespective of their first-use construction order.
        // Document/session owners must stop producing work before app teardown.
        qAddPostRoutine(kisDrainPageStoreReclamation);
    }
    ~ReclamationExecutor()
    {
        qRemovePostRoutine(kisDrainPageStoreReclamation);
        pool.waitForDone();
    }
    QThreadPool pool;
};

ReclamationExecutor &executor()
{
    // Created before any queued job; destruction drains continuations before
    // the pool is destroyed. No QObject event loop or image scheduler required.
    static ReclamationExecutor instance;
    return instance;
}
}

void kisSchedulePageStoreReclamation(std::function<void()> job)
{
    executor().pool.start(QRunnable::create([job = std::move(job)] {
        onReclaimer = true;
        job();
        // Captures can themselves release tree roots / owner state. Keep the
        // marker valid through their destruction, not just the callback body.
    }));
}

bool kisOnPageStoreReclamationThread() { return onReclaimer; }

void kisDrainPageStoreReclamation()
{
    Q_ASSERT(!onReclaimer);
    executor().pool.waitForDone();
}
