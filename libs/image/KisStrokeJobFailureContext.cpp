/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisStrokeJobFailureContext.h"

thread_local KisStrokeJobFailureContext *KisStrokeJobFailureContext::s_current = nullptr;

KisStrokeJobFailureContext::KisStrokeJobFailureContext(bool enabled)
    : m_previous(s_current)
{
    // A nested, unmigrated job must not inherit another job's failure policy.
    s_current = enabled ? this : nullptr;
}

KisStrokeJobFailureContext::~KisStrokeJobFailureContext()
{
    s_current = m_previous;
}

bool KisStrokeJobFailureContext::reportFailure(const QString &error) noexcept
{
    if (!s_current) return false;
    if (!s_current->m_failed) {
        s_current->m_error = error;
        s_current->m_failed = true;
    }
    return true;
}

bool KisStrokeJobFailureContext::currentJobHasFailed() noexcept
{
    return s_current && s_current->m_failed;
}
