/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPageReadiness_p.h"
#include <QMutex>
#include <QMutexLocker>
#include <boost/intrusive/set.hpp>
#include <limits>

struct KisPageReadinessEntry : boost::intrusive::set_base_hook<>
{
    quint64 id = 0;
    KisPageReadinessCallback callback;
    explicit KisPageReadinessEntry(KisPageReadinessCallback value) : callback(std::move(value)) {}
    static void dispose(KisPageReadinessEntry *entry) noexcept
    {
        auto allocator = entry->callback.storageAllocator<KisPageReadinessEntry>();
        std::destroy_at(entry);
        allocator.deallocate(entry, 1);
    }
};
namespace {
struct EntryLess {
    bool operator()(const KisPageReadinessEntry &a, const KisPageReadinessEntry &b) const { return a.id < b.id; }
    bool operator()(quint64 id, const KisPageReadinessEntry &b) const { return id < b.id; }
    bool operator()(const KisPageReadinessEntry &a, quint64 id) const { return a.id < id; }
};
struct Entries : boost::intrusive::set<KisPageReadinessEntry, boost::intrusive::compare<EntryLess>> {
    ~Entries() { clear_and_dispose([](KisPageReadinessEntry *entry) { KisPageReadinessEntry::dispose(entry); }); }
};
}
struct KisPageReadinessState
{
    explicit KisPageReadinessState(KisPageReadinessCallback value) : emptied(std::move(value)) {}
    QMutex mutex;
    quint64 nextId = 1;
    Entries callbacks;
    const KisPageReadinessCallback emptied;
};

KisPageReadinessSubscription::KisPageReadinessSubscription(KisPageReadinessCallback callback)
{
    if (!callback) return;
    auto allocator = callback.storageAllocator<KisPageReadinessEntry>();
    auto *entry = allocator.allocate(1);
    try { m_prepared = ::new (entry) KisPageReadinessEntry(std::move(callback)); }
    catch (...) { allocator.deallocate(entry, 1); throw; }
}
KisPageReadinessSubscription::~KisPageReadinessSubscription() { reset(); }
KisPageReadinessSubscription::KisPageReadinessSubscription(KisPageReadinessSubscription &&other) noexcept
    : m_state(std::move(other.m_state)), m_prepared(std::exchange(other.m_prepared, nullptr)),
      m_id(std::exchange(other.m_id, 0)) {}
KisPageReadinessSubscription &KisPageReadinessSubscription::operator=(KisPageReadinessSubscription &&other) noexcept
{
    if (this != &other) {
        reset();
        m_state = std::move(other.m_state);
        m_prepared = std::exchange(other.m_prepared, nullptr);
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}
void KisPageReadinessSubscription::reset()
{
    auto state = m_state.lock();
    const quint64 id = std::exchange(m_id, 0);
    m_state.reset();
    auto *cancelled = std::exchange(m_prepared, nullptr);
    bool last = false;
    if (state && id) {
        QMutexLocker lock(&state->mutex);
        auto found = state->callbacks.find(id, EntryLess{});
        if (found != state->callbacks.end()) {
            cancelled = &*found;
            state->callbacks.erase(found);
            last = state->callbacks.empty();
        }
    }
    // No capture destruction or registry cleanup while holding the signal gate.
    if (last && state->emptied) state->emptied(state.get());
    state.reset();
    if (cancelled) KisPageReadinessEntry::dispose(cancelled);
}
KisPageReadinessSignal::KisPageReadinessSignal(KisMutationStorageAllocator<std::byte> storage)
    : KisPageReadinessSignal(nullptr, std::move(storage)) {}
KisPageReadinessSignal::KisPageReadinessSignal(
    KisPageReadinessCallback emptied, KisMutationStorageAllocator<std::byte> storage)
    : m_state(std::allocate_shared<KisPageReadinessState>(
          KisMutationStorageAllocator<KisPageReadinessState>(storage), std::move(emptied))) {}
bool KisPageReadinessSignal::emptyState(const KisPageReadinessState *expected) const
{
    if (m_state.get() != expected) return false;
    QMutexLocker lock(&m_state->mutex);
    return m_state->callbacks.empty();
}
KisPageReadinessSubscription KisPageReadinessSignal::subscribe(KisPageReadinessCallback callback)
{
    KisPageReadinessSubscription prepared(std::move(callback));
    subscribeRetained(prepared);
    return prepared;
}
bool KisPageReadinessSignal::subscribeRetained(KisPageReadinessSubscription &prepared)
{
    QMutexLocker lock(&m_state->mutex);
    if (!prepared.m_prepared || prepared.m_id || !m_state->nextId ||
        m_state->nextId == std::numeric_limits<quint64>::max()) return false;
    auto *entry = prepared.m_prepared;
    entry->id = m_state->nextId++;
    m_state->callbacks.insert(*entry); // prepared node; no allocator/capture call
    prepared.m_state = m_state;
    prepared.m_id = entry->id;
    prepared.m_prepared = nullptr;
    return true;
}
void KisPageReadinessSignal::notify()
{
    Entries callbacks;
    {
        QMutexLocker lock(&m_state->mutex);
        callbacks.swap(m_state->callbacks);
    }
    while (!callbacks.empty()) {
        auto *entry = &*callbacks.begin();
        callbacks.erase(callbacks.begin());
        std::unique_ptr<KisPageReadinessEntry, decltype(&KisPageReadinessEntry::dispose)>
            owned(entry, KisPageReadinessEntry::dispose);
        entry->callback();
    }
}
qsizetype KisPageReadinessSignal::subscriberCount() const
{
    QMutexLocker lock(&m_state->mutex);
    return m_state->callbacks.size();
}
