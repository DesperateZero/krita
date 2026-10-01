/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_stroke.h"

#include "kis_stroke_strategy.h"


KisStroke::KisStroke(KisStrokeStrategy *strokeStrategy, Type type, int levelOfDetail)
    : m_strokeStrategy(strokeStrategy),
      m_strokeInitialized(false),
      m_strokeEnded(false),
      m_strokeSuspended(false),
      m_isCancelled(false),
      m_worksOnLevelOfDetail(levelOfDetail),
      m_type(type)
{
    m_initStrategy.reset(m_strokeStrategy->createInitStrategy());
    m_dabStrategy.reset(m_strokeStrategy->createDabStrategy());
    m_cancelStrategy.reset(m_strokeStrategy->createCancelStrategy());
    m_finishStrategy.reset(m_strokeStrategy->createFinishStrategy());
    m_suspendStrategy.reset(m_strokeStrategy->createSuspendStrategy());
    m_resumeStrategy.reset(m_strokeStrategy->createResumeStrategy());

    m_strokeStrategy->notifyUserStartedStroke();

    if(!m_initStrategy) {
        m_strokeInitialized = true;
    }
    else {
        enqueue(m_initStrategy.data(), m_strokeStrategy->createInitData());
    }
}

KisStroke::~KisStroke()
{
    Q_ASSERT(m_strokeEnded);
    Q_ASSERT(m_jobsQueue.isEmpty());
}

bool KisStroke::supportsSuspension()
{
    return !m_strokeInitialized || (m_suspendStrategy && m_resumeStrategy);
}

void KisStroke::suspendStroke(KisStrokeSP recipient)
{
    if (!m_strokeInitialized || m_strokeSuspended ||
        (m_strokeEnded && !hasJobs())) {

        return;
    }

    KIS_ASSERT_RECOVER_NOOP(m_suspendStrategy && m_resumeStrategy);

    prepend(m_resumeStrategy.data(),
            m_strokeStrategy->createResumeData(),
            worksOnLevelOfDetail(), false);

    recipient->prepend(m_suspendStrategy.data(),
                       m_strokeStrategy->createSuspendData(),
                       worksOnLevelOfDetail(), false);

    m_strokeSuspended = true;
}

void KisStroke::addJob(KisStrokeJobData *data)
{
    // Input submitted after a worker failure must not run after cleanup.
    // Required children of already running jobs use addMutatedJobs instead.
    if (m_failed) {
        delete data;
        return;
    }
    KIS_SAFE_ASSERT_RECOVER_NOOP(!m_strokeEnded);
    enqueue(m_dabStrategy.data(), data);
}

void KisStroke::addMutatedJobs(const QVector<KisStrokeJobData *> list)
{
    // factory methods can return null, if no action is needed
    if (!m_dabStrategy) {
        qDeleteAll(list);
        return;
    }

    // Find first non-alien (non-suspend/non-resume) job
    //
    // Please note that this algorithm will stop working at the day we start
    // adding alien jobs not to the beginning of the stroke, but to other places.
    // Right now both suspend and resume jobs are added to the beginning of
    // the stroke.

    auto it = std::find_if(m_jobsQueue.begin(), m_jobsQueue.end(),
                           std::mem_fn(&KisStrokeJob::isOwnJob));

    Q_FOREACH (KisStrokeJobData *data, list) {
        if (m_failed && (!data || data->isCancellable())) {
            delete data;
            continue;
        }
        it = m_jobsQueue.insert(it, new KisStrokeJob(m_dabStrategy.data(), data, worksOnLevelOfDetail(), true));
        ++it;
    }
}

bool KisStroke::addCheckpointJob(KisStrokeJobData *data)
{
    QScopedPointer<KisStrokeJobData> input(data);
    if (!data || !m_dabStrategy || m_isCancelled || m_failed ||
        (!data->isSequential() && !data->isBarrier()) || !data->isCancellable() ||
        (data->levelOfDetailOverride() >= 0 &&
         data->levelOfDetailOverride() != worksOnLevelOfDetail()) ||
        (m_strokeEnded && !m_queuedFinishJob)) {
        return false;
    }

    // The queue mutex linearizes this position with normal input admission.
    // Existing jobs and their recursively prepended children remain before it;
    // input accepted later stays after it. Sequential dispatch waits for all
    // running stroke jobs without making the requester wait for itself.
    auto position = m_jobsQueue.end();
    if (m_queuedFinishJob) {
        // Normal input has closed, and mutations only prepend. The queued
        // finish stays last, so locating it must not scan the stroke backlog.
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(
            !m_jobsQueue.isEmpty() && m_jobsQueue.back() == m_queuedFinishJob, false);
        --position;
    }
    QScopedPointer<KisStrokeJob> job(new KisStrokeJob(
        m_dabStrategy.data(), input.data(), worksOnLevelOfDetail(), true));
    input.take();
    m_jobsQueue.insert(position, job.data());
    job.take();
    return true;
}

KisStrokeJob* KisStroke::popOneJob()
{
    KisStrokeJob *job = dequeue();

    if(job) {
        m_retryCancellation = false;
        m_strokeInitialized = true;
        m_strokeSuspended = false;
    }

    return job;
}

KUndo2MagicString KisStroke::name() const
{
    return m_strokeStrategy->name();
}

QString KisStroke::id() const
{
    return m_strokeStrategy->id();
}

bool KisStroke::hasJobs() const
{
    return !m_jobsQueue.isEmpty();
}

qint32 KisStroke::numJobs() const
{
    return m_jobsQueue.size();
}

void KisStroke::endStroke()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(!m_strokeEnded);
    m_strokeEnded = true;

    if (!m_failed) {
        enqueue(m_finishStrategy.data(), m_strokeStrategy->createFinishData());
        if (m_finishStrategy) m_queuedFinishJob = m_jobsQueue.back();
    }
    m_strokeStrategy->notifyUserEndedStroke();
}

/**
 * About cancelling the stroke
 * There may be four different states of the stroke, when cancel
 * is requested:
 * 1) Not initialized, has jobs -- just clear the queue
 * 2) Initialized, has jobs, not finished -- clear the queue,
 *    enqueue the cancel job
 * 5) Initialized, no jobs, not finished -- enqueue the cancel job
 * 3) Initialized, has jobs, finished -- clear the queue, enqueue
 *    the cancel job
 * 4) Initialized, no jobs, finished -- it's too late to cancel
 *    anything
 * 6) Initialized, has jobs, cancelled -- cancelling twice is a permitted
 *                                        operation, though it does nothing
 */

void KisStroke::cancelStroke()
{
    if (m_failed) {
        m_strokeEnded = true;
        return;
    }
    // case 6
    if (m_isCancelled) return;

    const bool effectivelyInitialized =
        m_strokeInitialized || m_strokeStrategy->needsExplicitCancel();

    if(!effectivelyInitialized) {
        /**
         * Lod0 stroke cannot be suspended and !initialized at the
         * same time, because the suspend job is created iff the
         * stroke has already done some meaningful work.
         *
         * At the same time, LodN stroke can be prepended with a
         * 'suspend' job even when it has not been started yet. That
         * is obvious: we should suspend the other stroke before doing
         * anything else.
         */
        KIS_ASSERT_RECOVER_NOOP(type() == LODN ||
                                sanityCheckAllJobsAreCancellable());
        clearQueueOnCancel();
    }
    else if(effectivelyInitialized &&
            (!m_jobsQueue.isEmpty() || !m_strokeEnded)) {

        m_strokeStrategy->tryCancelCurrentStrokeJobAsync();

        clearQueueOnCancel();
        enqueue(m_cancelStrategy.data(),
                m_strokeStrategy->createCancelData());
    }
    // else {
    //     too late ...
    // }

    m_isCancelled = true;
    m_strokeEnded = true;
}

bool KisStroke::failStroke()
{
    if (m_failed) return false;
    m_failed = true;
    m_isCancelled = true;
    m_strokeStrategy->tryCancelCurrentStrokeJobAsync();
    clearQueueOnCancel();
    // Unlike a user cancel, a worker failure must also clean up a finish job
    // that has just failed with no more jobs queued. Do not close the input
    // handle here: addJob/endStroke may still arrive from its GUI owner.
    if (m_strokeInitialized || m_strokeSuspended || m_strokeStrategy->needsExplicitCancel())
        enqueue(m_cancelStrategy.data(), m_strokeStrategy->createCancelData());
    return true;
}

int KisStroke::retryCancellation()
{
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(m_isCancelled && m_cancelStrategy, 0);
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(!m_retryCancellation, 0);
    m_failed = true;
    const int delay = qMin(100, 1 << qMin<int>(m_cancellationRetries, 7));
    if (m_cancellationRetries < 7) ++m_cancellationRetries;
    m_retryDeadline = QDeadlineTimer(delay, Qt::PreciseTimer);
    m_retryCancellation = true;
    auto *data = m_strokeStrategy->createCancelData();
    if (data) data->setCancellable(false);
    enqueue(m_cancelStrategy.data(), data);
    return delay;
}

int KisStroke::cancellationRetryDelay() const
{
    return m_retryCancellation ? qMax(0, int(m_retryDeadline.remainingTime())) : 0;
}

bool KisStroke::canCancel() const
{
    return m_isCancelled || !m_strokeInitialized ||
        !m_jobsQueue.isEmpty() || !m_strokeEnded;
}

bool KisStroke::sanityCheckAllJobsAreCancellable() const
{
    Q_FOREACH (KisStrokeJob *item, m_jobsQueue) {
        if (!item->isCancellable()) {
            return false;
        }
    }
    return true;
}

void KisStroke::clearQueueOnCancel()
{
    QQueue<KisStrokeJob*>::iterator it = m_jobsQueue.begin();

    while (it != m_jobsQueue.end()) {
        if ((*it)->isCancellable()) {
            if (*it == m_queuedFinishJob) m_queuedFinishJob = nullptr;
            delete (*it);
            it = m_jobsQueue.erase(it);
        } else {
            ++it;
        }
    }
}

bool KisStroke::isInitialized() const
{
    return m_strokeInitialized;
}

bool KisStroke::isEnded() const
{
    return m_strokeEnded;
}

bool KisStroke::isCancelled() const
{
    return m_isCancelled;
}

bool KisStroke::isExclusive() const
{
    return m_strokeStrategy->isExclusive();
}

bool KisStroke::supportsWrapAroundMode() const
{
    return m_strokeStrategy->supportsWrapAroundMode();
}

int KisStroke::worksOnLevelOfDetail() const
{
    return m_worksOnLevelOfDetail;
}

bool KisStroke::canForgetAboutMe() const
{
    return m_strokeStrategy->canForgetAboutMe();
}

bool KisStroke::isAsynchronouslyCancellable() const
{
    return m_strokeStrategy->isAsynchronouslyCancellable();
}

bool KisStroke::clearsRedoOnStart() const
{
    return m_strokeStrategy->clearsRedoOnStart();
}

qreal KisStroke::balancingRatioOverride() const
{
    return m_strokeStrategy->balancingRatioOverride();
}

KisStrokeJobData::Sequentiality KisStroke::nextJobSequentiality() const
{
    return !m_jobsQueue.isEmpty() ?
        m_jobsQueue.head()->sequentiality() : KisStrokeJobData::SEQUENTIAL;
}

int KisStroke::nextJobLevelOfDetail() const
{
    return !m_jobsQueue.isEmpty() ?
                m_jobsQueue.head()->levelOfDetail() : worksOnLevelOfDetail();
}

void KisStroke::enqueue(KisStrokeJobStrategy *strategy,
                        KisStrokeJobData *data)
{
    // factory methods can return null, if no action is needed
    if(!strategy) {
        delete data;
        return;
    }

    m_jobsQueue.enqueue(new KisStrokeJob(strategy, data, worksOnLevelOfDetail(), true));
}

void KisStroke::prepend(KisStrokeJobStrategy *strategy,
                        KisStrokeJobData *data,
                        int levelOfDetail,
                        bool isOwnJob)
{
    // factory methods can return null, if no action is needed
    if(!strategy) {
        delete data;
        return;
    }

    // LOG_MERGE_FIXME:
    Q_UNUSED(levelOfDetail);

    m_jobsQueue.prepend(new KisStrokeJob(strategy, data, worksOnLevelOfDetail(), isOwnJob));
}

KisStrokeJob* KisStroke::dequeue()
{
    KisStrokeJob *job = !m_jobsQueue.isEmpty() ? m_jobsQueue.dequeue() : nullptr;
    if (job == m_queuedFinishJob) m_queuedFinishJob = nullptr;
    return job;
}

void KisStroke::setLodBuddy(KisStrokeSP buddy)
{
    m_lodBuddy = buddy;
}

KisStrokeSP KisStroke::lodBuddy() const
{
    return m_lodBuddy;
}

KisStroke::Type KisStroke::type() const
{
    if (m_type == LOD0) {
        KIS_ASSERT_RECOVER_NOOP(m_lodBuddy && "LOD0 strokes must always have a buddy");
    } else if (m_type == LODN) {
        KIS_ASSERT_RECOVER_NOOP(m_worksOnLevelOfDetail > 0 && "LODN strokes must work on LOD > 0!");
    } else if (m_type == LEGACY) {
        KIS_ASSERT_RECOVER_NOOP(m_worksOnLevelOfDetail == 0 && "LEGACY strokes must work on LOD == 0!");
    }

    return m_type;
}
