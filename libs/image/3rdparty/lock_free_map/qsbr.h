/*------------------------------------------------------------------------
  Junction: Concurrent data structures in C++
  Copyright (c) 2016 Jeff Preshing
  Distributed under the Simplified BSD License.
  Original location: https://github.com/preshing/junction
  This software is distributed WITHOUT ANY WARRANTY; without even the
  implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the LICENSE file for more information.
------------------------------------------------------------------------*/

#ifndef QSBR_H
#define QSBR_H

#include <QAtomicPointer>
#include <QAtomicInt>
#include <cstring>
#include <memory>
#include <type_traits>
#include <utility>
#include "kis_assert.h"

class QSBR
{
public:
    // The retirement record is the queue node. Preparing it does not publish
    // a callback; once queued, its target must remain alive until invocation.
    class Action {
    public:
        Action() = default;
        Action(const Action &) = delete;
        Action &operator=(const Action &) = delete;

        template<class T>
        void bind(void (T::*method)(), T *target) noexcept
        {
            struct Closure {
                void (T::*method)();
                T *target;
                void invoke() const { (target->*method)(); }
            };
            const Closure closure{method, target};
            set(closure);
        }

        template<class T>
        void bind(void (*function)(T *), T *target) noexcept
        {
            struct Closure {
                void (*function)(T *);
                T *target;
                void invoke() const { function(target); }
            };
            const Closure closure{function, target};
            set(closure);
        }

    private:
        template<class Closure>
        void set(const Closure &closure) noexcept
        {
            static_assert(sizeof(Closure) <= sizeof(m_parameters));
            static_assert(std::is_trivially_copyable<Closure>::value);
            std::memcpy(m_parameters, &closure, sizeof(closure));
            m_function = [](Action *action) {
                // Reconstitute a real typed object, rather than interpreting
                // a byte buffer as a Closure whose lifetime never began.
                Closure local{};
                std::memcpy(&local, action->m_parameters, sizeof(local));
                if (action->m_heapOwned) delete action;
                local.invoke();
            };
        }
        // An embedded action may destroy its containing target. Copy every
        // needed field before invoking it and never touch the node afterwards.
        void invokeAndDispose()
        {
            m_function(this);
        }
        Action *m_next = nullptr;
        void (*m_function)(Action *) = nullptr;
        quint64 m_parameters[4]{};
        bool m_heapOwned = false;
        friend class QSBR;
    };
    using PreparedAction = std::unique_ptr<Action>;

    static PreparedAction prepare()
    {
        auto action = std::make_unique<Action>();
        action->m_heapOwned = true;
        return action;
    }

    template<class Method, class T>
    static PreparedAction prepare(Method method, T *target)
    {
        auto action = prepare();
        action->bind(method, target);
        return action;
    }

    void enqueuePrepared(PreparedAction action, bool migration = false) noexcept
    {
        Q_ASSERT(action && action->m_function);
        enqueueNode(action.release(), migration);
    }

    // No allocation: the containing owner was allocated before publication.
    // The callback is responsible for retiring that owner. Enqueue once only.
    void enqueueEmbedded(Action &action, bool migration = false) noexcept
    {
        Q_ASSERT(!action.m_heapOwned && action.m_function);
        enqueueNode(&action, migration);
    }

    template<class T>
    void enqueue(void (T::*method)(), T *target, bool migration = false)
    {
        enqueuePrepared(prepare(method, target), migration);
    }

    class RawPointerAccess {
    public:
        explicit RawPointerAccess(QSBR &gc) : m_gc(gc) { m_gc.lockRawPointerAccess(); }
        ~RawPointerAccess() { m_gc.unlockRawPointerAccess(); }
        RawPointerAccess(const RawPointerAccess &) = delete;
        RawPointerAccess &operator=(const RawPointerAccess &) = delete;
    private:
        QSBR &m_gc;
    };

    void update()
    {
        releasePoolSafely(m_pendingActions);
        releasePoolSafely(m_migrationReclaimActions);
    }

    void flush()
    {
        releasePoolSafely(m_pendingActions, true);
        releasePoolSafely(m_migrationReclaimActions, true);
    }

    void lockRawPointerAccess() { m_rawPointerUsers.ref(); }
    void unlockRawPointerAccess() { m_rawPointerUsers.deref(); }
    bool sanityRawPointerAccessLocked() const { return m_rawPointerUsers.loadAcquire(); }

private:
    static void prepend(QAtomicPointer<Action> &pool, Action *first, Action *last) noexcept
    {
        Action *head;
        do {
            head = pool.loadAcquire();
            last->m_next = head;
        } while (!pool.testAndSetOrdered(head, first));
    }

    void enqueueNode(Action *action, bool migration) noexcept
    {
        // Publication transfers the complete record. No separate stack node,
        // closure allocation, destruction or callback belongs to this step.
        prepend(migration ? m_migrationReclaimActions : m_pendingActions, action, action);
    }

    void releasePoolSafely(QAtomicPointer<Action> &pool, bool force = false)
    {
        Action *first = pool.fetchAndStoreOrdered(nullptr);
        if (!first) return;
        Action *last = first;
        size_t count = 1;
        while (last->m_next) { last = last->m_next; ++count; }
        if (force || count > 4096) {
            while (m_rawPointerUsers.loadAcquire()) {}
        } else if (m_rawPointerUsers.loadAcquire()) {
            prepend(pool, first, last);
            return;
        }
        // The detached chain has one consumer. Producers only compare the
        // head pointer; they never dereference nodes already removed here.
        while (first) {
            Action *next = first->m_next;
            first->invokeAndDispose();
            first = next;
        }
    }

    QAtomicInt m_rawPointerUsers{0};
    QAtomicPointer<Action> m_pendingActions{nullptr};
    QAtomicPointer<Action> m_migrationReclaimActions{nullptr};
};

#endif // QSBR_H
