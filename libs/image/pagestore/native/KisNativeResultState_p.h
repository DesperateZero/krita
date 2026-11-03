/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_NATIVE_RESULT_STATE_P_H
#define KIS_NATIVE_RESULT_STATE_P_H
// One protocol authority, shared across implementation units, not a service context.
// gate protects Page/Selection, Resource relationships and View/Edge membership.
// Original Body preparation, accept and cancel own writer bindings; only the
// common all-D accept changes visible selections. Resources keep their physical
// and charged lifetime across writes, retained views and final retirement.
// growthGate/bodyGate serialize capacity/pool changes. terminalGate protects only
// notification scheduling and is never held across gate, binding or provider work.
// historyGate sequences public history operations, never individual page work.
#include "KisPixelWriteCursor.h"
#include <QRegion>
#include <QThread>
#include <boost/range/iterator_range.hpp>
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultProtocol.h"
#include "KisNativeResultDiagnostics_p.h"
#include "KisPageStoreReclamation_p.h"
#include "KisStrokeJobFailureContext.h"
#include "KisPageStoreProductPolicy_p.h"
#include "KisPageStoreDiagnostics_p.h"
#include "KisPageStoreIteratorReadScope_p.h"
#include "KisCpuResidentBinding_p.h"
#include "KisTiles3PageReplicaProvider.h"
#include "tiles3/kis_tiled_data_manager.h"
#include "tiles3/kis_tile_data_store.h"
#include "tiles3/kis_memento_manager.h"
#include "tiles3/KisTiledExtentManager.h"
#include <boost/intrusive/set.hpp>
#include <QScopeGuard>
#include <QReadWriteLock>
#include <QMutexLocker>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

// The actual native adapter boundary owns table/default/extent installation.
// It exposes no old PageStore mutation, private acceptance or publication.
class KisNativeResultAdapter {
public:
    static auto &table(KisTiledDataManager &m) { return *m.m_hashTable; }
    static auto &extent(KisTiledDataManager &m) { return m.m_extentManager; }
    static auto &gate(KisTiledDataManager &m) { return m.m_lock; }
    static quint32 pixelSize(const KisTiledDataManager &m) { return m.pixelSize(); }
    static void bridge(KisTiledDataManager &m, KisTilePageStoreBridge *bridge) { m.m_mementoManager->setPageStoreBridge(bridge); }
    static void installDefault(KisTiledDataManager &m, const quint8 *pixel, KisTileData *tile) { m.installDefaultPixel(pixel, tile); }
};

namespace KisNativeResultDetail {
using Class = KisBackingBudgetClass;
using Domain = KisPageAccessDomain;
using Key = std::pair<qint32, qint32>; // row, column: canonical Resource gate order.
template<class T> using Alloc = KisMutationStorageAllocator<T>;
template<class T> using Vec = std::vector<T, Alloc<T>>;
inline void error(QString *out, const char *s) { if (out) *out = QString::fromLatin1(s); }
inline void delta(KisBackingBudgetDelta &d, Class c, Domain domain, qint64 bytes)
{ *kisPageDomainComponent(d.bucket(c), domain) += bytes; }
inline qint32 floorTile(qint32 v) { return v >= 0 ? v / 64 : qint32((qint64(v) - 63) / 64); }
inline bool bounds(const quint8 *data, const QRect &rect, qsizetype stride, int bpp)
{
    return data && !rect.isEmpty() && bpp > 0 && stride >= qint64(rect.width()) * bpp
        && quint64(stride) <= quint64(std::numeric_limits<qsizetype>::max()) / quint64(rect.height());
}
#if defined(NR104_CENSUS)
#define COUNT(name, n) counts.name += (n)
#else
#define COUNT(name, n) do {} while (false)
#endif
}
using namespace KisNativeResultDetail;

class KisNativeHistory {
public:
    KisNativeResultProtocol::Snapshot before, after;
    quint64 sequence = 0;
    enum class Phase { None, Open, ClosingCommit, ClosingAbort, Failed, Sealed };
    std::atomic<Phase> phase{Phase::None};
};

struct KisNativeResultProtocol::Impl : std::enable_shared_from_this<Impl>, KisTilePageStoreBridge {
    struct Page;
    struct Edge;
    struct Resource {
        Page *home = nullptr;
        std::shared_ptr<KisCpuResidentBinding> binding;
        KisReplicaHandle handle;
        KisTileData *tile = nullptr;
        KisBackingBudgetReservation debt;
        Resource *nextIdle = nullptr;
        // Notification state only; eligibility and charges remain in this Resource.
        Resource *terminalNext = nullptr;
        KisPageReadinessSubscription terminalSubscription;
        quint64 terminalSerial = 0;
        bool terminalRegistered = false, terminalQueued = false, terminalProcessing = false, terminalAgain = false;
        Resource *nextFree = nullptr, *settlementNext = nullptr;
        Class settlementClass = Class::Count;
        bool settling = false;
        quint64 physicalSlot = 0, domainRevision = 0;
        Domain domain = Domain::Unknown;
        Class charged = Class::Count;
        quint32 current = 0, retained = 0, pending = 0;
        std::atomic<bool> physical{false};
        bool busy = false, idle = false, terminal = false, transient = false;
        Class desired() const {
            if (current) return Class::Current;
            if (retained) return Class::RetainedHistory;
            if (pending) return Class::ActivePending;
            return Class::IdleCapacity;
        }
    };
    struct Selection { Resource *resource = nullptr; quint64 generation = 0; };
    using EdgeHook = boost::intrusive::set_base_hook<boost::intrusive::link_mode<boost::intrusive::safe_link>>;
    struct Edge : EdgeHook {
        Page *page = nullptr;
        Selection selection;
        quint64 begin = 0, end = 0;
        View *owner = nullptr;
        Edge *previous = nullptr, *next = nullptr, *freeNext = nullptr;
    };
    struct EdgeLess {
        bool operator()(const Edge &a, const Edge &b) const { return a.begin < b.begin; }
        bool operator()(quint64 a, const Edge &b) const { return a < b.begin; }
        bool operator()(const Edge &a, quint64 b) const { return a.begin < b; }
    };
    using History = boost::intrusive::set<Edge, boost::intrusive::compare<EdgeLess>>;
    struct Page {
        KisPageKey key;
        Selection current;
        quint64 begin = 0, nextGeneration = 1;
        quint32 transient = 0;
        Body *writer = nullptr;
        size_t writerIndex = 0;
        Resource *idle = nullptr;
        History history;
    };
    struct EdgeBlock { std::array<Edge, 32> edges; };
    struct Move {
        Resource *resource;
        Class before, after;
        Domain domain;
        quint64 revision;
        int current = 0, retained = 0, pending = 0;
        bool target = false;
    };
    using Pages = std::map<Key, Page, std::less<Key>, Alloc<std::pair<const Key, Page>>>;
    using Views = std::map<std::pair<quint64, quint64>, View *, std::less<std::pair<quint64, quint64>>,
        Alloc<std::pair<const std::pair<quint64, quint64>, View *>>>;
    using Physical = std::map<quint64, Resource *, std::less<quint64>, Alloc<std::pair<const quint64, Resource *>>>;

    struct ReadLease;
    struct Admission;
    struct DomainMove;
    struct WriteLease;
    struct DefaultLease;

    KisTiledDataManager &adapter;
    KisPageAllocationDescriptor descriptor;
    KisTileData *defaultBacking = nullptr;
    std::shared_ptr<KisBackingBudgetController> parent;
    quint64 providerBytes = 0;
    KisBackingBudgetController budget;
    std::shared_ptr<KisCompletionRegistry> completions;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    std::shared_ptr<Admission> admission;
    QMutex gate, growthGate, bodyGate, asyncGate;
    QRecursiveMutex historyGate; // Public history sequencing; never taken by page execution.
    QMutex terminalGate; // Never held across S, B, provider calls or job execution.
    KisPageReclamationJobPointer terminalJob;
    KisPageReclamationDelay terminalDelay;
    Resource *terminalHead = nullptr, *terminalTail = nullptr;
    std::shared_ptr<Impl> terminalJobOwner;
    size_t terminalCount = 0;
    bool terminalRunning = false, terminalDelayed = false;
    Pages pages;
    Views views;
    Physical physical;
    std::list<Resource, Alloc<Resource>> resources;
    std::list<Body, Alloc<Body>> bodies;
    Vec<std::shared_ptr<EdgeBlock>> blocks;
    Edge *freeEdges = nullptr;
    Resource *freeResources = nullptr;
    Body *freeBodies = nullptr;
    Body *defaultWriter = nullptr;
    size_t edgeFree = 0, edgeReserved = 0;
    quint64 epoch = 1, viewSerial = 0, operation = 1;
    quint64 asyncSource = 0;
    size_t active = 0, domainMoves = 0;
    bool closing = false, closedRootHeld = false;
    const bool forceFresh = qEnvironmentVariableIntValue("KRITA_NATIVE_RESULT_FORCE_FRESH") == 1;
    Counts counts;
    std::shared_ptr<const KisPageStoreIteratorReadScope> failedReadScope;
    KisMementoSP currentHistory;
    Vec<std::weak_ptr<KisNativeHistory>> history;
    quint64 historySequence = 0;
    quint64 defaultRevision = 1;

    Impl(KisTiledDataManager &a, const KisPageAllocationDescriptor &desc, const KisPageBackingLimits &limits,
         std::shared_ptr<KisBackingBudgetController> sharedParent, quint64 physicalLimit)
        : adapter(a), descriptor(desc), parent(std::move(sharedParent)), providerBytes(physicalLimit),
          budget(limits), pages(std::less<Key>{}, Alloc<std::pair<const Key, Page>>(&budget)),
          views(std::less<std::pair<quint64, quint64>>{}, Alloc<std::pair<const std::pair<quint64, quint64>, View *>>(&budget)),
          physical(std::less<quint64>{}, Alloc<std::pair<const quint64, Resource *>>(&budget)),
          resources(Alloc<Resource>(&budget)), bodies(Alloc<Body>(&budget)),
          blocks(Alloc<std::shared_ptr<EdgeBlock>>(&budget)), history(Alloc<std::weak_ptr<KisNativeHistory>>(&budget)) {}
    void initialize();
    ~Impl();
    KisPageOperationId nextOperation() { QMutexLocker lock(&gate);
        return operation == std::numeric_limits<quint64>::max() ? KisPageOperationId{} : KisPageOperationId{++operation}; }
    void ensureEdges(size_t n);
    Body *checkout();
    void returnBody(Body *) noexcept;
    void cancelBody(Body *) noexcept;
    void finishProducer(Body &) noexcept;
    static void finishRaw(void *) noexcept;
    bool bindRows(Body &, const QRect &, QString *, bool append = false);
    static thread_local Body *iteratorHead;
    Body *iteratorBody() const;
    void detachIterator(Body *);
    bool finishIterator(Body *, bool success);
    bool finishRawResult(Body *) noexcept;
    bool prepareAdapter(Body &, QString *);
    bool prepareDefault(Body &, const QByteArray &, QString *);
    Selection viewSelection(const View &, Key) const;
    bool prepareTargets(Body &, const quint8 *, const QRect &, qsizetype, const Faults *, QString *, size_t first = 0, size_t last = std::numeric_limits<size_t>::max());
    bool acceptBody(Body &, const Faults *, QString *);
    void buildPlan(Body &);
    void preparePlan(Body &, KisBackingBudgetDelta &);
    bool samePlan(Body &) const;
    Resource *fresh(Page &, const KisPageVersion &, const quint8 *, qsizetype, Resource *, const QRect &, QString *,
        const std::shared_ptr<const KisPageReplicaSource> &imported = {});
    // A transient is an actual unpublished logical result, not every retained
    // or idle physical allocation. Its original Resource carries this fact.
    static void settleTransient(Resource *r) {
        if (r->transient && (r->current || (!r->retained && !r->pending))) {
            Q_ASSERT(r->home->transient); --r->home->transient; r->transient = false;
        }
    }
    void park(Resource *) noexcept;
    void retire(Resource *) noexcept;
    bool retireNow(Resource *) noexcept;
    void deferRetirement(Resource *) noexcept;
    void queueTerminal(Resource *, quint64 serial) noexcept;
    void enqueueTerminalLocked(Resource *) noexcept;
    void startTerminalLocked() noexcept;
    void runTerminals() noexcept;
    void finishTerminalJob() noexcept;
    void completeTerminal(Resource *) noexcept;
    void reclassifyUnselected(Resource *) noexcept;
    void claimReleased(Resource *, Resource *&) noexcept;
    void settleReleased(Resource *, bool retirePhysical = true) noexcept;
    void releaseView(View *) noexcept;
    void finishClosing() noexcept;
    View *retentionOwner(quint64 begin, quint64 end) const;
    Selection selection(const Page &, quint64 at) const;
    void linkEdge(Edge *, View *);
    void unlinkEdge(Edge *);
    bool readSelection(Selection, quint8 *, const QRect &, qsizetype, const KisPageByteArray &, QString *) const;
    TileLease acquireTile(qint32 column, qint32 row, Access access, TileLease *cache) override;
};

struct KisNativeResultProtocol::View {
    struct Item { Key key; Impl::Selection selection; };
    std::shared_ptr<Impl> owner;
    Vec<Item> items;
    QRect range;
    KisPageByteArray defaultPixel;
    KisTileData *defaultBacking = nullptr;
    quint64 epoch = 0, serial = 0, defaultRevision = 0;
    Impl::Edge *first = nullptr;
    bool registered = false, explicitItems = false;
    explicit View(KisBackingBudgetController *budget) : items(Alloc<Item>(budget)) {}
    ~View() { if (registered || explicitItems) owner->releaseView(this); if (defaultBacking) defaultBacking->deref(); }
};


struct KisNativeResultProtocol::Body {
    struct Row {
        Impl::Page *page = nullptr;
        Impl::Selection before, after;
        Impl::Resource *target = nullptr;
        Impl::Resource *idleCandidate = nullptr;
        View *retain = nullptr;
        KisTile *location = nullptr;
        KisTileHashTable::PreparedTile adapter;
        std::unique_ptr<KisTilePageStoreLease> detachedRead;
        QRect local;
        KisCpuWriteBindingReservation writable;
        quint8 *pixels = nullptr;
        bool changed = false, ownsTarget = false, remove = false, discard = false, selectionOnly = false;
    };
    Vec<Row> rows;
    Vec<Impl::Move> moves;
    Vec<KisTileSP> defaultReaders;
    Body *next = nullptr, *iteratorNext = nullptr;
    Impl *iteratorOwner = nullptr;
    size_t clients = 0, borrows = 0;
    bool iterator = false, raw = false, revoked = false;
    Qt::HANDLE thread = nullptr;
    std::shared_ptr<Impl> deferredOwner;
    std::shared_ptr<KisNativeHistory> history;
    KisPageByteArray defaultPixel;
    KisTileData *defaultTile = nullptr;
    size_t edgeBudget = 0;
    quint64 cutEpoch = 0;
    bool ready = false, accepted = false, restore = false, rawDirty = false;
    bool asyncPending = false, abandoned = false, failed = false, adapterReady = false;
    explicit Body(KisBackingBudgetController *b) : rows(Alloc<Row>(b)), moves(Alloc<Impl::Move>(b)), defaultReaders(Alloc<KisTileSP>(b)) {}
    ~Body() { if (defaultTile) defaultTile->deref(); }
};


inline KisNativeResultProtocol::View *KisNativeResultProtocol::Impl::retentionOwner(quint64 begin, quint64 end) const
{
    const auto it = views.lower_bound({begin, 0});
    return it != views.end() && it->first.first < end ? it->second : nullptr;
}


inline KisNativeResultProtocol::Impl::Selection KisNativeResultProtocol::Impl::selection(const Page &p, quint64 at) const
{
    if (p.begin <= at) return p.current;
    auto it = p.history.upper_bound(at, EdgeLess{});
    if (it == p.history.begin()) return {};
    --it;
    return at < it->end ? it->selection : Selection{};
}


inline void KisNativeResultProtocol::Impl::linkEdge(Edge *e, View *v)
{
    e->owner = v; e->previous = nullptr; e->next = v->first;
    if (e->next) e->next->previous = e;
    v->first = e;
}


inline void KisNativeResultProtocol::Impl::unlinkEdge(Edge *e)
{
    if (e->previous) e->previous->next = e->next; else e->owner->first = e->next;
    if (e->next) e->next->previous = e->previous;
    e->owner = nullptr; e->previous = e->next = nullptr;
}


inline KisNativeResultProtocol::Impl::Selection KisNativeResultProtocol::Impl::viewSelection(const View &v, Key key) const
{
    if (v.explicitItems) {
        auto it = std::lower_bound(v.items.begin(), v.items.end(), key, [](const View::Item &a, Key b) { return a.key < b; });
        return it != v.items.end() && it->key == key ? it->selection : Selection{};
    }
    auto it = pages.find(key);
    return it != pages.end() ? selection(it->second, v.epoch) : Selection{};
}


#endif
