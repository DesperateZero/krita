/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_STROKE_JOB_FAILURE_CONTEXT_H
#define KIS_STROKE_JOB_FAILURE_CONTEXT_H

#include <QString>
#include "kritaimage_export.h"

// A stack-only bridge for legacy void drawing calls. It owns no transaction,
// pixels, jobs or cancellation callback. The job owner consumes the first error
// after the callback has returned and all operation-local guards have unwound.
class KRITAIMAGE_EXPORT KisStrokeJobFailureContext
{
public:
    explicit KisStrokeJobFailureContext(bool enabled);
    ~KisStrokeJobFailureContext();
    KisStrokeJobFailureContext(const KisStrokeJobFailureContext &) = delete;
    KisStrokeJobFailureContext &operator=(const KisStrokeJobFailureContext &) = delete;

    bool failed() const noexcept { return m_failed; }
    const QString &error() const noexcept { return m_error; }

    static bool reportFailure(const QString &error) noexcept;
    static bool currentJobHasFailed() noexcept;

private:
    static thread_local KisStrokeJobFailureContext *s_current;
    KisStrokeJobFailureContext *m_previous;
    QString m_error;
    bool m_failed = false;
};

#endif
