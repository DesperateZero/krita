/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_RESTORATION_P_H
#define KIS_PAGE_RESTORATION_P_H

#include "KisPageStoreTypes.h"
#include <cstddef>

class KisPagePublicationCoordinator;

// Original root difference, borrowed only through synchronous adapter delivery.
// The caller's manager gate retains table locations; the publication candidate
// retains these versions and their one workspace prefix across metadata retries.
// Locations exist only when a preparation hook binds them; a completion-only
// notification borrows versions without allocating a position column.
class KisPageRestorationChanges final
{
public:
    KisPageRestorationChanges(const KisPageRestorationChanges &) = delete;
    size_t size() const { return m_count; }
    const KisPageVersion &version(size_t index) const { Q_ASSERT(index < m_count); return m_versions[index]; }
    void *&location(size_t index) const { Q_ASSERT(m_locations && index < m_count); return m_locations[index]; }
private:
    friend class KisPagePublicationCoordinator;
    KisPageRestorationChanges(const KisPageVersion *versions, void **locations, size_t count)
        : m_versions(versions), m_locations(locations), m_count(count) {}
    const KisPageVersion *m_versions;
    void **m_locations;
    size_t m_count;
};

#endif // KIS_PAGE_RESTORATION_P_H
