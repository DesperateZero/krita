/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_VIEW_TEST_ACCESS_H
#define KIS_PAGE_VIEW_TEST_ACCESS_H
#include "KisImageEpochReferenceModel.h"
#include "KisPageMetadataCoordinator.h"
#include "KisPageViewValue_p.h"
#include <limits>
#include <utility>

// Composed owner fixtures use the production Value domain explicitly. Pure
// epoch reference tests continue to use the independent identity-only oracle.
class KisPageViewTestAccess
{
public:
    static bool initialize(KisImageEpochReferenceModel &epochs, KisPageMetadataCoordinator &metadata,
                           const KisImageEpochSnapshot &initial, QString *error)
    {
        auto prepared = epochs.prepareInitialization(initial, error, &metadata);
        return prepared && epochs.installInitialization(std::move(prepared));
    }
    static auto values(KisPageMetadataCoordinator &metadata) { return metadata.viewValues(); }

    // Only an owner fixture: exercise the actual original-Value protocol.
    // No token registry or second protection policy. The facade outlives this
    // edge; public Store capture/core lifetimes are tested through Store.
    class ReadEdge
    {
    public:
        ReadEdge() = default;
        ReadEdge(const ReadEdge &) = delete;
        ReadEdge &operator=(const ReadEdge &) = delete;
        ReadEdge(ReadEdge &&other) noexcept { *this = std::move(other); }
        ReadEdge &operator=(ReadEdge &&other) noexcept
        {
            if (this != &other) {
                reset();
                metadata = std::exchange(other.metadata, nullptr);
                value = std::exchange(other.value, nullptr);
            }
            return *this;
        }
        ~ReadEdge() { reset(); }
        explicit operator bool() const { return value; }
        bool reset()
        {
            if (!value) return false;
            KisPageViewTestAccess::drop(*metadata, value);
            metadata = nullptr;
            return true;
        }
    private:
        friend class KisPageViewTestAccess;
        KisPageMetadataCoordinator *metadata = nullptr;
        KisPageViewValue *value = nullptr;
    };
    static ReadEdge readEdge(KisPageMetadataCoordinator &metadata, const KisPageVersion &identity)
    {
        ReadEdge result;
        const auto source = metadata.viewValues();
        auto *value = source ? source->pin(identity) : nullptr;
        if (!value) return result;
        const quint32 end = std::numeric_limits<quint32>::max();
        size_t next = 0;
        if (metadata.retainReadValues(&value, &end, next, 1) == 1) {
            result.metadata = &metadata;
            result.value = value;
        }
        source->release(value);
        return result;
    }
private:
    static void drop(KisPageMetadataCoordinator &metadata, KisPageViewValue *&value)
    {
        const quint32 end = std::numeric_limits<quint32>::max();
        size_t next = 0;
        KisPageKey revisit;
        const auto released = metadata.releaseReadValues(&value, &end, next, 1, &revisit);
        Q_ASSERT(released == 1 && !value);
        Q_UNUSED(released);
    }
};
#endif
