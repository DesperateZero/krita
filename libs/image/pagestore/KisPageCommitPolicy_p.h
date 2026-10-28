/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_COMMIT_POLICY_P_H
#define KIS_PAGE_COMMIT_POLICY_P_H
#include "KisPageStateMachine.h"

// Only these two facts belong to commit policy. Production acceptance need
// not construct a complete generic transition to pass them between owners.
struct KisPageCommitFacts {
    KisPageTransactionId transaction;
    KisImageEpochId imageEpoch;
};

// No ownership/allocation/replica projection. Validation and edits may be
// separated only by the caller's complete original-page claim.
template<class Prepared, class Published, class Transition, class Visit>
QString kisPageValidateCommit(KisImageEpochId publishedEpoch, KisPageGeneration nativeGeneration,
    const Transition &transition, Prepared *prepared, Published *published, Visit visit)
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
    return {};
}

template<class Prepared, class Published, class Transition, class Visit, class Edit>
void kisPageApplyCommit(const Transition &transition, Prepared *prepared, Published *published,
                        Visit visit, Edit edit)
{
    edit(*published, KisPagePublicationState::Historical, published->preparedBy);
    visit([&](auto &candidate) {
        if (candidate.publication == KisPagePublicationState::Prepared
            && candidate.preparedBy == transition.transaction
            && !(candidate.version == prepared->version))
            edit(candidate, KisPagePublicationState::Historical, KisPageTransactionId{});
    });
    edit(*prepared, KisPagePublicationState::Published, KisPageTransactionId{});
}

template<class Prepared, class Published, class Transition, class Visit, class Edit>
QString kisPageCommitPolicy(KisImageEpochId publishedEpoch, KisPageGeneration nativeGeneration,
    const Transition &transition, Prepared *prepared, Published *published, Visit visit, Edit edit)
{
    const auto failure = kisPageValidateCommit(publishedEpoch, nativeGeneration,
        transition, prepared, published, visit);
    if (!failure.isEmpty()) return failure;
    kisPageApplyCommit(transition, prepared, published, visit, edit);
    return {};
}
#endif
