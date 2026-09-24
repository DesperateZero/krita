/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_COMPLETION_REGISTRY_H
#define KIS_COMPLETION_REGISTRY_H

#include <QScopedPointer>
#include "KisPageStoreTypes.h"

enum class KisCompletionStatus : quint8 {
    Unknown,
    Pending,
    Succeeded,
    Failed,
    Cancelled
};

/** Cold diagnostic snapshot under the registry lock, not a hot-path counter. */
struct KRITAIMAGE_EXPORT KisCompletionSourceStatistics
{
    bool knownSource = false;
    quint64 allocatedTickets = 0;
    quint64 pendingTickets = 0;
    quint64 terminalTickets = 0;
    // Actual lookup records, not bytes or the number of logical tickets.
    quint64 storageRecords = 0;
};

/** Terminal completion evidence minted only by KisCompletionRegistry. */
class KRITAIMAGE_EXPORT KisVerifiedCompletion
{
public:
    KisVerifiedCompletion() = default;

    bool isValid() const
    {
        return m_ticket.isValid() &&
               (m_status == KisCompletionStatus::Succeeded ||
                m_status == KisCompletionStatus::Failed ||
                m_status == KisCompletionStatus::Cancelled);
    }
    KisCompletionTicket ticket() const { return m_ticket; }
    KisCompletionStatus status() const { return m_status; }
    bool succeeded() const { return m_status == KisCompletionStatus::Succeeded; }

private:
    KisVerifiedCompletion(const KisCompletionTicket &ticket,
                          KisCompletionStatus status)
        : m_ticket(ticket)
        , m_status(status)
    {
    }

    KisCompletionTicket m_ticket;
    KisCompletionStatus m_status = KisCompletionStatus::Unknown;

    friend class KisCompletionRegistry;
};

/**
 * Bridge for GPU timeline, CPU job, I/O and host-logical completions. Ticket
 * values are qualified by this registry instance and can never expose native
 * fences or alias an equally numbered ticket from another session registry.
 */
class KRITAIMAGE_EXPORT KisCompletionRegistry
{
public:
    KisCompletionRegistry();
    ~KisCompletionRegistry();

    bool isOperational() const;
    quint64 registerSource(KisCompletionDomain domain);
    KisCompletionTicket allocatePending(quint64 source);
    bool complete(const KisCompletionTicket &ticket, KisCompletionStatus status);
    KisCompletionStatus status(const KisCompletionTicket &ticket) const;
    KisVerifiedCompletion verifyTerminal(const KisCompletionTicket &ticket) const;

    KisCompletionSourceStatistics sourceStatistics(quint64 source) const;

private:
    class Private;
    QScopedPointer<Private> d;
};

#endif // KIS_COMPLETION_REGISTRY_H
