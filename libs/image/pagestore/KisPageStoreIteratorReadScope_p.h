/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_STORE_ITERATOR_READ_SCOPE_P_H
#define KIS_PAGE_STORE_ITERATOR_READ_SCOPE_P_H

#include "KisPageStore.h"
#include <array>
#include <optional>

// One stack-owned exact CPU access. Generic leases exist only for a real
// residency/capability fallback. The owning facade must outlive this object.
class KRITAIMAGE_EXPORT KisPageStoreReadPage
{
public:
    KisPageStoreReadPage() = default;
    KisPageStoreReadPage(KisPageStore *store, const KisCapturedReadView &view,
                         const KisPageKey &key, QString *error = nullptr,
                         KisPagePriority priority = KisPagePriority::Normal, bool tryNative = true);
    ~KisPageStoreReadPage();
    KisPageStoreReadPage(KisPageStoreReadPage &&) noexcept = default;
    KisPageStoreReadPage &operator=(KisPageStoreReadPage &&) noexcept;
    KisPageStoreReadPage(const KisPageStoreReadPage &) = delete;
    KisPageStoreReadPage &operator=(const KisPageStoreReadPage &) = delete;
    void reset();
    const quint8 *data() const;
    quint32 rowStride() const;
    quint64 byteSize() const
    {
        return m_native.isValid() ? m_native.byteSize()
                                  : m_lease ? m_lease->byteSize() : 0;
    }
    KisPageVersion version() const;
    const KisCpuReadGuard &nativeGuard() const { return m_native; }
    const KisReadLease *genericLease() const { return m_lease ? &*m_lease : nullptr; }
private:
    KisPageStore *m_store = nullptr;
    KisCpuReadGuard m_native;
    std::optional<KisReadLease> m_lease;
};

// Immutable current/before selection for one business traversal. Wrapped
// sub-iterators share this selection, never mutable cursors or physical pins.
// Writable traversal captures before only; current writes keep their existing
// mutation/publication boundaries. No hidden refresh at cache eviction/reset.
class KRITAIMAGE_EXPORT KisPageStoreIteratorReadScope
{
public:
    bool isValid() const;
    bool beforeOnly() const { return m_beforeOnly; }
    bool beforeAliasesWrite() const
    { return m_store && m_beforeOnly && !m_before.isValid(); }
    // Live legacy writer pointers cannot be revoked by a read constructor.
    // Such a traversal still uses tiles3 and is not an immutable epoch view.
    bool observesLiveLegacyWriter() const
    { return m_store && !m_beforeOnly && !m_current.isValid() && !m_before.isValid(); }
    bool beforeMatches(const KisPageVersion &version) const;
    KisPageStoreReadPage readPage(qint32 column, qint32 row, bool before,
                                  QString *error = nullptr) const;
private:
    friend class KisTiledDataManagerPageStoreBackend;
    KisPageStore *m_store = nullptr;
    KisSurfaceId m_surface;
    bool m_beforeOnly = false;
    KisCapturedReadView m_current;
    KisCapturedReadView m_before;
};

struct KisPageStoreReadPair
{
    const quint8 *current = nullptr;
    const quint8 *before = nullptr;
    quint32 currentStride = 0;
    quint32 beforeStride = 0;
    bool isValid() const { return current && before && currentStride && beforeStride; }
};

// Fixed-size hot pin set, independent of image/traversal dimensions. Cache
// eviction releases access, not the operation's logical visibility protection.
// Each cursor is thread-confined; the immutable scope may be shared by peers.
class KRITAIMAGE_EXPORT KisPageStoreReadCursor
{
public:
    explicit KisPageStoreReadCursor(QSharedPointer<const KisPageStoreIteratorReadScope> scope);
    KisPageStoreReadPair read(qint32 column, qint32 row, QString *error = nullptr);
private:
    static constexpr size_t Capacity = 4;
    struct Entry {
        qint32 column = 0, row = 0;
        KisPageStoreReadPage current, before;
    };
    QSharedPointer<const KisPageStoreIteratorReadScope> m_scope;
    std::array<Entry, Capacity> m_entries;
    std::array<size_t, Capacity> m_order{{0, 1, 2, 3}};
};

#endif
