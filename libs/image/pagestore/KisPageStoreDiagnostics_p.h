/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_STORE_DIAGNOSTICS_P_H
#define KIS_PAGE_STORE_DIAGNOSTICS_P_H

#include <array>
#include <functional>
#include <QtGlobal>
#include "kritaimage_export.h"

class KisPageStore;

// Cold structural inspection only. No per-access global counter and no
// change to the public SessionStats return layout used by old fixtures.
struct KisPageMetadataMetrics;
KRITAIMAGE_EXPORT KisPageMetadataMetrics kisPageStoreMetadataMetrics(const KisPageStore &store);

/** Private opt-in diagnostics, not a PageStore access/publication capability.
 * A recorder belongs to one thread and must outlive every timer it captures.
 * No recorder: no clocks, allocations, atomics, or diagnostic locks. The
 * disabled TLS lookup/branch still needs a plain-run perturbation check.
 */
enum class KisPageStoreDiagnosticPhase : quint8 {
    CommitOwnerWait,
    CommitProofValidation,
    CommitDefaultRemovalPreparation,
    CommitTransitionPreflight,
    CommitEpochPrepare,
    CommitCompletion,
    CommitRoot,
    CommitMetadataApply,
    CommitRootPublication,
    CommitCleanup,
    CommitHistoryCollect,
    CommitProviderRetire,
    TransactionBeginOwnerWait,
    TransactionBegin,
    ManifestExportOwnerWait,
    ManifestExport,
    WriteBytesOwnerWait,
    WriteBytesBatchBegin,
    WriteBytesBody,
    WriteBytesBatchFinish,
    WriteAcquire,
    WriteResolve,
    WritePublishHost,
    WriteProviderPrepare,
    WriteProviderTransfer,
    CommitProviderValidation,
    CommitEpochDeltaPrepare,
    CommitPublishOwnerWait,
    CommitTransitionInputs,
    CommitPublicationRevalidate,
    RestoreTransitionPreflight,
    RestorePublishOwnerWait,
    RestorePublication,
    CommitInputOwnerWait,
    ReadBytesCapture,
    ReadBytesPage,
    ReadBytesRelease,
    ReadPlanarPage,
    ReadPlanarCapture,
    ReadPlanarRelease,
    WriteWritablePin,
    WritePendingMaterialize,
    // Entry observes no owner gate; alias/source scopes may still hold the
    // thread-confined mutation mutex. Do not re-enter that same mutation.
    MutationAliasPrepare,
    MutationSourceInitialize,
    FillPage,
    CopySourcePage,
    MutationIndexRefresh,
    CopyPrepare,
    CopyFinish,
    MutationSealOwnerWait,
    MutationSealInputs,
    // Private publish/proof/metadata preparation, publish-owner wait and
    // cleanup enter without owner. The mutation mutex stays held throughout;
    // observers must never re-enter the same mutation.
    MutationSealPrivatePublish,
    MutationSealProofPrepare,
    MutationSealMetadataPrepare,
    MutationSealPublishOwnerWait,
    MutationSealInstall,
    MutationSealCleanup,
    // Unfiltered product recorder only: source/destination/mask have different
    // owners. Entry precedes accessor movement and holds no PageStore owner gate.
    PainterSourceResolve,
    PixelOperationRangePrepare,
    // Backend registry mutex held on entry; observer may signal test latches
    // only, never call back into the owner/backend. wait releases that mutex.
    PixelOperationRangeWait,
    PixelOperationBody,
    PixelOperationFinish,
    // Packed native body only. Entries hold no PageStore/backend registry
    // gate or DataManager operation lock. Target range claims remain live.
    // Compare includes exact read resolution/pin; Prepare includes COW/pin.
    PackedWriteCapture,
    PackedWriteCompare,
    PackedWritePrepare,
    PackedWriteCopy,
    PackedWriteRelease,
    // Copy only into already acquired pending/fallback storage. Direct fresh
    // payload initialization is inside WriteProviderPrepare (allocator + copy).
    MutationPayloadCopy,
    Count
};

struct KisPageStoreDiagnosticMetric {
    quint64 intervals = 0;
    quint64 workItems = 0;
    quint64 wallNanoseconds = 0;
    quint64 threadCpuNanoseconds = 0;
};

class KRITAIMAGE_EXPORT KisPageStoreDiagnosticRecorder
{
public:
    explicit KisPageStoreDiagnosticRecorder(bool enabled,
                                            const KisPageStore *owner = nullptr);
    ~KisPageStoreDiagnosticRecorder();
    KisPageStoreDiagnosticRecorder(const KisPageStoreDiagnosticRecorder &) = delete;
    KisPageStoreDiagnosticRecorder &operator=(const KisPageStoreDiagnosticRecorder &) = delete;

    using Metrics = std::array<KisPageStoreDiagnosticMetric,
        size_t(KisPageStoreDiagnosticPhase::Count)>;
    const Metrics &metrics() const { return m_metrics; }
    bool threadCpuClockAvailable() const { return m_cpuAvailable; }
    // Sample entire operations, including nested timers, not individual
    // phases. May only change between operations with no live timer.
    bool setRecording(bool recording);
    static const char *phaseName(KisPageStoreDiagnosticPhase phase);
    // Test-only scheduling observation, on explicit timer.next() boundaries
    // (not timer construction). Does not grant owner authority or
    // modify a candidate. Runs on the recorder's thread; nested notifications
    // are suppressed. Runs with the phase's documented locking context, so a
    // test may re-enter PageStore only at an explicitly unlocked phase.
    void setPhaseObserver(std::function<void(KisPageStoreDiagnosticPhase)> observer);

private:
    friend class KisPageStoreDiagnosticTimer;
    static thread_local KisPageStoreDiagnosticRecorder *s_current;
    KisPageStoreDiagnosticRecorder *m_previous = nullptr;
    const KisPageStore *m_owner = nullptr;
    bool m_cpuAvailable = false;
    bool m_recording = true;
    quint64 m_activeTimers = 0;
    Metrics m_metrics{};
    std::function<void(KisPageStoreDiagnosticPhase)> m_phaseObserver;
    bool m_notifying = false;
};

/** Sequential, non-overlapping phases within one commit/adoption invocation.
 * Wall includes internal waits; thread CPU excludes sleeping. These totals
 * are not product E or additive across concurrent workers. Nested invocations
 * are inclusive and must not be added to their parent operation's elapsed time.
 */
class KRITAIMAGE_EXPORT KisPageStoreDiagnosticTimer
{
public:
    KisPageStoreDiagnosticTimer(const KisPageStore *owner,
                               KisPageStoreDiagnosticPhase phase,
                               quint64 workItems = 0);
    ~KisPageStoreDiagnosticTimer();
    KisPageStoreDiagnosticTimer(const KisPageStoreDiagnosticTimer &) = delete;
    KisPageStoreDiagnosticTimer &operator=(const KisPageStoreDiagnosticTimer &) = delete;
    void next(KisPageStoreDiagnosticPhase phase, quint64 workItems = 0);

private:
    void finish();
    void start();
    KisPageStoreDiagnosticRecorder *m_recorder = nullptr;
    KisPageStoreDiagnosticPhase m_phase;
    quint64 m_workItems = 0;
    quint64 m_wallStart = 0;
    quint64 m_cpuStart = 0;
};

#endif
