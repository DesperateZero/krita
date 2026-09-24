/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisPageStoreDiagnostics_p.h"

#include <chrono>
#include <ctime>

namespace {
quint64 wallNow()
{
    return quint64(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool cpuNow(quint64 *value)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    timespec time;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0) {
        *value = quint64(time.tv_sec) * 1000000000ULL + quint64(time.tv_nsec);
        return true;
    }
#endif
    *value = 0;
    return false;
}
}

thread_local KisPageStoreDiagnosticRecorder *
    KisPageStoreDiagnosticRecorder::s_current = nullptr;

KisPageStoreDiagnosticRecorder::KisPageStoreDiagnosticRecorder(
    bool enabled, const KisPageStore *owner)
    : m_owner(owner)
{
    if (!enabled) return;
    quint64 unused = 0;
    m_cpuAvailable = cpuNow(&unused);
    m_previous = s_current;
    s_current = this;
}

KisPageStoreDiagnosticRecorder::~KisPageStoreDiagnosticRecorder()
{
    if (s_current != this) return;
    Q_ASSERT(m_activeTimers == 0);
    s_current = m_previous;
}

bool KisPageStoreDiagnosticRecorder::setRecording(bool recording)
{
    if (s_current != this || m_activeTimers != 0) return false;
    m_recording = recording;
    return true;
}

void KisPageStoreDiagnosticRecorder::setPhaseObserver(
    std::function<void(KisPageStoreDiagnosticPhase)> observer)
{
    Q_ASSERT(m_activeTimers == 0);
    m_phaseObserver = std::move(observer);
}

const char *KisPageStoreDiagnosticRecorder::phaseName(KisPageStoreDiagnosticPhase phase)
{
    static const char *const names[] = {
        "commit_owner_wait", "commit_proof_validation", "commit_default_removal_prepare",
        "commit_transition_preflight", "commit_epoch_prepare", "commit_completion",
        "commit_root", "commit_metadata_apply", "commit_root_publication", "commit_cleanup", "commit_history_collect",
        "commit_provider_retire", "transaction_begin_owner_wait", "transaction_begin",
        "manifest_export_owner_wait", "manifest_export",
        "write_bytes_owner_wait", "write_bytes_batch_begin", "write_bytes_body", "write_bytes_batch_finish",
        "write_acquire", "write_resolve", "write_publish_host",
        "write_provider_prepare", "write_provider_transfer",
        "commit_provider_validation", "commit_epoch_delta_prepare", "commit_publish_owner_wait",
        "commit_transition_inputs", "commit_publication_revalidate",
        "restore_transition_preflight", "restore_publish_owner_wait", "restore_publication",
        "commit_input_owner_wait", "read_bytes_capture", "read_bytes_page", "read_bytes_release", "read_planar_page",
        "read_planar_capture", "read_planar_release", "write_writable_pin", "write_pending_materialize",
        "mutation_alias_prepare", "mutation_source_initialize", "fill_page", "copy_source_page", "mutation_index_refresh",
        "copy_prepare", "copy_finish",
        "mutation_seal_owner_wait", "mutation_seal_inputs", "mutation_seal_private_publish",
        "mutation_seal_proof_prepare", "mutation_seal_metadata_prepare", "mutation_seal_publish_owner_wait",
        "mutation_seal_install", "mutation_seal_cleanup",
        "painter_source_resolve",
        "pixel_operation_range_prepare", "pixel_operation_range_wait", "pixel_operation_body", "pixel_operation_finish",
        "packed_write_capture", "packed_write_compare", "packed_write_prepare", "packed_write_copy", "packed_write_release",
        "mutation_payload_copy"
    };
    static_assert(sizeof(names) / sizeof(names[0]) ==
                  size_t(KisPageStoreDiagnosticPhase::Count));
    const auto index = size_t(phase);
    return index < size_t(KisPageStoreDiagnosticPhase::Count) ? names[index] : "invalid";
}

KisPageStoreDiagnosticTimer::KisPageStoreDiagnosticTimer(
    const KisPageStore *owner, KisPageStoreDiagnosticPhase phase, quint64 workItems)
    : m_phase(phase), m_workItems(workItems)
{
    auto *recorder = KisPageStoreDiagnosticRecorder::s_current;
    if (recorder && recorder->m_recording && (!recorder->m_owner || recorder->m_owner == owner)) {
        m_recorder = recorder;
        ++m_recorder->m_activeTimers;
        start();
    }
}

KisPageStoreDiagnosticTimer::~KisPageStoreDiagnosticTimer()
{
    finish();
    if (m_recorder) --m_recorder->m_activeTimers;
}

void KisPageStoreDiagnosticTimer::start()
{
    if (!m_recorder) return;
    // Endpoint ordering is fixed. Clock overhead remains in the independent
    // profiled E; short intervals can have CPU slightly greater than wall.
    if (m_recorder->m_cpuAvailable && !cpuNow(&m_cpuStart)) {
        m_recorder->m_cpuAvailable = false;
    }
    m_wallStart = wallNow();
}

void KisPageStoreDiagnosticTimer::finish()
{
    if (!m_recorder) return;
    const quint64 wallEnd = wallNow();
    quint64 cpuEnd = 0;
    const bool hasCpu = m_recorder->m_cpuAvailable && cpuNow(&cpuEnd);
    if (!hasCpu) m_recorder->m_cpuAvailable = false;
    auto &metric = m_recorder->m_metrics[size_t(m_phase)];
    ++metric.intervals;
    metric.workItems += m_workItems;
    metric.wallNanoseconds += wallEnd - m_wallStart;
    if (hasCpu) metric.threadCpuNanoseconds += cpuEnd - m_cpuStart;
}

void KisPageStoreDiagnosticTimer::next(KisPageStoreDiagnosticPhase phase, quint64 workItems)
{
    if (!m_recorder) return;
    finish();
    m_phase = phase;
    m_workItems = workItems;
    start();
    if (m_recorder->m_phaseObserver && !m_recorder->m_notifying) {
        m_recorder->m_notifying = true;
        m_recorder->m_phaseObserver(phase);
        m_recorder->m_notifying = false;
    }
}
