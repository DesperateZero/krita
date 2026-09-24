/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_BR1_STRESS_PROFILE_H
#define KIS_BR1_STRESS_PROFILE_H

// Opt-in diagnostic instrumentation, outside the production data path.
// KIS_BR1_PROFILE=1: per-worker lock-wait/body wall time and thread CPU time.
// KIS_BR1_SEED=<uint>: reproducible per-worker operation streams, NOT scheduling.
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QReadWriteLock>
#include <array>
#include <chrono>
#include <ctime>
#include <vector>
#include <algorithm>
#include "pagestore/KisPageStoreDiagnostics_p.h"

inline QJsonArray kisBr1StageMetrics(const KisPageStoreDiagnosticRecorder &recorder)
{
    QJsonArray result;
    for (size_t i = 0; i < recorder.metrics().size(); ++i) {
        const auto &metric = recorder.metrics()[i];
        if (!metric.intervals) continue;
        result.append(QJsonObject{
            {"name", KisPageStoreDiagnosticRecorder::phaseName(KisPageStoreDiagnosticPhase(i))},
            {"intervals", double(metric.intervals)},
            {"work_items", double(metric.workItems)},
            {"wall_ms", double(metric.wallNanoseconds) / 1e6},
            {"cpu_ms", recorder.threadCpuClockAvailable()
                ? QJsonValue(double(metric.threadCpuNanoseconds) / 1e6) : QJsonValue(QJsonValue::Null)}
        });
    }
    return result;
}

class KisBr1StressProfile
{
    using Clock = std::chrono::steady_clock;
    struct Metric {
        qint64 wall = 0;
        qint64 cpu = 0;
        qint64 wait = 0;
        std::vector<qint64> durations;
    };
public:
    enum Phase { ReadBytes, WriteBytes, HistoryBegin, HistoryClear,
                 HistoryCommit, Rollback, Rollforward, Purge, PhaseCount };

    KisBr1StressProfile(QReadWriteLock &lock, int worker)
        : m_lock(lock), m_worker(worker),
          m_enabled(qEnvironmentVariableIntValue("KIS_BR1_PROFILE") != 0) {}

    void operation(int type) { m_type = type; }
    void lockForRead() { acquire(false); }
    void lockForWrite() { acquire(true); }
    void unlock() {
        if (m_enabled) {
            const qint64 elapsed = wallNow() - m_bodyStart;
            Metric &metric = m_ops[m_type];
            metric.wall += elapsed;
            metric.cpu += cpuNow() - m_cpuStart;
            metric.durations.push_back(elapsed);
        }
        m_lock.unlock();
    }

    template<class F> void phase(Phase phase, F &&fn) {
        if (!m_enabled) { fn(); return; }
        const qint64 cpuStart = cpuNow();
        const qint64 wallStart = wallNow();
        fn();
        const qint64 elapsed = wallNow() - wallStart;
        Metric &metric = m_phases[phase];
        metric.wall += elapsed;
        metric.cpu += cpuNow() - cpuStart;
        metric.durations.push_back(elapsed);
    }

    void report(quint32 seed) {
        if (!m_enabled) return;
        static const char *opNames[] = {"default_same", "tile_locks_1", "tile_locks_2",
            "extent_query", "clear_rect", "byte_roundtrip", "bitblt_with_source",
            "history_7", "history_8", "has_memento", "clear_all", "set_extent"};
        static const char *phaseNames[] = {"read_bytes", "write_identical_bytes",
            "history_begin", "history_clear", "history_commit", "rollback",
            "rollforward", "purge_history"};
        QJsonArray ops, phases;
        for (size_t i = 0; i < m_ops.size(); ++i) ops.append(json(opNames[i], m_ops[i]));
        for (size_t i = 0; i < m_phases.size(); ++i) phases.append(json(phaseNames[i], m_phases[i]));
        qInfo().noquote() << "BR1_PROFILE" << QJsonDocument(QJsonObject{
            {"worker", m_worker}, {"seed", double(seed)}, {"operations", ops},
            {"phases", phases}, {"cpu_clock_available", cpuNow() >= 0}
        }).toJson(QJsonDocument::Compact);
    }

private:
    static qint64 wallNow() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch()).count();
    }
    static qint64 cpuNow() {
#ifdef CLOCK_THREAD_CPUTIME_ID
        timespec time{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) == 0)
            return qint64(time.tv_sec) * 1000000000 + time.tv_nsec;
#endif
        return -1;
    }
    void acquire(bool exclusive) {
        const qint64 start = m_enabled ? wallNow() : 0;
        if (exclusive) m_lock.lockForWrite();
        else m_lock.lockForRead();
        if (m_enabled) {
            m_bodyStart = wallNow();
            m_ops[m_type].wait += m_bodyStart - start;
            m_cpuStart = cpuNow();
        }
    }
    static QJsonObject json(const char *name, Metric &metric) {
        std::sort(metric.durations.begin(), metric.durations.end());
        const size_t n = metric.durations.size();
        return {{"name", QString::fromLatin1(name)}, {"count", double(n)},
            {"wall_ms", metric.wall / 1e6}, {"cpu_ms", metric.cpu / 1e6},
            {"wait_ms", metric.wait / 1e6},
            {"p95_ms", n ? metric.durations[(n * 95 - 1) / 100] / 1e6 : 0.0}};
    }
    QReadWriteLock &m_lock;
    int m_worker;
    bool m_enabled;
    int m_type = 0;
    qint64 m_bodyStart = 0, m_cpuStart = 0;
    std::array<Metric, 12> m_ops;
    std::array<Metric, PhaseCount> m_phases;
};

#endif
