/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_VIEW_VALUE_P_H
#define KIS_PAGE_VIEW_VALUE_P_H

#include "KisPageMetadataArena_p.h"
#include "KisPageStoreTypes.h"
#include <atomic>

struct KisPagePrivateWrite;

// The identity inside the final content cell. A tree/candidate may hold this
// memory without retaining any pixel authority, published role or read edge.
// Once a cell has an identity, replacing its mutable placement/state must not
// reconstruct this header or reset references held by an immutable tree.
struct KisPageViewValue {
    KisPageVersion version;
    KisVersionSlotId slot;
    // The existing indexed/Frame/read lifetime suppresses pointless last-view
    // callbacks. The shard gate owns this bit; tree references own low bits.
    static constexpr quint64 OriginalLifetime = quint64(1) << 63;
    std::atomic<quint64> viewMemoryState{0};
    // Semantic edges from live immutable nodes, distinct from raw memory
    // copies and candidates. The epoch gate writes; the Store admission gate
    // excludes publication while metadata tests this at its retirement cut.
    std::atomic<quint64> rootEdges{0};
    // The original shard gate owns actual captured/range read edges. They
    // retain identity memory as well as non-default pixel authority; mutable
    // placement snapshots never overwrite this count.
    quint32 readEdges = 0;
    void attachLifetime() noexcept
    { viewMemoryState.fetch_or(OriginalLifetime, std::memory_order_release); }
    bool detachLifetime() noexcept
    { return !(viewMemoryState.fetch_and(~OriginalLifetime, std::memory_order_acq_rel) & ~OriginalLifetime); }
};

// One original storage owner is retained by a tree/selection owner, not by
// every copied node or value pointer. Callers keep this owner alive across
// pin(), borrowed pointer use, and release(). These operations grant memory
// only; the view protocol remains responsible for real semantic live edges.
class KisPageViewValueStorage
{
public:
    virtual ~KisPageViewValueStorage() = default;
    // An existing final Value, including an original Frame-held candidate.
    // Missing/dead identities return null; this never creates pixel content.
    virtual KisPageViewValue *pin(const KisPageVersion &, KisVersionSlotId = {},
                                 const KisPagePrivateWrite *original = nullptr) = 0;
    // Initialization may precede backing registration. This prepares the same
    // final identity that registration must fill, with no replica or read grant.
    virtual KisPageViewValue *prepareInitial(const KisPageVersion &) = 0;
    static void retain(KisPageViewValue *value) noexcept
    {
        // A retained pointer or the original shard gate already protects it.
        const auto before = value->viewMemoryState.fetch_add(1, std::memory_order_relaxed);
        Q_ASSERT((before & ~KisPageViewValue::OriginalLifetime) != ~KisPageViewValue::OriginalLifetime);
        Q_UNUSED(before);
    }
    void release(KisPageViewValue *value) noexcept
    {
        if (!value) return;
        // Copy location BEFORE dropping the last reference. Another lawful
        // terminal consumer can erase the cell before its shard gate is taken;
        // releaseMemory resolves the generational slot, never this pointer.
        const auto key = value->version.key;
        const auto slot = value->slot;
        const auto count = value->viewMemoryState.fetch_sub(1, std::memory_order_acq_rel);
        Q_ASSERT(count & ~KisPageViewValue::OriginalLifetime);
        if (count == 1) releaseMemory(key, slot, value);
    }
private:
    // Native arena storage uses key/slot only: the address may already be
    // dead. An identity-only oracle has no independent terminal consumer and
    // can destroy its original allocation directly at this zero transition.
    virtual void releaseMemory(const KisPageKey &, KisVersionSlotId, KisPageViewValue *) noexcept = 0;
};

#endif
