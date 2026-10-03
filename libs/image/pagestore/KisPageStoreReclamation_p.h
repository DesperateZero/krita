/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_STORE_RECLAMATION_P_H
#define KIS_PAGE_STORE_RECLAMATION_P_H

#include <atomic>
#include <memory>
#include <QtGlobal>
#include <kritaimage_export.h>
#include "KisMutationStorage_p.h"
#include "KisPageReadiness_p.h"

class KisBackingBudgetController;

// Separate from the image/brush worker pool. Jobs must own everything they
// use, must not wait for another reclamation job, and must bound their work.
// This is execution only: it never decides logical/physical release eligibility.
// Private runtime/test boundary; exporting the symbol does not grant page
// mutation authority. Tests use queued barriers to prove inter-pass ordering.
struct KisPageReclamationJob
{
    KisPageReclamationJob *next = nullptr;
    void (*finished)(void *) = nullptr;
    void *finishContext = nullptr;
    bool reusable = false;
    virtual void invoke() = 0;
    virtual void dispose() noexcept = 0;
protected:
    ~KisPageReclamationJob() = default;
};
// Transfers a prepared node only on success. The executor's single persistent
// runnable consumes these intrusive nodes; enqueue never creates a second
// callback capture or a per-job QRunnable.
// A null node only prepares the process runtime at a cold composition boundary.
// Permanent stop rejects new producers. Already accepted worker continuations
// may finish draining; false leaves the node and its responsibility with caller.
KRITAIMAGE_EXPORT bool kisEnqueuePageStoreReclamation(KisPageReclamationJob *job);
struct KisPageReclamationJobDeleter {
    void operator()(KisPageReclamationJob *job) const noexcept { if (job) job->dispose(); }
};
using KisPageReclamationJobPointer =
    std::unique_ptr<KisPageReclamationJob, KisPageReclamationJobDeleter>;

template<class Function>
struct KisPageReclamationTask final : KisPageReclamationJob {
    Function function;
    KisMutationStorageAllocator<KisPageReclamationTask> storage;
    template<class Value>
    KisPageReclamationTask(Value &&value, KisMutationStorageAllocator<KisPageReclamationTask> allocator)
        : function(std::forward<Value>(value)), storage(std::move(allocator)) {}
    void invoke() override { function(); }
    void dispose() noexcept override
    {
        auto allocator = std::move(storage);
        std::destroy_at(this);
        allocator.deallocate(this, 1);
    }
};

// Cold preparation admits the real inline closure and starts the existing
// runtime. The original consumer owns the reusable node and excludes concurrent
// enqueue; its completion pin must keep that owner alive through finished().
template<class Function>
KisPageReclamationJobPointer kisPreparePageStoreReclamation(Function &&function,
                                    KisBackingBudgetController *budget = nullptr,
                                    void (*finished)(void *) = nullptr,
                                    void *finishContext = nullptr)
{
    if (!kisEnqueuePageStoreReclamation(nullptr)) throw std::bad_alloc();
    using Job = KisPageReclamationTask<std::decay_t<Function>>;
    auto allocator = KisMutationStorageAllocator<Job>::retained(budget);
    void *storage = allocator.allocate(1);
    Job *job = nullptr;
    try {
        job = ::new (storage) Job(std::forward<Function>(function), allocator);
        job->finished = finished;
        job->finishContext = finishContext;
        job->reusable = true;
        return KisPageReclamationJobPointer(job);
    } catch (...) {
        if (job) job->dispose();
        else allocator.deallocate(static_cast<Job *>(storage), 1);
        throw;
    }
}

template<class Function>
void kisSchedulePageStoreReclamation(Function &&function,
                                    KisBackingBudgetController *budget = nullptr,
                                    void (*finished)(void *) = nullptr,
                                    void *finishContext = nullptr)
{
    auto job = kisPreparePageStoreReclamation(std::forward<Function>(function),
                                             budget, finished, finishContext);
    job->reusable = false;
    if (!kisEnqueuePageStoreReclamation(job.get())) throw std::bad_alloc();
    job.release();
}
KRITAIMAGE_EXPORT bool kisOnPageStoreReclamationThread();

// A cold-prepared deadline retains its charged storage across arm/cancel.
// Callbacks only notify a weak consumer; takeReady consumes the current arm's
// monitor-published generation, so a late canceled callback grants nothing.
struct KisPageReclamationDelayState;
class KRITAIMAGE_EXPORT KisPageReclamationDelay
{
public:
    KisPageReclamationDelay();
    explicit KisPageReclamationDelay(KisPageReadinessCallback dispatch,
                                    KisBackingBudgetController *storageBudget = nullptr);
    ~KisPageReclamationDelay();
    KisPageReclamationDelay(KisPageReclamationDelay &&) noexcept;
    KisPageReclamationDelay &operator=(KisPageReclamationDelay &&) noexcept;
    KisPageReclamationDelay(const KisPageReclamationDelay &) = delete;
    KisPageReclamationDelay &operator=(const KisPageReclamationDelay &) = delete;
    void reset();
    bool isValid() const;
    bool arm(int delayMs);
    void cancel();
    bool takeReady();
private:
    std::shared_ptr<KisPageReclamationDelayState> d;
};

// Cold-prepared reusable notification. notify() only splices its existing node
// and signals the monitor: no allocation, callback, or task destruction on the
// producer (including a publication install). Dispatch must coalesce its own
// worker job and validate consumer lifetime; reset cancels future dispatches.
struct KisPageReclamationDelayClock;
struct KisPageReclamationWakeState
{
    explicit KisPageReclamationWakeState(
        KisMutationStorageAllocator<KisPageReclamationWakeState> allocator)
        : storage(std::move(allocator)) {}
    std::weak_ptr<KisPageReclamationDelayClock> clock;
    KisPageReadinessCallback dispatch;
    KisPageReclamationWakeState *previous = nullptr;
    KisPageReclamationWakeState *next = nullptr;
    KisMutationStorageAllocator<KisPageReclamationWakeState> storage;
    std::atomic<quint32> references{1};
    bool registered = false;
    bool ready = false;
};
class KRITAIMAGE_EXPORT KisPageReclamationWake
{
public:
    KisPageReclamationWake();
    ~KisPageReclamationWake();
    KisPageReclamationWake(KisPageReclamationWake &&) noexcept;
    KisPageReclamationWake &operator=(KisPageReclamationWake &&) noexcept;
    KisPageReclamationWake(const KisPageReclamationWake &) = delete;
    KisPageReclamationWake &operator=(const KisPageReclamationWake &) = delete;
    bool isValid() const;
    void reset();
    void notify() const noexcept;
private:
    KisPageReclamationWakeState *d = nullptr;
    friend KisPageReclamationWake kisPreparePageStoreReclamationWake(
        KisPageReadinessCallback, KisBackingBudgetController *);
};
KRITAIMAGE_EXPORT KisPageReclamationWake kisPreparePageStoreReclamationWake(
    KisPageReadinessCallback dispatch, KisBackingBudgetController *storageBudget = nullptr);

// Drains immediate work, not future deadlines or physical retirement debt.
// Cold correctness/shutdown endpoint. Requires quiescent producers; never call
// from a reclamation job or an interaction/read-guard destructor.
KRITAIMAGE_EXPORT void kisDrainPageStoreReclamation();
// Same permanent stop used by application teardown. Producers must be
// quiescent; late prepared handles may only release after this endpoint.
KRITAIMAGE_EXPORT void kisStopPageStoreReclamation();

struct KisPageTreeReclamationStatistics
{
    quint64 foregroundNodeDestructions = 0;
    quint64 backgroundNodeDestructions = 0;
    quint64 passes = 0;
    quint64 maximumReferenceDropsPerPass = 0;
};
constexpr size_t KisPageTreeReclamationFixedStorageBytes = 4 * sizeof(std::atomic<quint64>);
KRITAIMAGE_EXPORT KisPageTreeReclamationStatistics kisPageTreeReclamationStatistics();

#endif
