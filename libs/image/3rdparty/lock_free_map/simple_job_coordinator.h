/*------------------------------------------------------------------------
  Junction: Concurrent data structures in C++
  Copyright (c) 2016 Jeff Preshing
  Distributed under the Simplified BSD License.
  Original location: https://github.com/preshing/junction
  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the LICENSE file for more information.
------------------------------------------------------------------------*/

#ifndef SIMPLEJOBCOORDINATOR_H
#define SIMPLEJOBCOORDINATOR_H

#include <QMutex>
#include <QWaitCondition>
#include <QMutexLocker>

#include "kis_assert.h"
#include "atomic.h"

#define SANITY_CHECK

class SimpleJobCoordinator
{
public:
    struct Job {
        virtual ~Job()
        {
        }

        virtual void run() = 0;
    };

private:
    Atomic<quint64> m_job;
    Atomic<quint64> m_retryRevision{0};
    QMutex mutex;
    QWaitCondition condVar;

public:
    SimpleJobCoordinator() : m_job(quint64(NULL))
    {
    }

    Job* loadConsume() const
    {
        return (Job*) m_job.load(Consume);
    }

    void storeRelease(Job* job)
    {
        {
            QMutexLocker guard(&mutex);
            m_job.store(quint64(job), Release);
        }

        condVar.wakeAll();
    }

    void participate()
    {
        quint64 prevJob = quint64(NULL);
        quint64 prevRetryRevision = 0;

        for (;;) {
            quint64 job = m_job.load(Consume);
            quint64 retryRevision = m_retryRevision.load(Acquire);
            if (job == prevJob && retryRevision == prevRetryRevision) {
                QMutexLocker guard(&mutex);

                for (;;) {
                    job = m_job.loadNonatomic(); // No concurrent writes inside lock
                    retryRevision = m_retryRevision.loadNonatomic();
                    if (job != prevJob || retryRevision != prevRetryRevision) {
                        break;
                    }

                    condVar.wait(&mutex);
                }
            }

            if (job == 1) {
                return;
            }

            prevJob = job;
            // Capture before run(): a failed finishing attempt may signal
            // retry without replacing the still-owned job pointer.
            prevRetryRevision = retryRevision;
            reinterpret_cast<Job*>(job)->run();
        }
    }

    void notifyRetry()
    {
        {
            QMutexLocker guard(&mutex);
            m_retryRevision.fetchAdd(1, Release);
        }
        condVar.wakeAll();
    }

    void runOne(Job* job)
    {
#ifdef SANITY_CHECK
        KIS_ASSERT_RECOVER_NOOP(job != (Job*) m_job.load(Relaxed));
#endif // SANITY_CHECK
        storeRelease(job);
        job->run();
    }

    void end()
    {
        {
            QMutexLocker guard(&mutex);
            m_job.store(1, Release);
        }

        condVar.wakeAll();
    }
};

#endif // SIMPLEJOBCOORDINATOR_H
