/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisPageStoreIteratorReadScope_p.h"
#include <algorithm>
#include <utility>

KisPageStoreReadPage::KisPageStoreReadPage(KisPageStore *store, const KisCapturedReadView &view,
                                         const KisPageKey &key, QString *error,
                                         KisPagePriority priority, bool tryNative) : m_store(store)
{
    if (!store || !view.isValid()) { KisPageStoreDetail::setError(error, QStringLiteral("read scope is unavailable")); return; }
    KisCpuResidentReadStatus status = KisCpuResidentReadStatus::UnsupportedProvider;
    if (tryNative) m_native = view.readResidentPage(key, &status);
    if (m_native.isValid()) { KisPageStoreDetail::setError(error, {}); return; }
    if (status != KisCpuResidentReadStatus::NonResident &&
        status != KisCpuResidentReadStatus::UnsupportedProvider &&
        status != KisCpuResidentReadStatus::BindingUnavailable) {
        KisPageStoreDetail::setError(error, status == KisCpuResidentReadStatus::Busy
            ? QStringLiteral("CPU resident access is locally busy; retry after the writer releases")
            : QStringLiteral("resident read rejected (status %1)").arg(int(status)));
        return;
    }
    const KisPageAccessRequirement cpu{KisPageAccessDomain::CpuRam, KisPageAccessKind::CpuPointer};
    const auto request = store->acquireReadInView(key, view, cpu, priority);
    if (!request.isValid()) { KisPageStoreDetail::setError(error, request.error); return; }
    m_lease.emplace(store->resolve(request, request.readiness));
    if (!m_lease->isValid()) {
        store->cancel(request);
        KisPageStoreDetail::setError(error, QStringLiteral("control read could not resolve captured version"));
        return;
    }
    KisPageStoreDetail::setError(error, {});
}

KisPageStoreReadPage::~KisPageStoreReadPage() { reset(); }
KisPageStoreReadPage &KisPageStoreReadPage::operator=(KisPageStoreReadPage &&other) noexcept
{
    if (this != &other) {
        reset();
        m_store = std::exchange(other.m_store, nullptr);
        m_native = std::move(other.m_native);
        if (other.m_lease) { m_lease.emplace(std::move(*other.m_lease)); other.m_lease.reset(); }
    }
    return *this;
}
void KisPageStoreReadPage::reset()
{
    if (m_lease && m_lease->isValid()) m_store->release(std::move(*m_lease));
    m_lease.reset(); m_native = {}; m_store = nullptr;
}
const quint8 *KisPageStoreReadPage::data() const
{ return static_cast<const quint8 *>(m_native.isValid() ? m_native.data() : m_lease ? m_lease->cpuData() : nullptr); }
quint32 KisPageStoreReadPage::rowStride() const
{ return m_native.isValid() ? m_native.rowStride() : m_lease ? m_lease->rowStride() : 0; }
KisPageVersion KisPageStoreReadPage::version() const
{ return m_native.isValid() ? m_native.version() : m_lease ? m_lease->version() : KisPageVersion{}; }

bool KisPageStoreIteratorReadScope::isValid() const
{ return m_store && m_surface.isValid() && (m_beforeOnly || m_current.isValid()); }
bool KisPageStoreIteratorReadScope::beforeMatches(const KisPageVersion &version) const
{
    if (!m_before.isValid()) return true;
    KisPageVersion before;
    return m_before.resolvePageVersion(version.key, &before) && before == version;
}
KisPageStoreReadPage KisPageStoreIteratorReadScope::readPage(
    qint32 column, qint32 row, bool before, QString *error) const
{
    if (!isValid() || beforeAliasesWrite() || (m_beforeOnly && !before)) {
        KisPageStoreDetail::setError(error, QStringLiteral("iterator read visibility is unavailable")); return {};
    }
    return {m_store, before && m_before.isValid() ? m_before : m_current,
            {m_surface, {column, row}}, error};
}
KisPageStoreReadCursor::KisPageStoreReadCursor(QSharedPointer<const KisPageStoreIteratorReadScope> scope)
    : m_scope(std::move(scope)) {}
KisPageStoreReadPair KisPageStoreReadCursor::read(qint32 column, qint32 row, QString *error)
{
    if (!m_scope || !m_scope->isValid() || m_scope->beforeOnly()) {
        KisPageStoreDetail::setError(error, QStringLiteral("read cursor needs a current/before scope")); return {};
    }
    size_t position = 0;
    while (position < Capacity) {
        const auto &e = m_entries[m_order[position]];
        if (e.current.data() && e.column == column && e.row == row) break;
        ++position;
    }
    if (position == Capacity) {
        position = Capacity - 1;
        auto &e = m_entries[m_order[position]];
        e.current.reset(); e.before.reset();
        e.current = m_scope->readPage(column, row, false, error);
        if (!e.current.data()) return {};
        if (!m_scope->beforeMatches(e.current.version())) {
            e.before = m_scope->readPage(column, row, true, error);
            if (!e.before.data()) { e.current.reset(); return {}; }
        }
        e.column = column; e.row = row;
    }
    const auto index = m_order[position];
    std::move_backward(m_order.begin(), m_order.begin() + position, m_order.begin() + position + 1);
    m_order[0] = index;
    const auto &e = m_entries[index];
    KisPageStoreDetail::setError(error, {});
    return {e.current.data(), e.before.data() ? e.before.data() : e.current.data(),
            e.current.rowStride(), e.before.data() ? e.before.rowStride() : e.current.rowStride()};
}
