/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_READINESS_P_H
#define KIS_PAGE_READINESS_P_H

#include "KisMutationStorage_p.h"
#include <memory>
#include <kritaimage_export.h>

struct KisPageReadinessState;
struct KisPageReadinessEntry;

enum class KisPageReadinessStatus : quint8 { Unavailable, Ready, Waiting };

// The actual immutable notification capture. Copies only retain its prepared
// storage; dispatch never copies a std::function or allocates another capture.
// This holds accounting/weak consumer identities, never page authority.
class KisPageReadinessCallback
{
    struct Data {
        std::atomic<quint32> references{1};
        void (*invoke)(Data *, const KisPageReadinessState *);
        void (*destroy)(Data *) noexcept;
        Data *(*copy)(Data *, KisMutationStorageAllocator<std::byte>);
        KisMutationStorageAllocator<std::byte> storage;
        size_t bytes = 0;
        explicit Data(KisMutationStorageAllocator<std::byte> allocator)
            : storage(std::move(allocator)) {}
    };
    template<class Function> struct Capture final : Data {
        Function function;
        template<class Value>
        Capture(Value &&value, KisMutationStorageAllocator<std::byte> allocator)
            : Data(std::move(allocator)), function(std::forward<Value>(value))
        {
            this->invoke = [](Data *data, const KisPageReadinessState *state) {
                auto &function = static_cast<Capture *>(data)->function;
                if constexpr (std::is_invocable_v<Function &, const KisPageReadinessState *>) function(state);
                else function();
            };
            this->destroy = [](Data *data) noexcept {
                auto *capture = static_cast<Capture *>(data);
                auto allocator = KisMutationStorageAllocator<Capture>(capture->storage);
                std::destroy_at(capture);
                allocator.deallocate(capture, 1);
            };
            this->copy = [](Data *data, KisMutationStorageAllocator<std::byte> storage) -> Data * {
                return create(static_cast<Capture *>(data)->function, std::move(storage));
            };
        }
    };
    template<class Function>
    static Data *create(Function &&function, KisMutationStorageAllocator<std::byte> storage)
    {
        using Value = Capture<std::decay_t<Function>>;
        auto allocator = KisMutationStorageAllocator<Value>(storage);
        auto *data = allocator.allocate(1);
        try {
            auto *value = ::new (data) Value(std::forward<Function>(function), std::move(storage));
            value->bytes = kisPageStorageBytes(value, alignof(Value));
            return value;
        }
        catch (...) { allocator.deallocate(data, 1); throw; }
    }
public:
    KisPageReadinessCallback() = default;
    KisPageReadinessCallback(std::nullptr_t) noexcept {}
    template<class Function, std::enable_if_t<!std::is_same_v<std::decay_t<Function>, KisPageReadinessCallback>
        && (std::is_invocable_v<Function &> || std::is_invocable_v<Function &, const KisPageReadinessState *>), int> = 0>
    KisPageReadinessCallback(Function &&function, KisBackingBudgetController *budget = nullptr)
        : m_data(create(std::forward<Function>(function),
            KisMutationStorageAllocator<std::byte>::retained(budget))) {}
    template<class Function>
    static KisPageReadinessCallback prepare(Function &&function, KisMutationStorageAllocator<std::byte> storage)
    {
        KisPageReadinessCallback result;
        result.m_data = create(std::forward<Function>(function), std::move(storage));
        return result;
    }
    KisPageReadinessCallback(const KisPageReadinessCallback &other) noexcept : m_data(other.m_data)
    { if (m_data) m_data->references.fetch_add(1, std::memory_order_relaxed); }
    KisPageReadinessCallback(KisPageReadinessCallback &&other) noexcept : m_data(std::exchange(other.m_data, nullptr)) {}
    KisPageReadinessCallback &operator=(KisPageReadinessCallback other) noexcept { swap(other); return *this; }
    ~KisPageReadinessCallback()
    { if (m_data && m_data->references.fetch_sub(1, std::memory_order_acq_rel) == 1) m_data->destroy(m_data); }
    void swap(KisPageReadinessCallback &other) noexcept { std::swap(m_data, other.m_data); }
    explicit operator bool() const noexcept { return m_data; }
    void operator()(const KisPageReadinessState *state = nullptr) const { m_data->invoke(m_data, state); }
    // Only an unsponsored external callable is copied during cold admission.
    // An already funded capture preserves its original accounting owner.
    void fund(KisBackingBudgetController *budget)
    {
        if (!m_data || !budget || m_data->storage.budget) return;
        KisPageReadinessCallback prepared;
        prepared.m_data = m_data->copy(m_data, KisMutationStorageAllocator<std::byte>::retained(budget));
        swap(prepared);
    }
    template<class Function> static size_t storageBytesFor() noexcept
    {
        using Value = Capture<std::decay_t<Function>>;
        return kisPageStorageAllocationBytes(sizeof(Value), alignof(Value));
    }
    size_t storageBytes() const noexcept { return m_data ? m_data->bytes : 0; }
    template<class T> KisMutationStorageAllocator<T> storageAllocator() const noexcept
    { return m_data ? KisMutationStorageAllocator<T>(m_data->storage) : KisMutationStorageAllocator<T>{}; }
private:
    Data *m_data = nullptr;
};

// Cold one-shot notification only. Cancelling removes the subscription; an
// already dispatched callback must still validate its consumer incarnation.
// No page identity, eligibility, operation result or owner lifetime is granted.
class KRITAIMAGE_EXPORT KisPageReadinessSubscription final
{
public:
    KisPageReadinessSubscription() = default;
    // Admit the real subscriber node before entering a producer/owner gate.
    explicit KisPageReadinessSubscription(KisPageReadinessCallback callback);
    ~KisPageReadinessSubscription();
    KisPageReadinessSubscription(KisPageReadinessSubscription &&) noexcept;
    KisPageReadinessSubscription &operator=(KisPageReadinessSubscription &&) noexcept;
    KisPageReadinessSubscription(const KisPageReadinessSubscription &) = delete;
    KisPageReadinessSubscription &operator=(const KisPageReadinessSubscription &) = delete;
    void reset();
    bool isValid() const { return m_id != 0 && !m_state.expired(); }
private:
    std::weak_ptr<KisPageReadinessState> m_state;
    KisPageReadinessEntry *m_prepared = nullptr;
    quint64 m_id = 0;
    friend class KisPageReadinessSignal;
    friend class KisCompletionRegistry;
    friend class KisCpuResidentBinding;
};

// Created lazily only by an actual waiter. The producer checks its condition
// and subscribes under the same gate; it notifies after dropping that gate.
class KRITAIMAGE_EXPORT KisPageReadinessSignal final
{
public:
    explicit KisPageReadinessSignal(KisMutationStorageAllocator<std::byte> storage = KisMutationStorageAllocator<std::byte>());
    // Allocation failure preserves every existing callback and subscription.
    KisPageReadinessSubscription subscribe(KisPageReadinessCallback callback);
    void notify();
    qsizetype subscriberCount() const;
    // Registry-only cold cleanup. The immutable callback runs after the signal
    // gate is dropped and must recheck both identity and emptiness at its owner.
    KisPageReadinessSignal(KisPageReadinessCallback emptied, KisMutationStorageAllocator<std::byte> storage);
private:
    // Leave captures with the registry caller until all node allocation succeeds.
    bool subscribeRetained(KisPageReadinessSubscription &prepared);
    bool emptyState(const KisPageReadinessState *expected) const;
    std::shared_ptr<KisPageReadinessState> m_state;
    friend class KisCompletionRegistry;
    friend class KisCpuResidentBinding;
};

#endif
