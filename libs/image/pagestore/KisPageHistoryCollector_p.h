/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAGE_HISTORY_COLLECTOR_P_H
#define KIS_PAGE_HISTORY_COLLECTOR_P_H

#include "KisImageEpochReferenceModel.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageRetirementQueue_p.h"

#include <QAtomicInt>
#include <QMutex>
#include "KisPageWaitCondition_p.h"

#include <array>
#include <boost/intrusive/set.hpp>

struct KisPageHistoryWakeContext;

struct KisPageHistoryCollectorSnapshot
{
    qsizetype pendingPages = 0;
    qsizetype deferredPages = 0;
    qsizetype activeScans = 0;
    quint64 retryWakeups = 0;
    bool retryScheduled = false;
    bool jobScheduled = false;
    quint64 pagesVisited = 0;
    quint64 foregroundPagesVisited = 0;
    quint64 maximumPagesPerPass = 0;
    quint64 maximumVersionsPerPass = 0;
    quint64 versionSlices = 0;
};

/**
 * Bounded historical-version collection from original Value live edges.
 *
 * All methods ending in Locked require the PageStore owner mutex. Physical
 * retirement is delegated after releasing that mutex; this service never
 * owns provider operations or publishes epoch roots. Abandoned mutations stay
 * on the Store's original scope links and share this prepared collection pass
 * and retry delay; their cancellation runs without the owner mutex.
 */
class KisPageHistoryCollector final
{
public:
    static constexpr qsizetype PageAdmissionBudget = 16;
    static constexpr qsizetype VersionScanBudget = 32;
    static constexpr qsizetype IdleCapacityBudget = 16;

    using ReleaseOwnerLifetime = void (*)(void *context);
    using HasAbandonedMutations = bool (*)(void *context);
    using CollectAbandonedMutations = void (*)(void *context);
    // Observing the original chain never retires capacity. Only the worker
    // invokes collection, under the Store gate and with a bounded allowance.
    using NextIdleDeadline = qint64 (*)(void *context);
    using CollectIdleCapacity = void (*)(void *context, qint64 now, qsizetype budget);

    KRITAIMAGE_EXPORT KisPageHistoryCollector(KisPageMetadataCoordinator &metadata,
                            KisImageEpochReferenceModel &epochs,
                            KisPageRetirementQueue &retirementQueue,
                            QMutex &ownerMutex,
                            QAtomicInt &ownerLifetimeReferences,
                            bool &operational,
                            bool &closing,
                            bool &backgroundReclamation,
                            void *ownerContext,
                            ReleaseOwnerLifetime releaseOwnerLifetime,
                            HasAbandonedMutations hasAbandonedMutations = nullptr,
                            CollectAbandonedMutations collectAbandonedMutations = nullptr,
                            NextIdleDeadline nextIdleDeadline = nullptr,
                            CollectIdleCapacity collectIdleCapacity = nullptr);
    KRITAIMAGE_EXPORT ~KisPageHistoryCollector();
    KRITAIMAGE_EXPORT void prepareTask(KisBackingBudgetController &budget);

    KisPageHistoryCollector(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector &operator=(const KisPageHistoryCollector &) = delete;
    KisPageHistoryCollector(KisPageHistoryCollector &&) = delete;
    KisPageHistoryCollector &operator=(KisPageHistoryCollector &&) = delete;

    KRITAIMAGE_EXPORT void collectUnreachableLocked();
    // Changed keys and actual last-root-edge notifications share the same
    // bounded collector. Neither publication nor base exit scans known keys.
    void collectAfterEpochChangeLocked();
    KRITAIMAGE_EXPORT void requestKeyLocked(const KisPageKey &key) noexcept;
    KRITAIMAGE_EXPORT void scheduleLocked();
    KRITAIMAGE_EXPORT void waitForIdleLocked();
    KRITAIMAGE_EXPORT void stopAutomaticWakeups();
    void clearLocked();

    KRITAIMAGE_EXPORT KisPageHistoryCollectorSnapshot snapshotLocked() const;

private:
    // True consumes the original metadata census under its shard gate, or a
    // successful read-protection result without releasing Store admission.
    // It is only a scheduling fact; collection still checks live protection.
    // Ordinary requests query only a new key's history.
    void requestKeyLocked(const KisPageKey &, bool historyAlreadyObserved) noexcept;
    friend class KisPageReadCoordinator;
    struct Work : boost::intrusive::list_base_hook<>
    {
        explicit Work(const KisPageKey &value) noexcept : key(value) {}
        boost::intrusive::set_member_hook<> indexHook;
        KisPageKey key;
        KisPageVersion after;
        bool repeat = false;
    };
    struct KeyLess {
        bool operator()(const KisPageKey &a, const KisPageKey &b) const {
            if (a.surface.value != b.surface.value) return a.surface.value < b.surface.value;
            if (a.page.row != b.page.row) return a.page.row < b.page.row;
            return a.page.column < b.page.column;
        }
    };
    struct WorkKey {
        using type = KisPageKey;
        const KisPageKey &operator()(const Work &work) const noexcept { return work.key; }
    };
    using WorkIndex = boost::intrusive::set<Work,
        boost::intrusive::member_hook<Work, boost::intrusive::set_member_hook<>, &Work::indexHook>,
        boost::intrusive::key_of_value<WorkKey>, boost::intrusive::compare<KeyLess>>;
    enum class SliceResult { Ready, Blocked, Removed };
    SliceResult collectSliceLocked(Work &work, quint64 &visitedVersions);
    bool collectPassLocked();
    void queueWorkLocked(Work &work) noexcept;
    void disposeWork(Work *work) noexcept;
    bool hasSemanticWorkLocked() const;
    // Requires the notification gate only. Both foreground and timer use
    // this one admission; neither can queue a second instance of the task.
    bool enqueueTask(KisPageHistoryWakeContext &context);

    KisPageMetadataCoordinator &m_metadata;
    KisImageEpochReferenceModel &m_epochs;
    KisPageRetirementQueue &m_retirementQueue;
    QMutex &m_ownerMutex;
    QAtomicInt &m_ownerLifetimeReferences;
    bool &m_operational;
    bool &m_closing;
    bool &m_backgroundReclamation;
    void *m_ownerContext = nullptr;
    ReleaseOwnerLifetime m_releaseOwnerLifetime = nullptr;
    HasAbandonedMutations m_hasAbandonedMutations = nullptr;
    CollectAbandonedMutations m_collectAbandonedMutations = nullptr;
    NextIdleDeadline m_nextIdleDeadline = nullptr;
    CollectIdleCapacity m_collectIdleCapacity = nullptr;

    KisMutationStorageAllocator<Work> m_workAllocator;
    WorkIndex m_work;
    boost::intrusive::list<Work> m_ready;
    bool m_rescanRequested = false;
    KisPageReclamationJobPointer m_task;
    std::shared_ptr<KisPageHistoryWakeContext> m_wakeContext;
    qint64 m_pressureDeadline = 0;
    bool m_blocked = false;
    bool m_automaticWakeupsStopped = false;
    int m_retryDelayMs = 1;

    quint64 m_pagesVisited = 0;
    quint64 m_foregroundPagesVisited = 0;
    quint64 m_maximumPagesPerPass = 0;
    quint64 m_maximumVersionsPerPass = 0;
    quint64 m_versionSlices = 0;
};

#endif // KIS_PAGE_HISTORY_COLLECTOR_P_H
