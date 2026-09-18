/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPageStateMachine.h"

KisPageStateMachine::KisPageStateMachine() = default;
KisPageStateMachine::~KisPageStateMachine() = default;

KisPageTransitionResult KisPageStateMachine::apply(
    const KisPageStateSnapshot &current,
    const KisPageTransition &transition) const
{
    Q_UNUSED(transition);

    KisPageTransitionResult result;
    result.next = current;
    result.rejectionReason = QStringLiteral("BR1 PageStore state machine is not implemented");
    // TODO(BR1): use this exact transition function in both reference and production paths.
    return result;
}

bool KisPageStateMachine::validateInvariants(const KisPageStateSnapshot &state,
                                             QString *failureReason) const
{
    Q_UNUSED(state);
    if (failureReason) {
        *failureReason = QStringLiteral("BR1 invariant validator is not implemented");
    }
    // TODO(BR1): validate writer, publication, authority, lease and replica rules.
    return false;
}
