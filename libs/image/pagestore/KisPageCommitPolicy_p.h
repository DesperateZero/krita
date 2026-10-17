/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_COMMIT_POLICY_P_H
#define KIS_PAGE_COMMIT_POLICY_P_H
#include "KisPageStateMachine.h"

// No ownership, allocation or replica/protection representation. All guards
// precede the first edit callback. The caller owns the input stability window.
template<class Version, class Visit, class Edit>
QString kisPageCommitPolicy(KisImageEpochId publishedEpoch, KisPageGeneration nativeGeneration,
    const KisPageTransition &transition,
    Version *prepared, Version *published, Visit visit, Edit edit)
{
    if (nativeGeneration.isValid())
        return QStringLiteral("page has an unaccepted native write reservation");
    if (!transition.transaction.isValid() || !transition.imageEpoch.isValid()
        || transition.imageEpoch.value <= publishedEpoch.value)
        return QStringLiteral("transaction identity is invalid");
    if (!prepared || prepared->publication != KisPagePublicationState::Prepared
        || !(prepared->preparedBy == transition.transaction))
        return QStringLiteral("transaction page is not prepared");
    bool newer = false;
    visit([&](auto &candidate) {
        if (candidate.publication == KisPagePublicationState::Prepared
            && candidate.preparedBy == transition.transaction
            && candidate.version.generation.value > prepared->version.generation.value)
            newer = true;
    });
    if (newer)
        return QStringLiteral("transaction commit does not select its latest prepared generation");
    if (!published || prepared->version.generation.value <= published->version.generation.value)
        return QStringLiteral("transaction would not advance the published generation");
    edit(*published, KisPagePublicationState::Historical, published->preparedBy);
    visit([&](auto &candidate) {
        if (candidate.publication == KisPagePublicationState::Prepared
            && candidate.preparedBy == transition.transaction
            && !(candidate.version == prepared->version))
            edit(candidate, KisPagePublicationState::Historical, KisPageTransactionId{});
    });
    edit(*prepared, KisPagePublicationState::Published, KisPageTransactionId{});
    return {};
}
#endif
