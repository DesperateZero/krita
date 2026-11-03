/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultProtocol.h"
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

// The existing test friend exposes the real adapter primitives. This class is
// confined to the isolated construction executable, never an alternate store.
class KisTiledDataManagerTest {
public:
    static auto &table(KisTiledDataManager &m) { return *m.m_hashTable; }
    static auto &extent(KisTiledDataManager &m) { return m.m_extentManager; }
    static auto &gate(KisTiledDataManager &m) { return m.m_lock; }
    static bool hasBackend(KisTiledDataManager &m) { return m.m_pageStoreBackend != nullptr; }
    static quint32 pixelSize(const KisTiledDataManager &m) { return m.pixelSize(); }
    static void bridge(KisTiledDataManager &m, KisTilePageStoreBridge *bridge) { m.m_mementoManager->setPageStoreBridge(bridge); }
    static void installDefault(KisTiledDataManager &m, const quint8 *pixel, KisTileData *tile) { m.installDefaultPixel(pixel, tile); }
};

namespace {
using Class = KisBackingBudgetClass;
using Domain = KisPageAccessDomain;
using Key = std::pair<qint32, qint32>; // row, column: canonical Resource gate order.
template<class T> using Alloc = KisMutationStorageAllocator<T>;
template<class T> using Vec = std::vector<T, Alloc<T>>;
void error(QString *out, const char *s) { if (out) *out = QString::fromLatin1(s); }
void delta(KisBackingBudgetDelta &d, Class c, Domain domain, qint64 bytes)
{ *kisPageDomainComponent(d.bucket(c), domain) += bytes; }
qint32 floorTile(qint32 v) { return v >= 0 ? v / 64 : qint32((qint64(v) - 63) / 64); }
bool bounds(const quint8 *data, const QRect &rect, qsizetype stride, int bpp)
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

KisCpuWriteBindingReservation KisNativeResultProtocol::claimNative(
    const std::shared_ptr<KisCpuResidentBinding> &binding, quint64 incarnation,
    const KisPageVersion &target)
{
    // Birth established native layout/key/provider provenance. Actual mutable
    // exclusion is still the production Binding -> Tiles3 -> TileData service.
    if (!binding || !(binding->m_key == target.key) || !target.generation.isValid()
        || incarnation == std::numeric_limits<quint64>::max()
        || !binding->tryClaimHandoff(incarnation)) return {};
    binding->finishHandoff(&target);
    KisCpuWriteBindingReservation result;
    result.m_binding = binding;
    return result;
}

bool KisNativeResultProtocol::validateNative(KisCpuResidentBinding *b, const KisReplicaHandle &expected)
{
    // Called with B held until the all-D no-fail cut. Immutable layout is not
    // projected out of this same binding merely to compare it with itself.
    return b && !b->m_retired && !b->m_storageBusy
        && b->allocationIdentityLocked() == expected.allocationIdentity()
        && b->matchesVersionLocked(expected.version);
}

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
        Resource *nextFree = nullptr, *settlementNext = nullptr;
        Class settlementClass = Class::Count;
        bool settling = false;
        quint64 physicalSlot = 0, domainRevision = 0;
        Domain domain = Domain::Unknown;
        Class charged = Class::Count;
        quint32 current = 0, retained = 0, pending = 0;
        bool physical = false, busy = false, idle = false, terminal = false;
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
        Body *writer = nullptr;
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

    struct ReadLease final : KisTilePageStoreLease {
        std::shared_ptr<Impl> owner;
        std::shared_ptr<KisCpuResidentBinding> binding;
        KisReplicaHandle expected;
        KisTileData *tile = nullptr;
        bool pinned = false;
        ~ReadLease() override { finish(); }
        KisTileData *tileData() const override { return pinned ? tile : nullptr; }
        bool writable() const override { return false; }
        void markDirty() override {}
        bool finish(TileLeaseRelease * = nullptr) override {
            if (std::exchange(pinned, false)) binding->releaseRead();
            return true;
        }
        bool reusableReadCache() const override { return true; }
        bool readPinned() const override { return pinned; }
        ReadPinResult repinRead() override {
            if (pinned) return ReadPinResult::Ready;
            KisCpuResidentReadStatus status;
            pinned = binding->acquireRead(expected.allocationIdentity(), false, &status) != nullptr;
            if (status == KisCpuResidentReadStatus::Retired) return ReadPinResult::Retired;
            return pinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
        }
        static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(ReadLease)); }
        static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(ReadLease), alignof(ReadLease)); }
    };
    struct Admission;
    struct DomainMove;
    struct WriteLease;
    struct DefaultLease;

    KisTiledDataManager &adapter;
    KisPageAllocationDescriptor descriptor;
    std::shared_ptr<KisBackingBudgetController> parent;
    KisBackingBudgetController budget;
    std::shared_ptr<KisCompletionRegistry> completions;
    std::shared_ptr<KisTiles3PageReplicaProvider> provider;
    std::shared_ptr<Admission> admission;
    QMutex gate, growthGate, bodyGate, asyncGate;
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
    Counts counts;
    quint64 defaultRevision = 1;

    Impl(KisTiledDataManager &a, const KisPageAllocationDescriptor &desc, const KisPageBackingLimits &limits)
        : adapter(a), descriptor(desc), parent(std::allocate_shared<KisBackingBudgetController>(Alloc<KisBackingBudgetController>{}, KisPageBackingLimits{}, true)),
          budget(limits), pages(std::less<Key>{}, Alloc<std::pair<const Key, Page>>(&budget)),
          views(std::less<std::pair<quint64, quint64>>{}, Alloc<std::pair<const std::pair<quint64, quint64>, View *>>(&budget)),
          physical(std::less<quint64>{}, Alloc<std::pair<const quint64, Resource *>>(&budget)),
          resources(Alloc<Resource>(&budget)), bodies(Alloc<Body>(&budget)),
          blocks(Alloc<std::shared_ptr<EdgeBlock>>(&budget)) {}
    void initialize();
    ~Impl();
    KisPageOperationId nextOperation() { QMutexLocker lock(&gate); return {++operation}; }
    void ensureEdges(size_t n);
    Body *checkout();
    void returnBody(Body *) noexcept;
    void cancelBody(Body *) noexcept;
    void finishProducer(Body &) noexcept;
    static void finishRaw(void *) noexcept;
    bool bindRows(Body &, const QRect &, QString *);
    bool prepareAdapter(Body &, QString *);
    bool prepareDefault(Body &, const QByteArray &, QString *);
    Selection viewSelection(const View &, Key) const;
    bool prepareTargets(Body &, const quint8 *, const QRect &, qsizetype, const Faults *, QString *);
    bool acceptBody(Body &, const Faults *, QString *);
    void buildPlan(Body &);
    void preparePlan(Body &, KisBackingBudgetDelta &);
    bool samePlan(Body &) const;
    Resource *fresh(Page &, const KisPageVersion &, const quint8 *, qsizetype, Resource *, const QRect &, QString *,
        const std::shared_ptr<const KisPageReplicaSource> &imported = {});
    void park(Resource *) noexcept;
    void retire(Resource *) noexcept;
    void reclassifyUnselected(Resource *) noexcept;
    void claimReleased(Resource *, Resource *&) noexcept;
    void settleReleased(Resource *) noexcept;
    void releaseView(View *) noexcept;
    void finishClosing() noexcept;
    View *retentionOwner(quint64 begin, quint64 end) const;
    Selection selection(const Page &, quint64 at) const;
    void linkEdge(Edge *, View *);
    void unlinkEdge(Edge *);
    bool readSelection(Selection, quint8 *, const QRect &, qsizetype, const QByteArray &, QString *) const;
    TileLease acquireTile(qint32 column, qint32 row, Access access, TileLease *cache) override;
};

struct KisNativeResultProtocol::View {
    struct Item { Key key; Impl::Selection selection; };
    std::shared_ptr<Impl> owner;
    Vec<Item> items;
    QRect range;
    QByteArray defaultPixel;
    quint64 epoch = 0, serial = 0;
    Impl::Edge *first = nullptr;
    bool registered = false, explicitItems = false;
    explicit View(KisBackingBudgetController *budget) : items(Alloc<Item>(budget)) {}
    ~View() { if (registered || explicitItems) owner->releaseView(this); }
};

struct KisNativeResultProtocol::Impl::DefaultLease final : KisTilePageStoreLease {
    std::shared_ptr<Impl> owner;
    KisTileData *tile = nullptr;
    quint64 revision = 0;
    bool pinned = false;
    ~DefaultLease() override { finish(); if (tile) tile->deref(); }
    KisTileData *tileData() const override { return pinned ? tile : nullptr; }
    bool writable() const override { return false; }
    void markDirty() override {}
    bool finish(TileLeaseRelease * = nullptr) override { if (std::exchange(pinned, false)) tile->unblockSwapping(); return true; }
    bool reusableReadCache() const override { return true; }
    bool readPinned() const override { return pinned; }
    ReadPinResult repinRead() override {
        { QMutexLocker lock(&owner->gate); if (revision != owner->defaultRevision) return ReadPinResult::Stale; }
        if (!pinned) pinned = tile->blockSwapping();
        return pinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
    }
    static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(DefaultLease)); }
    static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(DefaultLease), alignof(DefaultLease)); }
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
        QRect local;
        KisCpuWriteBindingReservation writable;
        quint8 *pixels = nullptr;
        bool changed = false, ownsTarget = false;
    };
    Vec<Row> rows;
    Vec<Impl::Move> moves;
    Body *next = nullptr;
    std::shared_ptr<Impl> deferredOwner;
    QByteArray defaultPixel;
    KisTileData *defaultTile = nullptr;
    size_t edgeBudget = 0;
    quint64 cutEpoch = 0;
    bool ready = false, accepted = false, restore = false, rawDirty = false;
    bool asyncPending = false, abandoned = false, failed = false, adapterReady = false;
    explicit Body(KisBackingBudgetController *b) : rows(Alloc<Row>(b)), moves(Alloc<Impl::Move>(b)) {}
    ~Body() { if (defaultTile) defaultTile->deref(); }
};

struct KisNativeResultProtocol::Async {
    std::shared_ptr<Impl> owner;
    std::atomic<Body *> body{nullptr};
    KisCompletionTicket ticket;
    ~Async() { finish(false, true); }
    bool finish(bool success, bool cancelled = false) {
        auto *b = body.exchange(nullptr);
        if (!b) return false;
        owner->completions->completePrepared(ticket, success ? KisCompletionStatus::Succeeded
            : cancelled ? KisCompletionStatus::Cancelled : KisCompletionStatus::Failed);
        const auto terminal = owner->completions->verifyTerminal(ticket);
        Q_ASSERT(terminal.isValid());
        owner->finishProducer(*b);
        bool abandoned;
        { QMutexLocker lock(&owner->gate); b->asyncPending = false; b->failed = !success; abandoned = b->abandoned; }
        if (abandoned) { owner->cancelBody(b); owner->finishClosing(); }
        return true;
    }
};

struct KisNativeResultProtocol::Impl::WriteLease final : KisTilePageStoreLease {
    std::shared_ptr<Impl> owner;
    Body *body = nullptr;
    ~WriteLease() override { finish(); }
    KisTileData *tileData() const override { return body ? body->rows.front().target->tile : nullptr; }
    bool writable() const override { return body != nullptr; }
    void markDirty() override { if (body) body->rawDirty = true; }
    bool finish(TileLeaseRelease *afterUnlock = nullptr) override {
        if (!body) return true;
        auto *b = std::exchange(body, nullptr);
        owner->finishProducer(*b);
        b->deferredOwner = owner;
        if (afterUnlock) *afterUnlock = TileLeaseRelease(b, &Impl::finishRaw);
        else Impl::finishRaw(b);
        return true;
    }
    static void *operator new(size_t bytes) { return kisAllocatePageProcessStorage(bytes, alignof(WriteLease)); }
    static void operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(WriteLease), alignof(WriteLease)); }
};

struct KisNativeResultProtocol::Impl::DomainMove final : KisReplicaBackingDomainReservation {
    std::shared_ptr<Impl> owner;
    Resource *resource = nullptr;
    Domain source, target;
    KisBackingBudgetDelta change;
    KisBackingBudgetReservation fee;
    ~DomainMove() override {
        if (!resource) return;
        fee.release();
        QMutexLocker lock(&owner->gate);
        resource->busy = false; --owner->domainMoves;
    }
    void commit(quint64 revision) noexcept override {
        if (!resource) return;
        auto *r = resource;
        {
            QMutexLocker lock(&owner->gate);
            Q_ASSERT(r->busy && r->domain == source && revision != r->domainRevision);
            fee.commit(change);
            // The actual existing retirement credit follows this allocation;
            // domain movement does not create a second physical obligation.
            if (r->debt.isValid()) owner->budget.moveRetirementHeadroom(
                r->debt.cookie, source, target, owner->descriptor.minimumByteSize());
            r->domain = target; r->domainRevision = revision;
            r->busy = false; --owner->domainMoves; resource = nullptr;
        }
    }
};

struct KisNativeResultProtocol::Impl::Admission final : KisReplicaBackingDomainAdmission {
    std::weak_ptr<Impl> owner;
    explicit Admission(const std::shared_ptr<Impl> &p) : owner(p) {}
    std::shared_ptr<KisReplicaBackingDomainReservation> prepare(
        const KisReplicaPhysicalSlotIdentity &physical, quint64 bytes,
        Domain source, quint64 revision, Domain target, QString *why) override
    try {
        auto p = owner.lock();
        if (!p || !(physical.provider == p->provider->providerId())
            || !(physical.providerEpoch == p->provider->providerEpoch())
            || bytes != p->descriptor.minimumByteSize() || target == Domain::Unknown || source == target) {
#ifdef NR104_CENSUS
            qWarning("104 domain birth mismatch bytes=%llu expected=%llu", bytes, p ? p->descriptor.minimumByteSize() : 0);
#endif
            return {};
        }
        Resource *r;
        {
            QMutexLocker lock(&p->gate);
            auto it = p->physical.find(physical.slot);
            if (it == p->physical.end() || it->second->busy || it->second->domain != source
                || it->second->domainRevision != revision) {
#ifdef NR104_CENSUS
                qWarning("104 domain claim refused slot=%llu revision=%llu found=%d busy=%d", physical.slot, revision, it != p->physical.end(), it != p->physical.end() && it->second->busy);
#endif
                error(why, "physical domain is unregistered or claimed"); return {};
            }
            r = it->second; r->busy = true; ++p->domainMoves;
        }
        auto undo = qScopeGuard([&] { if (r) { QMutexLocker lock(&p->gate); r->busy = false; --p->domainMoves; } });
        auto move = std::allocate_shared<DomainMove>(Alloc<DomainMove>::retained(&p->budget));
        delta(move->change, r->charged, source, -qint64(bytes));
        delta(move->change, r->charged, target, qint64(bytes));
        move->fee = p->budget.reserveChange(move->change, why);
        if (!move->fee.isValid()) {
#ifdef NR104_CENSUS
            qWarning("104 domain fee refused: %s", why ? qPrintable(*why) : "");
#endif
            return {};
        }
        move->owner = p; move->source = source; move->target = target;
        move->resource = std::exchange(r, nullptr);
        return move;
    } catch (const std::bad_alloc &) { error(why, "domain claim storage refused"); return {}; }
};

void KisNativeResultProtocol::Impl::initialize()
{
    QString why;
    if (!descriptor.isValid() || !budget.configureSharedNonPayloadBudget(parent, &why)) throw std::runtime_error(why.toStdString());
    completions = std::allocate_shared<KisCompletionRegistry>(Alloc<KisCompletionRegistry>(&budget), parent, Alloc<KisCompletionRegistry>(&budget));
    provider = std::allocate_shared<KisTiles3PageReplicaProvider>(Alloc<KisTiles3PageReplicaProvider>(&budget), Alloc<KisTiles3PageReplicaProvider>(&budget));
    KisCpuResidentReplicaProviderConfig config;
    config.provider = {104}; config.providerEpoch = {1}; config.budgetBytes = 256 * 1024 * 1024;
    if (!provider->configure(config, completions, &why, parent)) throw std::runtime_error(why.toStdString());
    admission = std::allocate_shared<Admission>(Alloc<Admission>::retained(&budget), shared_from_this());
    if (!provider->registerBackingDomainAdmission(admission, &why)) throw std::runtime_error(why.toStdString());
    blocks.reserve(8);
    if (KisTiledDataManagerTest::hasBackend(adapter)) throw std::runtime_error("native protocol requires an external selection adapter");
    if (KisTiledDataManagerTest::pixelSize(adapter) != descriptor.format.pixelStride || KisTiledDataManagerTest::table(adapter).numTiles())
        throw std::runtime_error("native protocol requires an empty compatible adapter");
    if (std::memcmp(adapter.defaultPixel(), descriptor.format.defaultPixel.constData(), descriptor.format.pixelStride)) {
        auto *tile = KisTileDataStore::instance()->createDefaultTileData(descriptor.format.pixelStride,
            reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()));
        if (!tile) throw std::bad_alloc();
        tile->ref();
        KisTiledDataManagerTest::installDefault(adapter, reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()), tile);
        tile->deref();
    }
    KisTiledDataManagerTest::bridge(adapter, this);
}

KisNativeResultProtocol::Impl::~Impl()
{
    // Facade/capabilities have already stopped every new access. Real storage
    // goes away before its local and process metadata allocators are destroyed.
    Q_ASSERT(active == 0 && views.empty());
    for (auto &p : pages) {
        if (p.second.current.resource) {
            if (closedRootHeld) --p.second.current.resource->retained;
            else --p.second.current.resource->current;
        }
        p.second.current = {}; p.second.idle = nullptr;
    }
    for (auto &r : resources) { r.idle = false; r.nextIdle = nullptr; }
    for (auto &r : resources) if (r.physical) retire(&r);
    Q_ASSERT(physical.empty());
    pages.clear();
    bodies.clear();
    resources.clear();
    physical.clear();
    blocks.clear();
    admission.reset();
    provider.reset();
    completions.reset();
}

void KisNativeResultProtocol::Impl::ensureEdges(size_t n)
{
    QMutexLocker growing(&growthGate);
    for (;;) {
        {
            QMutexLocker lock(&gate);
            if (edgeFree >= edgeReserved + n) { edgeReserved += n; return; }
        }
        // Charged capacity is prepared outside S. All uses are intrusive and
        // allocation-free at the final visibility cut.
        auto block = std::allocate_shared<EdgeBlock>(Alloc<EdgeBlock>(&budget));
        blocks.push_back(block);
        QMutexLocker lock(&gate);
        for (auto &e : block->edges) { e.freeNext = freeEdges; freeEdges = &e; ++edgeFree; }
    }
}

KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::checkout()
{
    {
        QMutexLocker lock(&gate);
        if (closing || defaultWriter) return nullptr;
        ++active;
        if (auto *b = freeBodies) { freeBodies = b->next; b->next = nullptr; return b; }
    }
    try {
        QMutexLocker allocation(&bodyGate);
        bodies.emplace_back(&budget);
        return &bodies.back();
    } catch (...) { QMutexLocker lock(&gate); --active; throw; }
}

void KisNativeResultProtocol::Impl::returnBody(Body *b) noexcept
{
    // Candidate destruction can release real adapter/GC storage; never do it
    // under S or the manager gate.
    b->rows.clear(); b->moves.clear();
    if (b->defaultTile) { b->defaultTile->deref(); b->defaultTile = nullptr; }
    b->defaultPixel.clear();
    b->ready = b->accepted = b->restore = b->rawDirty = false;
    b->asyncPending = b->abandoned = b->failed = b->adapterReady = false;
    QMutexLocker lock(&gate);
    if (defaultWriter == b) defaultWriter = nullptr;
    Q_ASSERT(edgeReserved >= b->edgeBudget);
    edgeReserved -= std::exchange(b->edgeBudget, 0);
    b->next = freeBodies; freeBodies = b; --active;
}

KisNativeResultProtocol::View *KisNativeResultProtocol::Impl::retentionOwner(quint64 begin, quint64 end) const
{
    const auto it = views.lower_bound({begin, 0});
    return it != views.end() && it->first.first < end ? it->second : nullptr;
}

KisNativeResultProtocol::Impl::Selection KisNativeResultProtocol::Impl::selection(const Page &p, quint64 at) const
{
    if (p.begin <= at) return p.current;
    auto it = p.history.upper_bound(at, EdgeLess{});
    if (it == p.history.begin()) return {};
    --it;
    return at < it->end ? it->selection : Selection{};
}

void KisNativeResultProtocol::Impl::linkEdge(Edge *e, View *v)
{
    e->owner = v; e->previous = nullptr; e->next = v->first;
    if (e->next) e->next->previous = e;
    v->first = e;
}

void KisNativeResultProtocol::Impl::unlinkEdge(Edge *e)
{
    if (e->previous) e->previous->next = e->next; else e->owner->first = e->next;
    if (e->next) e->next->previous = e->previous;
    e->owner = nullptr; e->previous = e->next = nullptr;
}

KisNativeResultProtocol::Impl::Resource *KisNativeResultProtocol::Impl::fresh(
    Page &page, const KisPageVersion &version, const quint8 *payload, qsizetype stride,
    Resource *before, const QRect &local, QString *why, const std::shared_ptr<const KisPageReplicaSource> &imported)
{
    const auto bytes = qint64(descriptor.minimumByteSize());
    KisBackingBudgetDelta requested;
    delta(requested, Class::ActivePending, Domain::CpuRam, bytes);
    delta(requested, Class::RetirementDebt, Domain::CpuRam, bytes);
    auto reservation = budget.reserve(requested, why);
    if (!reservation.isValid()) return nullptr;
    Resource *r = nullptr;
    {
        QMutexLocker lock(&gate);
        if (freeResources) { r = freeResources; freeResources = r->nextFree; r->nextFree = nullptr; }
    }
    if (!r) { QMutexLocker allocation(&bodyGate); resources.emplace_back(); r = &resources.back(); }
    r->home = &page;
    auto recycle = qScopeGuard([&] {
        if (!r) return;
        if (r->physical) { r->pending = 0; retire(r); }
        else { QMutexLocker lock(&gate); r->nextFree = freeResources; freeResources = r; }
    });
    Physical staged(std::less<quint64>{}, physical.get_allocator());
    staged.emplace(0, r); // terminal index capacity exists before physical birth
    KisReplicaOperation made;
    if (imported) {
        made = provider->prepareSynchronousSource(nextOperation(), imported, version, descriptor,
            KisReplicaSourceUse::ImmutableAlias, KisPagePriority::Interactive);
    } else if (payload && local == QRect(0, 0, 64, 64)) {
        const KisCpuPagePayload source{payload, stride, 63 * stride + 64 * descriptor.format.pixelStride};
        made = provider->prepareSynchronousCpuPayload(nextOperation(), version, descriptor, source, KisPagePriority::Interactive);
    } else if (before) {
        made = provider->prepareSynchronousWriteCopy(nextOperation(), before->handle, version, descriptor, KisPagePriority::Interactive);
    } else {
        made = provider->prepareWrite(nextOperation(), version, descriptor, Domain::CpuRam,
                                     KisPageWriteMode::DiscardContents, KisPagePriority::Interactive);
    }
    if (!made.isValid() || made.status != KisPageRequestStatus::Ready) {
        error(why, "real provider creation refused"); return nullptr;
    }
    r->handle = made.replica;
    r->binding = provider->cpuResidentBinding(r->handle);
    r->physical = true; r->charged = Class::ActivePending; r->pending = 1;
    const auto footprint = provider->backingFootprint(r->handle);
    if (!footprint.isValid()) qFatal("native provider broke its physical birth contract");
    r->domain = footprint.domain; r->domainRevision = footprint.revision; r->physicalSlot = footprint.physicalSlot;
    KisBackingBudgetDelta installed, retained;
    delta(installed, Class::ActivePending, r->domain, bytes);
    delta(retained, Class::RetirementDebt, r->domain, bytes);
    reservation.commitRetaining(installed, retained);
    r->debt = std::move(reservation);
    // Both metadata registration and the real native TileData pointer belong
    // to this physical birth, not each logical write. The public provider lease
    // is consumed now and never synthesized again by the warm path.
    auto node = staged.extract(staged.begin()); node.key() = r->physicalSlot;
    {
        QMutexLocker lock(&gate); physical.insert(std::move(node)); COUNT(fresh, 1);
    }
    const auto lease = KisPageLeaseId{nextOperation().value};
    auto access = provider->resolveAccess(lease, nextOperation(), r->handle,
        {Domain::CpuRam, KisPageAccessKind::CpuPointer}, KisPageAccessMode::Read);
    if (access.isValid()) r->tile = provider->tileDataForLease(lease);
    provider->releaseAccess(std::move(access), {});
    if (!r->binding || !r->tile || !footprint.isValid()) {
        error(why, "real native factory returned incomplete ownership"); return nullptr;
    }
    return std::exchange(r, nullptr);
}

void KisNativeResultProtocol::Impl::park(Resource *r) noexcept
{
    Q_ASSERT(!r->current && !r->retained && !r->pending && r->physical && !r->idle);
    r->idle = true; r->nextIdle = r->home->idle; r->home->idle = r;
}

void KisNativeResultProtocol::Impl::retire(Resource *r) noexcept
{
    if (!r || !r->physical) return;
    {
        QMutexLocker lock(&gate);
        if (r->busy || r->current || r->retained || r->pending) { r->terminal = true; return; }
        if (r->idle) {
            auto **link = &r->home->idle;
            while (*link && *link != r) link = &(*link)->nextIdle;
            Q_ASSERT(*link == r); *link = r->nextIdle;
            r->idle = false; r->nextIdle = nullptr;
        }
        r->busy = true;
    }
    const auto done = provider->retire(nextOperation(), r->handle, {});
    if (!done.isValid() || done.status != KisPageRequestStatus::Ready) {
        // The birth reservation is the actual funded fallback. No source live
        // charge is returned before retirement has really completed.
        if (r->charged != Class::RetirementDebt && r->debt.isValid()) {
            KisBackingBudgetDelta change;
            delta(change, r->charged, r->domain, -qint64(descriptor.minimumByteSize()));
            delta(change, Class::RetirementDebt, r->domain, qint64(descriptor.minimumByteSize()));
            r->debt.commit(change);
            r->charged = Class::RetirementDebt;
        }
        QMutexLocker lock(&gate); r->busy = false; r->terminal = true; return;
    }
    budget.releaseLive(r->charged, r->domain, descriptor.minimumByteSize());
    r->debt.release();
    r->binding.reset();
    Physical::node_type released;
    {
        QMutexLocker lock(&gate);
        released = physical.extract(r->physicalSlot);
        r->physical = false; r->busy = r->terminal = false; r->tile = nullptr;
        r->charged = Class::Count; r->handle = {}; r->physicalSlot = 0;
        r->nextFree = freeResources; freeResources = r;
        COUNT(retired, 1);
    }
}

void KisNativeResultProtocol::Impl::claimReleased(Resource *r, Resource *&head) noexcept
{
    if (!r || !r->physical) return;
    if (r->busy) { if (!r->settling) r->terminal = true; return; }
    const auto wanted = r->desired();
    if (wanted == r->charged && !(wanted == Class::IdleCapacity && (closing || r->terminal))) {
        if (wanted == Class::IdleCapacity && !r->idle) park(r);
        return;
    }
    r->busy = r->settling = true; r->settlementNext = head; head = r;
}

void KisNativeResultProtocol::Impl::settleReleased(Resource *head) noexcept
{
    if (!head) return;
    auto allowance = budget.idleCapacityAllowance();
    KisBackingBudgetDelta change;
    {
        QMutexLocker lock(&gate);
        for (auto *r = head; r; r = r->settlementNext) {
            auto wanted = r->desired();
            if (wanted == Class::IdleCapacity && r->charged != wanted) {
                if (!closing && !r->terminal && allowance >= descriptor.minimumByteSize()) allowance -= descriptor.minimumByteSize();
                else wanted = Class::Count; // direct physical termination, not another class round trip
            } else if (wanted == Class::IdleCapacity && (closing || r->terminal)) wanted = Class::Count;
            r->settlementClass = wanted;
            if (wanted != r->charged && wanted != Class::RetirementDebt && wanted != Class::Count) {
                delta(change, r->charged, r->domain, -qint64(descriptor.minimumByteSize()));
                delta(change, wanted, r->domain, qint64(descriptor.minimumByteSize()));
            }
        }
    }
    KisBackingBudgetReservation fee;
    if (change.changedClasses()) fee = budget.reserveChange(change, nullptr);
    const bool admitted = !change.changedClasses() || fee.isValid();
    if (admitted && fee.isValid()) fee.commit(change);
    while (head) {
        auto *r = head; head = r->settlementNext;
        bool terminal = false;
        {
            QMutexLocker lock(&gate);
            auto wanted = r->settlementClass;
            if (!admitted && wanted != r->charged && wanted != Class::RetirementDebt && wanted != Class::Count) {
                // A surviving exported owner retains the original source
                // charge on refusal. It never gets an unfunded new category.
                wanted = !r->current && !r->retained && !r->pending ? Class::Count
                    : closing && !r->current && !r->pending ? Class::RetirementDebt : r->charged;
            }
            if (wanted == Class::RetirementDebt && r->charged != wanted) {
                Q_ASSERT(r->debt.isValid());
                KisBackingBudgetDelta debt;
                delta(debt, r->charged, r->domain, -qint64(descriptor.minimumByteSize()));
                delta(debt, wanted, r->domain, qint64(descriptor.minimumByteSize()));
                r->debt.commit(debt);
            }
            if (wanted != Class::Count) r->charged = wanted;
            r->busy = r->settling = false; r->settlementNext = nullptr; r->settlementClass = Class::Count;
            if (wanted == Class::IdleCapacity && !r->idle) park(r);
            terminal = wanted == Class::RetirementDebt || wanted == Class::Count;
        }
        if (terminal) retire(r);
    }
}

void KisNativeResultProtocol::Impl::reclassifyUnselected(Resource *r) noexcept
{
    Resource *head = nullptr;
    { QMutexLocker lock(&gate); claimReleased(r, head); }
    settleReleased(head);
}

void KisNativeResultProtocol::Impl::cancelBody(Body *b) noexcept
{
    { QMutexLocker lock(&gate); if (b->asyncPending) { b->abandoned = true; return; } }
    if (!b->accepted) {
        finishProducer(*b);
        {
            QMutexLocker lock(&gate);
            for (auto &row : b->rows) if (auto *r = std::exchange(row.idleCandidate, nullptr)) {
                r->busy = false; park(r);
            }
        }
        Resource *released = nullptr;
        {
            QMutexLocker lock(&gate);
            for (auto &row : b->rows) if (row.ownsTarget && row.target) {
                Q_ASSERT(row.target->pending); --row.target->pending;
                claimReleased(row.target, released);
                row.target = nullptr; row.ownsTarget = false;
            }
        }
        settleReleased(released);
        QMutexLocker lock(&gate);
        for (auto &row : b->rows) if (row.page && row.page->writer == b) row.page->writer = nullptr;
        COUNT(cancelled, 1);
    }
    returnBody(b);
}

void KisNativeResultProtocol::Impl::finishProducer(Body &b) noexcept
{
    for (auto &row : b.rows) {
        row.writable.unpin(); row.writable.reset(); row.pixels = nullptr;
    }
    b.ready = true;
}

void KisNativeResultProtocol::Impl::finishRaw(void *opaque) noexcept
{
    auto *body = static_cast<Body *>(opaque);
    auto owner = std::move(body->deferredOwner);
    if (body->rawDirty && owner->acceptBody(*body, nullptr, nullptr)) owner->returnBody(body);
    else owner->cancelBody(body);
}

bool KisNativeResultProtocol::Impl::bindRows(Body &b, const QRect &rect, QString *why)
{
    const QRect tiles(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                      QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    const quint64 total = quint64(tiles.width()) * quint64(tiles.height());
    if (!total || total > std::numeric_limits<quint32>::max()) return false;
    b.rows.resize(size_t(total)); b.moves.reserve(size_t(total) * 2);
    Pages staged(std::less<Key>{}, pages.get_allocator());
    {
        QMutexLocker lock(&gate);
        size_t at = 0;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto it = pages.find({y, x});
            if (it != pages.end()) b.rows[at].page = &it->second;
        }
    }
    size_t at = 0;
    for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at)
        if (!b.rows[at].page) {
            auto it = staged.try_emplace({y, x}).first;
            it->second.key = {{1}, {x, y}};
        }
    {
        QMutexLocker lock(&gate);
        if (closing) return false;
        pages.merge(staged);
        at = 0;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto &row = b.rows[at];
            if (!row.page) row.page = &pages.find({y, x})->second;
            if (row.page->writer) { error(why, "same-page writer conflict"); return false; }
            row.page->writer = &b; row.before = row.page->current;
            row.local = rect.intersected(QRect(x * 64, y * 64, 64, 64)).translated(-x * 64, -y * 64);
        }
    }
    return true;
}

bool KisNativeResultProtocol::Impl::prepareAdapter(Body &b, QString *why)
{
    if (b.adapterReady) return true;
    size_t changed = 0; QRect range;
    for (const auto &row : b.rows) if (row.changed) {
        ++changed;
        if (row.after.resource) range |= QRect(row.page->key.page.column, row.page->key.page.row, 1, 1);
    }
    if (!b.edgeBudget && changed) { ensureEdges(changed); b.edgeBudget = changed; }
    if (!range.isEmpty() && !KisTiledDataManagerTest::extent(adapter).prepareTileRange(range)) {
        error(why, "adapter extent capacity refused; Ready retained"); return false;
    }
    auto &table = KisTiledDataManagerTest::table(adapter);
    for (auto &row : b.rows) if (row.changed) {
        const auto update = row.after.resource ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent;
        if (!row.adapter && !(row.location && row.after.resource))
            row.adapter = table.prepareTileUpdate(row.page->key.page.column, row.page->key.page.row, update, &row.location);
    }
    b.adapterReady = true; return true;
}

KisNativeResultProtocol::Impl::Selection KisNativeResultProtocol::Impl::viewSelection(const View &v, Key key) const
{
    if (v.explicitItems) {
        auto it = std::lower_bound(v.items.begin(), v.items.end(), key, [](const View::Item &a, Key b) { return a.key < b; });
        return it != v.items.end() && it->key == key ? it->selection : Selection{};
    }
    auto it = pages.find(key);
    return it != pages.end() ? selection(it->second, v.epoch) : Selection{};
}

bool KisNativeResultProtocol::Impl::prepareTargets(Body &b, const quint8 *data,
    const QRect &rect, qsizetype stride, const Faults *faults, QString *why)
{
    const int bpp = descriptor.format.pixelStride;
    const bool producer = !data;
    if (rect.isEmpty() || (!producer && !bounds(data, rect, stride, bpp))) { error(why, "invalid packed input"); return false; }
    if (!bindRows(b, rect, why)) return false;
    const auto payloadFor = [&](const Body::Row &row) -> const quint8 * {
        return producer ? nullptr : data + (qint64(row.page->key.page.row) * 64 + row.local.top() - rect.top()) * stride
            + (qint64(row.page->key.page.column) * 64 + row.local.left() - rect.left()) * bpp;
    };
    // Original rows retain original positions. Positive destination admission
    // is one actual budget group, while physical exclusion remains per backing.
    KisBackingBudgetDelta reuseChange;
    size_t reuseCandidates = 0;
    for (auto &row : b.rows) {
        auto &p = *row.page;
        const auto local = row.local;
        const auto *payload = payloadFor(row);
        const auto *source = row.before.resource;
        bool same = !producer;
        if (!producer && source) {
            const auto *before = static_cast<const quint8 *>(source->binding->acquireRead(source->handle.allocationIdentity(), false));
            if (!before) { error(why, "source pin refused"); return false; }
            for (int y = 0; y < local.height() && same; ++y)
                same = std::memcmp(before + ((local.y() + y) * 64 + local.x()) * bpp,
                                   payload + y * stride, size_t(local.width() * bpp)) == 0;
            source->binding->releaseRead();
        } else if (!producer) {
            const auto *pixel = descriptor.format.defaultPixel.constData();
            for (int y = 0; y < local.height() && same; ++y)
                for (int x = 0; x < local.width() && same; ++x)
                    same = std::memcmp(payload + y * stride + x * bpp, pixel, size_t(bpp)) == 0;
        }
        if (same) continue;
        QMutexLocker lock(&gate);
        if (p.nextGeneration == std::numeric_limits<quint64>::max()) return false;
        row.after.generation = p.nextGeneration++; row.changed = true;
        if (p.idle && !p.idle->busy) {
            auto *idle = p.idle; p.idle = idle->nextIdle;
            idle->nextIdle = nullptr; idle->idle = false; idle->busy = true;
            row.idleCandidate = idle; ++reuseCandidates;
            delta(reuseChange, idle->charged, idle->domain, -qint64(descriptor.minimumByteSize()));
            delta(reuseChange, Class::ActivePending, idle->domain, qint64(descriptor.minimumByteSize()));
        }
    }
    if (reuseCandidates) {
        auto fee = budget.reserveChange(reuseChange, why);
        if (!fee.isValid()) return false;
        KisBackingBudgetDelta succeeded;
        for (auto &row : b.rows) if (auto *idle = row.idleCandidate) {
            if (!(faults && faults->rejectPhysical)) row.writable = KisNativeResultProtocol::claimNative(
                idle->binding, idle->handle.allocation.generation, {row.page->key, {row.after.generation}});
            if (!row.writable.isValid()) continue;
            delta(succeeded, idle->charged, idle->domain, -qint64(descriptor.minimumByteSize()));
            delta(succeeded, Class::ActivePending, idle->domain, qint64(descriptor.minimumByteSize()));
        }
        // All unclaimed positive shares are returned before Fresh can reserve.
        fee.commit(succeeded);
        for (auto &row : b.rows) if (auto *idle = std::exchange(row.idleCandidate, nullptr)) {
            if (row.writable.isValid()) {
                QMutexLocker lock(&gate);
                idle->handle.version = {row.page->key, {row.after.generation}}; ++idle->handle.allocation.generation;
                idle->charged = Class::ActivePending; idle->pending = 1; idle->busy = false;
                row.target = idle; row.ownsTarget = true; COUNT(handoff, 1);
            } else {
                { QMutexLocker lock(&gate); idle->busy = false; }
                retire(idle);
            }
        }
    }
    size_t made = 0;
    for (auto &row : b.rows) if (row.changed) {
        const auto local = row.local;
        const auto *payload = payloadFor(row);
        auto *source = row.before.resource;
        const bool reused = row.target != nullptr;
        if (!row.target) {
            row.target = fresh(*row.page, {row.page->key, {row.after.generation}}, payload, stride,
                               source, local, why);
            if (!row.target) return false;
            row.ownsTarget = true;
            if (producer || local != QRect(0, 0, 64, 64)) row.writable = KisCpuWriteBindingReservation::acquire(
                row.target->binding, row.target->handle.allocationIdentity());
            if ((producer || local != QRect(0, 0, 64, 64)) && !row.writable.isValid()) return false;
        }
        auto &writable = row.writable;
        if (writable.isValid()) {
            auto *destination = static_cast<quint8 *>(writable.pinResident());
            if (!destination) destination = static_cast<quint8 *>(writable.materialize());
            if (!destination) return false;
            if (reused && (producer || local != QRect(0, 0, 64, 64))) {
                if (source) {
                    const void *before = source->binding->acquireRead(source->handle.allocationIdentity(), false);
                    if (!before) return false;
                    std::memcpy(destination, before, size_t(descriptor.minimumByteSize()));
                    source->binding->releaseRead();
                } else for (int i = 0; i < 4096; ++i)
                    std::memcpy(destination + i * bpp, descriptor.format.defaultPixel.constData(), size_t(bpp));
            }
            if (producer) {
                if (!source && !reused) for (int i = 0; i < 4096; ++i)
                    std::memcpy(destination + i * bpp, descriptor.format.defaultPixel.constData(), size_t(bpp));
                row.pixels = destination;
            } else {
                for (int y = 0; y < local.height(); ++y)
                    std::memcpy(destination + ((local.y() + y) * 64 + local.x()) * bpp,
                                payload + y * stride, size_t(local.width() * bpp));
                writable.unpin(); writable.reset();
            }
        }
        row.after.resource = row.target;
        if (faults && faults->rejectAfterTargets >= 0 && ++made == size_t(faults->rejectAfterTargets)) {
            error(why, "injected producer failure after real targets"); return false;
        }
    }
    b.ready = !producer;
    return true;
}

void KisNativeResultProtocol::Impl::buildPlan(Body &b)
{
    b.moves.clear(); b.cutEpoch = epoch + 1;
    // At most two facts per changed row. Sort/reduce the actual resources once,
    // so alias/restore share fees and physical gates instead of quadratic scans.
    const auto add = [&](Resource *r, int c, int h, int p, bool target) {
        if (r) b.moves.push_back({r, r->charged, r->charged, r->domain, r->domainRevision, c, h, p, target});
    };
    for (auto &row : b.rows) if (row.changed) {
        row.retain = retentionOwner(row.page->begin, b.cutEpoch);
        add(row.before.resource, -1, row.retain ? 1 : 0, 0, false);
        add(row.after.resource, 1, 0, row.ownsTarget ? -1 : 0, true);
    }
    std::sort(b.moves.begin(), b.moves.end(), [](const Move &a, const Move &z) {
        return std::less<Resource *>{}(a.resource, z.resource);
    });
    size_t out = 0;
    for (const auto &m : b.moves) {
        if (out && b.moves[out - 1].resource == m.resource) {
            auto &v = b.moves[out - 1]; v.current += m.current; v.retained += m.retained;
            v.pending += m.pending; v.target |= m.target;
        } else b.moves[out++] = m;
    }
    b.moves.resize(out);
    auto idleAllowance = budget.idleCapacityAllowance();
    for (auto &m : b.moves) {
        auto *r = m.resource;
        const int c = int(r->current) + m.current, h = int(r->retained) + m.retained, p = int(r->pending) + m.pending;
        Q_ASSERT(c >= 0 && h >= 0 && p >= 0);
        m.after = c ? Class::Current : h ? Class::RetainedHistory : p ? Class::ActivePending : Class::IdleCapacity;
        if (m.after == Class::IdleCapacity && m.before != Class::IdleCapacity) {
            if (idleAllowance >= descriptor.minimumByteSize()) idleAllowance -= descriptor.minimumByteSize();
            else m.after = Class::RetirementDebt; // consume the original prepaid terminal credit
        }
    }
}

void KisNativeResultProtocol::Impl::preparePlan(Body &b, KisBackingBudgetDelta &change)
{
    buildPlan(b);
    for (const auto &m : b.moves) if (m.before != m.after && m.after != Class::RetirementDebt) {
        delta(change, m.before, m.domain, -qint64(descriptor.minimumByteSize()));
        delta(change, m.after, m.domain, qint64(descriptor.minimumByteSize()));
    }
}

bool KisNativeResultProtocol::Impl::samePlan(Body &b) const
{
    if (closing || epoch == std::numeric_limits<quint64>::max()) return false;
    b.cutEpoch = epoch + 1; // unrelated pages do not invalidate funded facts
    for (const auto &row : b.rows) if (row.changed) {
        if (row.page->writer != &b || row.page->current.resource != row.before.resource
            || row.page->current.generation != row.before.generation) return false;
        const bool retainedNow = retentionOwner(row.page->begin, b.cutEpoch) != nullptr;
        if (retainedNow != (row.retain != nullptr)) return false;
    }
    for (const auto &m : b.moves) {
        auto *r = m.resource;
        if (r->busy || r->charged != m.before || r->domain != m.domain || r->domainRevision != m.revision) return false;
        const int c = int(r->current) + m.current, h = int(r->retained) + m.retained, p = int(r->pending) + m.pending;
        if (c < 0 || h < 0 || p < 0) return false;
        const auto wanted = c ? Class::Current : h ? Class::RetainedHistory : p ? Class::ActivePending : Class::IdleCapacity;
        if (wanted != m.after && !(wanted == Class::IdleCapacity && m.after == Class::RetirementDebt)) return false;
    }
    return true;
}

bool KisNativeResultProtocol::Impl::acceptBody(Body &b, const Faults *faults, QString *why)
try {
    { QMutexLocker lock(&gate); if (!b.ready || b.accepted || b.asyncPending || b.failed) return false; }
    const size_t changed = size_t(std::count_if(b.rows.begin(), b.rows.end(), [](const auto &r) { return r.changed; }));
    if (!changed && !b.defaultTile) {
        QMutexLocker lock(&gate);
        for (auto &r : b.rows) if (r.page->writer == &b) r.page->writer = nullptr;
        b.accepted = true; return true;
    }
    if (!prepareAdapter(b, why)) return false;
    KisBackingBudgetDelta change;
    {
        QMutexLocker lock(&gate);
        if (epoch == std::numeric_limits<quint64>::max()) return false;
        preparePlan(b, change);
    }
    auto fee = budget.reserveChange(change, why);
    if (!fee.isValid()) return false;
    if (faults && faults->beforeAccept) faults->beforeAccept();
    if (faults && faults->rejectAdapter) { error(why, "adapter preparation refused; Ready retained"); return false; }
    QWriteLocker adapterGate(&KisTiledDataManagerTest::gate(adapter));
    {
        QMutexLocker lock(&gate);
        if (!samePlan(b)) { COUNT(retried, 1); error(why, "actual final facts changed; Ready retained"); return false; }
        // Resource order follows canonical PageSlot order. No old Value,
        // metadata index, proof projection or publication entry is traversed.
        size_t locked = 0;
        int targetIndex = 0;
        const auto unlock = qScopeGuard([&] {
            for (size_t i = locked; i; --i) if (b.moves[i - 1].target)
                b.moves[i - 1].resource->binding->m_mutex.unlock();
        });
        for (auto &move : b.moves) {
            ++locked;
            if (!move.target) continue;
            auto *r = move.resource;
            r->binding->m_mutex.lock();
            if (!KisNativeResultProtocol::validateNative(r->binding.get(), r->handle)
                || (faults && (faults->rejectAccept || faults->rejectAcceptAt == targetIndex))) {
                error(why, "actual Resource validation refused; nothing installed"); return false;
            }
            ++targetIndex;
        }
        for (auto &row : b.rows) if (row.changed) {
            auto &p = *row.page;
            // Reconsume the actual owner; an equal-epoch capture/release does
            // not invalidate the already funded class plan.
            if (auto *v = retentionOwner(p.begin, b.cutEpoch)) {
                Q_ASSERT(freeEdges);
                auto *edge = freeEdges; freeEdges = edge->freeNext; --edgeFree;
                Q_ASSERT(b.edgeBudget && edgeReserved); --b.edgeBudget; --edgeReserved;
                edge->page = &p; edge->selection = row.before; edge->begin = p.begin; edge->end = b.cutEpoch;
                p.history.insert(*edge); linkEdge(edge, v);
                if (row.before.resource) ++row.before.resource->retained;
                COUNT(retainedEdges, 1);
            }
            if (row.before.resource) --row.before.resource->current;
            if (row.after.resource) ++row.after.resource->current;
            if (row.ownsTarget) --row.target->pending;
            p.current = row.after; p.begin = b.cutEpoch;
        }
        fee.commit(change);
        for (const auto &m : b.moves) {
            if (m.after == Class::RetirementDebt && m.before != m.after) {
                KisBackingBudgetDelta debt;
                delta(debt, m.before, m.domain, -qint64(descriptor.minimumByteSize()));
                delta(debt, Class::RetirementDebt, m.domain, qint64(descriptor.minimumByteSize()));
                m.resource->debt.commit(debt);
            }
            m.resource->charged = m.after;
            if (m.after == Class::IdleCapacity && !m.resource->idle) park(m.resource);
        }
        epoch = b.cutEpoch; b.accepted = true;
        if (b.defaultTile) { descriptor.format.defaultPixel = b.defaultPixel; ++defaultRevision; }
        COUNT(results, changed);
    }
    // Real adapter work stays inside the manager cut but outside S/B: legacy
    // Tile read takes its swap barrier before asking this bridge for selection.
    auto &table = KisTiledDataManagerTest::table(adapter);
    auto &extent = KisTiledDataManagerTest::extent(adapter);
    if (b.defaultTile) {
        KisTiledDataManagerTest::installDefault(adapter, reinterpret_cast<const quint8 *>(b.defaultPixel.constData()), b.defaultTile);
        KisTileHashTableConstIterator it(&table);
        while (!it.isDone()) {
            auto tile = it.tile();
            bool affected;
            { QMutexLocker lock(&gate); auto p = pages.find({tile->row(), tile->col()}); affected = p == pages.end() || !p->second.current.resource; }
            if (affected) tile->invalidatePageStoreReadCache();
            it.next();
        }
    }
    for (auto &row : b.rows) if (row.changed) {
        if (!row.after.resource) {
            if (table.removePreparedTile(row.adapter)) extent.notifyTileRemoved(row.page->key.page.column, row.page->key.page.row);
            continue;
        }
        KisTileSP installed;
        bool created = false;
        auto *tile = row.location;
        if (!tile) { installed = table.installPreparedTile(row.adapter, created); tile = installed.data(); }
        Q_ASSERT(tile);
        tile->setPageStoreBridge(this, false);
        tile->invalidatePageStoreReadCache();
        if (created) extent.notifyTileAdded(row.page->key.page.column, row.page->key.page.row);
        COUNT(adapterInstalled, 1);
    }
    adapterGate.unlock();
    {
        QMutexLocker lock(&gate);
        for (auto &row : b.rows) if (row.page->writer == &b) row.page->writer = nullptr;
    }
    for (const auto &m : b.moves) if (m.after == Class::RetirementDebt) retire(m.resource);
    return true;
} catch (const std::bad_alloc &) { error(why, "acceptance preparation capacity refused; Ready retained"); return false; }

void KisNativeResultProtocol::Impl::releaseView(View *v) noexcept
{
    Resource *released = nullptr;
    Views::node_type releasedView;
    {
        QMutexLocker lock(&gate);
        if (v->explicitItems) {
            for (const auto &item : v->items) if (auto *r = item.selection.resource) {
                Q_ASSERT(r->retained); --r->retained; claimReleased(r, released);
            }
        } else {
            releasedView = views.extract({v->epoch, v->serial}); v->registered = false;
            while (v->first) {
                auto *e = v->first; unlinkEdge(e);
                if (auto *next = retentionOwner(e->begin, e->end)) { linkEdge(e, next); continue; }
                if (auto *r = e->selection.resource) { --r->retained; claimReleased(r, released); }
                e->page->history.erase(e->page->history.iterator_to(*e));
                e->freeNext = freeEdges; freeEdges = e; ++edgeFree;
            }
        }
    }
    settleReleased(released); finishClosing();
}

void KisNativeResultProtocol::Impl::finishClosing() noexcept
{
    Resource *released = nullptr;
    {
        QMutexLocker lock(&gate);
        if (!closing || active) return;
        if (views.empty()) {
            for (auto &p : pages) if (auto *r = p.second.current.resource) {
                if (closedRootHeld) --r->retained; else --r->current;
                p.second.current = {}; claimReleased(r, released);
            }
            closedRootHeld = false;
        } else if (!closedRootHeld) {
            // The closed graph is an actual retained root. Keep its existing
            // page index for views, change the physical responsibility once.
            for (auto &p : pages) if (auto *r = p.second.current.resource) {
                --r->current; ++r->retained; claimReleased(r, released);
            }
            closedRootHeld = true;
        }
    }
    settleReleased(released);
}

bool KisNativeResultProtocol::Impl::readSelection(Selection selected, quint8 *out,
    const QRect &local, qsizetype stride, const QByteArray &defaultPixel, QString *why) const
{
    const int bpp = descriptor.format.pixelStride;
    if (!selected.resource) {
        for (int y = 0; y < local.height(); ++y) for (int x = 0; x < local.width(); ++x)
            std::memcpy(out + y * stride + x * bpp, defaultPixel.constData(), size_t(bpp));
        return true;
    }
    auto *r = selected.resource;
    const auto *data = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
    if (!data) { error(why, "real historical/current read pin refused"); return false; }
    for (int y = 0; y < local.height(); ++y)
        std::memcpy(out + y * stride, data + ((local.y() + y) * 64 + local.x()) * bpp, size_t(local.width() * bpp));
    r->binding->releaseRead();
    return true;
}

TileLease KisNativeResultProtocol::Impl::acquireTile(qint32 col, qint32 row, Access access, TileLease *cache)
{
    if (access == Access::Write) {
        std::unique_ptr<WriteLease> result;
        Body *body = nullptr;
        auto cancel = qScopeGuard([&] { if (body) cancelBody(body); });
        try {
            result = std::make_unique<WriteLease>();
            body = checkout();
            if (!body || !prepareTargets(*body, nullptr, QRect(col * 64, row * 64, 64, 64), 0, nullptr, nullptr)) return {};
            result->owner = shared_from_this(); result->body = std::exchange(body, nullptr);
            return TileLease(result.release());
        } catch (const std::bad_alloc &) { return {}; }
    }
    std::shared_ptr<KisCpuResidentBinding> binding;
    KisReplicaHandle expected;
    KisTileData *tile = nullptr;
    {
        QMutexLocker lock(&gate);
        auto it = pages.find({row, col});
        if (it != pages.end() && it->second.current.resource) {
            auto *r = it->second.current.resource;
            binding = r->binding; expected = r->handle; tile = r->tile;
        }
    }
    if (!binding) {
        auto result = std::make_unique<DefaultLease>(); result->owner = shared_from_this();
        result->tile = KisTiledDataManagerTest::table(adapter).refAndFetchDefaultTileData();
        { QMutexLocker lock(&gate); result->revision = defaultRevision; }
        return result->repinRead() == KisTilePageStoreLease::ReadPinResult::Ready ? TileLease(result.release()) : TileLease{};
    }
    std::unique_ptr<ReadLease> result;
    if (cache && *cache) {
        auto *old = dynamic_cast<ReadLease *>(cache->get());
        if (old && old->binding == binding && old->expected == expected)
            result.reset(static_cast<ReadLease *>(cache->release()));
    }
    if (!result) result = std::make_unique<ReadLease>();
    result->owner = shared_from_this(); result->binding = std::move(binding); result->expected = expected; result->tile = tile;
    return result->repinRead() == KisTilePageStoreLease::ReadPinResult::Ready ? TileLease(result.release()) : TileLease{};
}

KisNativeResultProtocol::KisNativeResultProtocol(KisTiledDataManager &adapter,
    const KisPageAllocationDescriptor &descriptor, const KisPageBackingLimits &limits)
    : d(std::allocate_shared<Impl>(Alloc<Impl>{}, adapter, descriptor, limits)) { d->initialize(); }

KisNativeResultProtocol::~KisNativeResultProtocol() { close(); }
KisNativeResultProtocol::Prepared::~Prepared() { if (body) owner->cancelBody(body); }
KisNativeResultProtocol::Prepared::Prepared(Prepared &&other) noexcept
    : owner(std::move(other.owner)), body(std::exchange(other.body, nullptr)) {}
KisNativeResultProtocol::Prepared &KisNativeResultProtocol::Prepared::operator=(Prepared &&other) noexcept
{
    if (this != &other) {
        if (body) owner->cancelBody(body);
        owner = std::move(other.owner); body = std::exchange(other.body, nullptr);
    }
    return *this;
}

KisNativeResultProtocol::Prepared KisNativeResultProtocol::prepareWrite(
    const quint8 *bytes, const QRect &pixels, qsizetype stride, QString *why, const Faults *faults)
try {
    if (!bytes) { error(why, "null packed input"); return {}; }
    Prepared result;
    result.owner = d; result.body = d->checkout();
    if (!result.body || !d->prepareTargets(*result.body, bytes, pixels, stride, faults, why)) return {};
    return result;
} catch (const std::bad_alloc &) { error(why, "real managed storage admission refused"); return {}; }

bool KisNativeResultProtocol::accept(Prepared &p, QString *why, const Faults *faults)
{
    if (p.owner != d || !p.body || !d->acceptBody(*p.body, faults, why)) return false;
    d->returnBody(std::exchange(p.body, nullptr)); p.owner.reset(); return true;
}

KisNativeResultProtocol::Prepared KisNativeResultProtocol::prepareProducer(const QRect &pixels, QString *why)
try {
    Prepared p; p.owner = d; p.body = d->checkout();
    if (!p.body || !d->prepareTargets(*p.body, nullptr, pixels, 0, nullptr, why)) return {};
    return p;
} catch (const std::bad_alloc &) { error(why, "producer storage refused"); return {}; }

quint8 *KisNativeResultProtocol::producerPage(Prepared &p, qint32 col, qint32 row, quint32 *stride)
{
    if (p.owner != d || !p.body || p.body->ready || p.body->failed) return nullptr;
    for (auto &r : p.body->rows) if (r.page->key.page.column == col && r.page->key.page.row == row) {
        if (stride) *stride = 64 * d->descriptor.format.pixelStride;
        return r.pixels;
    }
    return nullptr;
}

bool KisNativeResultProtocol::finishProducer(Prepared &p, bool success, QString *)
{
    if (p.owner != d || !p.body || p.body->ready || p.body->asyncPending) return false;
    if (!success) { cancel(p); return false; }
    d->finishProducer(*p.body); return true;
}

bool KisNativeResultProtocol::ProducerCompletion::finish(bool success)
{ return d && d->finish(success); }
KisCompletionTicket KisNativeResultProtocol::ProducerCompletion::ticket() const
{ return d ? d->ticket : KisCompletionTicket{}; }

KisNativeResultProtocol::ProducerCompletion KisNativeResultProtocol::deferProducer(Prepared &p, QString *why)
try {
    if (p.owner != d || !p.body || p.body->ready || p.body->asyncPending || p.body->failed) return {};
    ProducerCompletion completion;
    completion.d = std::allocate_shared<Async>(Alloc<Async>::retained(&d->budget)); completion.d->owner = d;
    {
        QMutexLocker setup(&d->asyncGate);
        if (!d->asyncSource) d->asyncSource = d->completions->registerSource(KisCompletionDomain::HostLogical);
        if (!d->asyncSource) return {};
        completion.d->ticket = d->completions->allocatePending(d->asyncSource);
        if (!completion.d->ticket.isValid()) return {};
    }
    auto cancelTicket = qScopeGuard([&] {
        if (!completion.d->body) d->completions->completePrepared(completion.d->ticket, KisCompletionStatus::Cancelled);
    });
    KisBackingBudgetDelta change;
    for (const auto &row : p.body->rows) if (row.target) {
        delta(change, row.target->charged, row.target->domain, -qint64(d->descriptor.minimumByteSize()));
        delta(change, Class::InFlight, row.target->domain, qint64(d->descriptor.minimumByteSize()));
    }
    auto fee = d->budget.reserveChange(change, why); if (!fee.isValid()) return {};
    {
        QMutexLocker lock(&d->gate);
        fee.commit(change);
        for (const auto &row : p.body->rows) if (row.target) row.target->charged = Class::InFlight;
        p.body->asyncPending = true; completion.d->body = p.body;
    }
    cancelTicket.dismiss(); return completion;
} catch (const std::bad_alloc &) { error(why, "asynchronous completion capacity refused"); return {}; }

void KisNativeResultProtocol::cancel(Prepared &p) noexcept
{ if (p.body) p.owner->cancelBody(std::exchange(p.body, nullptr)); p.owner.reset(); }

bool KisNativeResultProtocol::write(const quint8 *bytes, const QRect &pixels, qsizetype stride,
                                  QString *why, const Faults *faults)
{ auto prepared = prepareWrite(bytes, pixels, stride, why, faults); return prepared && accept(prepared, why, faults); }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::capture(QString *why)
try {
    Snapshot s;
    s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    Impl::Views staged(std::less<std::pair<quint64, quint64>>{}, d->views.get_allocator());
    staged.emplace(std::pair<quint64, quint64>{0, 0}, s.d.get());
    auto node = staged.extract(staged.begin());
    QMutexLocker lock(&d->gate);
    if (d->closing) return {};
    s.d->epoch = d->epoch; s.d->serial = ++d->viewSerial;
    s.d->defaultPixel = d->descriptor.format.defaultPixel;
    node.key() = {s.d->epoch, s.d->serial}; d->views.insert(std::move(node));
    s.d->registered = true; return s;
} catch (const std::bad_alloc &) { error(why, "snapshot capacity refused"); return {}; }

bool KisNativeResultProtocol::restore(const Snapshot &s, QString *why)
try {
    if (!s.d || s.d->owner != d || s.d->explicitItems) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; b.restore = true;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate);
        if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &page = item.second; auto &row = b.rows[n++]; row.page = &page;
            if (page.writer) return false;
            page.writer = &b; row.before = page.current; row.after = d->viewSelection(*s.d, item.first);
            row.target = row.after.resource;
            row.changed = row.before.resource != row.after.resource || row.before.generation != row.after.generation;
        }
    }
    auto &table = KisTiledDataManagerTest::table(d->adapter);
    for (auto &row : b.rows) if (row.changed) {
        row.adapter = table.prepareTileUpdate(row.page->key.page.column, row.page->key.page.row,
            row.after.resource ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent, &row.location);
    }
    if (s.d->defaultPixel != d->descriptor.format.defaultPixel && !d->prepareDefault(b, s.d->defaultPixel, why)) return false;
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "restore preparation refused"); return false; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::captureRange(const QRect &rect, QString *why)
try {
    if (rect.isEmpty()) return {};
    const QRect tiles(QPoint(floorTile(rect.left()), floorTile(rect.top())), QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    Snapshot s; s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    s.d->range = rect; s.d->items.reserve(size_t(tiles.width()) * size_t(tiles.height()));
    QMutexLocker lock(&d->gate);
    if (d->closing) return {};
    s.d->defaultPixel = d->descriptor.format.defaultPixel;
    for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x) {
        auto it = d->pages.find({y, x});
        const auto selected = it != d->pages.end() ? it->second.current : Impl::Selection{};
        s.d->items.push_back({{y, x}, selected});
        if (selected.resource) ++selected.resource->retained;
    }
    s.d->explicitItems = true;
    return s;
} catch (const std::bad_alloc &) { error(why, "range capability capacity refused"); return {}; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::capturePrepared(const Prepared &p, QString *why)
try {
    if (p.owner != d || !p.body || !p.body->ready || p.body->failed || p.body->asyncPending) return {};
    Snapshot s; s.d = std::allocate_shared<View>(Alloc<View>::retained(&d->budget), &d->budget); s.d->owner = d;
    s.d->items.reserve(p.body->rows.size());
    s.d->defaultPixel = d->descriptor.format.defaultPixel;
    Vec<Impl::Move> claims{Alloc<Impl::Move>(&d->budget)};
    claims.reserve(p.body->rows.size());
    auto undo = qScopeGuard([&] {
        QMutexLocker lock(&d->gate);
        for (const auto &m : claims) m.resource->busy = false;
    });
    KisBackingBudgetDelta change;
    {
        QMutexLocker lock(&d->gate);
        for (const auto &row : p.body->rows) {
            auto selected = row.changed ? row.after : row.before;
            auto *r = selected.resource;
            if (r && r->busy) return {};
            if (r && r->charged == Class::ActivePending) {
                r->busy = true;
                claims.push_back({r, r->charged, Class::RetainedHistory, r->domain, r->domainRevision});
                delta(change, Class::ActivePending, r->domain, -qint64(d->descriptor.minimumByteSize()));
                delta(change, Class::RetainedHistory, r->domain, qint64(d->descriptor.minimumByteSize()));
            }
            s.d->items.push_back({{row.page->key.page.row, row.page->key.page.column}, selected});
            s.d->range |= QRect(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
        }
    }
    auto fee = d->budget.reserveChange(change, why);
    if (!fee.isValid()) return {};
    {
        QMutexLocker lock(&d->gate);
        fee.commit(change);
        for (const auto &m : claims) m.resource->charged = Class::RetainedHistory;
        for (const auto &item : s.d->items) if (auto *r = item.selection.resource) ++r->retained;
    }
    s.d->explicitItems = true;
    return s;
} catch (const std::bad_alloc &) { error(why, "private capability capacity refused"); return {}; }

bool KisNativeResultProtocol::alias(const Snapshot &s, const QRect &source, const QPoint &destination, QString *why)
try {
    if (!s.d || s.d->owner != d || source.isEmpty() || source.x() % 64 || source.y() % 64
        || source.width() % 64 || source.height() % 64 || destination.x() % 64 || destination.y() % 64
        || (s.d->explicitItems && !s.d->range.contains(source))) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    const QRect target(destination, source.size());
    auto &b = *p.body; if (!d->bindRows(b, target, why)) return false;
    {
        QMutexLocker lock(&d->gate);
        for (auto &row : b.rows) {
            const Key key{row.page->key.page.row + (source.y() - destination.y()) / 64,
                          row.page->key.page.column + (source.x() - destination.x()) / 64};
            const auto selected = d->viewSelection(*s.d, key);
            if (selected.resource == row.before.resource
                && (selected.resource || s.d->defaultPixel == d->descriptor.format.defaultPixel)) continue;
            if (row.page->nextGeneration == std::numeric_limits<quint64>::max()) return false;
            row.after = {selected.resource, row.page->nextGeneration++}; row.target = selected.resource; row.changed = true;
        }
    }
    Vec<quint8> constant{Alloc<quint8>(&d->budget)};
    Impl::Resource *virtualDefault = nullptr;
    for (auto &row : b.rows) if (row.changed && !row.after.resource) {
        if (s.d->defaultPixel != d->descriptor.format.defaultPixel) {
            if (!virtualDefault) {
                const auto bpp = d->descriptor.format.pixelStride;
                constant.resize(size_t(d->descriptor.minimumByteSize()));
                for (int i = 0; i < 4096; ++i) std::memcpy(constant.data() + i * bpp, s.d->defaultPixel.constData(), bpp);
                virtualDefault = d->fresh(*row.page, {row.page->key, {row.after.generation}}, constant.data(), 64 * bpp,
                                           nullptr, QRect(0, 0, 64, 64), why);
                if (!virtualDefault) return false;
                row.ownsTarget = true;
            }
            row.target = row.after.resource = virtualDefault;
        } else row.adapter = KisTiledDataManagerTest::table(d->adapter).prepareTileUpdate(
            row.page->key.page.column, row.page->key.page.row, KisTileHashTable::Update::Absent, &row.location);
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "alias preparation refused"); return false; }

bool KisNativeResultProtocol::remove(const QRect &pixels, QString *why)
try {
    if (pixels.isEmpty() || pixels.x() % 64 || pixels.y() % 64 || pixels.width() % 64 || pixels.height() % 64) return false;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; if (!d->bindRows(b, pixels, why)) return false;
    for (auto &row : b.rows) if (row.before.resource) {
        row.changed = true; row.after = {};
        row.adapter = KisTiledDataManagerTest::table(d->adapter).prepareTileUpdate(
            row.page->key.page.column, row.page->key.page.row, KisTileHashTable::Update::Absent, &row.location);
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "remove preparation refused"); return false; }

bool KisNativeResultProtocol::Impl::prepareDefault(Body &b, const QByteArray &pixel, QString *why)
{
    if (pixel.size() != descriptor.format.pixelStride) return false;
    {
        QMutexLocker lock(&gate);
        if (active != 1 || closing || defaultRevision == std::numeric_limits<quint64>::max()) {
            error(why, "default change conflicts with active result work"); return false;
        }
        defaultWriter = &b;
    }
    b.defaultPixel = pixel;
    b.defaultTile = KisTileDataStore::instance()->createDefaultTileData(pixel.size(), reinterpret_cast<const quint8 *>(pixel.constData()));
    if (!b.defaultTile) { error(why, "real default storage refused"); return false; }
    b.defaultTile->ref(); return true;
}

bool KisNativeResultProtocol::setDefaultPixel(const QByteArray &pixel, QString *why)
try {
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    { QMutexLocker lock(&d->gate); if (pixel == d->descriptor.format.defaultPixel) return true; }
    if (!d->prepareDefault(*p.body, pixel, why)) return false;
    p.body->ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "default metadata refused"); return false; }

std::unique_ptr<KisNativeResultProtocol> KisNativeResultProtocol::cloneTo(KisTiledDataManager &adapter, QString *why)
try {
    auto view = capture(why); if (!view) return {};
    Vec<Key> keys{Alloc<Key>(&d->budget)};
    size_t count;
    { QMutexLocker lock(&d->gate); count = d->pages.size(); }
    keys.reserve(count);
    {
        QMutexLocker lock(&d->gate);
        if (count != d->pages.size()) { error(why, "clone page index changed; retry capture"); return {}; }
        for (const auto &p : d->pages) if (d->selection(p.second, view.d->epoch).resource) keys.push_back(p.first);
    }
    auto branchDescriptor = d->descriptor; branchDescriptor.format.defaultPixel = view.d->defaultPixel;
    auto result = std::make_unique<KisNativeResultProtocol>(adapter, branchDescriptor);
    Prepared p; p.owner = result->d; p.body = result->d->checkout(); if (!p.body) return {};
    auto &b = *p.body;
    b.rows.resize(keys.size()); b.moves.reserve(2 * keys.size());
    using Imported = std::map<Impl::Resource *, Impl::Resource *, std::less<Impl::Resource *>,
        Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>>;
    Imported imported(std::less<Impl::Resource *>{}, Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>(&result->d->budget));
    // This branch is private until return. Alias topology stays physical once;
    // all selected pages share one visibility cut, not P little transactions.
    for (size_t index = 0; index < keys.size(); ++index) {
        const auto key = keys[index];
        auto &page = result->d->pages.try_emplace(key).first->second;
        page.key = {{1}, {key.second, key.first}}; page.writer = &b;
        auto &row = b.rows[index]; row.page = &page;
        Impl::Selection selected;
        { QMutexLocker lock(&d->gate); selected = d->viewSelection(*view.d, key); }
        auto *r = selected.resource;
        auto node = imported.try_emplace(r, nullptr).first;
        const auto gen = page.nextGeneration++;
        if (!node->second) {
            if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return {};
            auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
            auto source = result->d->provider->captureCompletedTileSource(d->descriptor, r->tile, why);
            if (!source) return {};
            row.target = result->d->fresh(page, {page.key, {gen}}, nullptr, 0, nullptr,
                                          QRect(0, 0, 64, 64), why, source);
            if (!row.target) return {};
            row.ownsTarget = true; node->second = row.target;
        } else row.target = node->second;
        row.changed = true; row.after = {row.target, gen};
    }
    b.ready = true;
    if (!result->accept(p, why)) return {};
    return result;
} catch (const std::bad_alloc &) { error(why, "branch native alias capacity refused"); return {}; }

bool KisNativeResultProtocol::read(const Snapshot &s, quint8 *bytes, const QRect &pixels,
                                  qsizetype stride, QString *why) const
{ return s.d && s.d->owner == d && s.read(bytes, pixels, stride, why); }

bool KisNativeResultProtocol::Snapshot::read(quint8 *bytes, const QRect &pixels,
                                            qsizetype stride, QString *why) const
{
    if (!d || !bounds(bytes, pixels, stride, d->owner->descriptor.format.pixelStride)
        || (d->explicitItems && !d->range.contains(pixels))) return false;
    const int bpp = d->owner->descriptor.format.pixelStride;
    for (int y = floorTile(pixels.top()); y <= floorTile(pixels.bottom()); ++y)
        for (int x = floorTile(pixels.left()); x <= floorTile(pixels.right()); ++x) {
            Impl::Selection selected;
            { QMutexLocker lock(&d->owner->gate); selected = d->owner->viewSelection(*d, {y, x}); }
            const QRect hit = pixels.intersected(QRect(x * 64, y * 64, 64, 64));
            if (!d->owner->readSelection(selected, bytes + qint64(hit.y() - pixels.y()) * stride + (hit.x() - pixels.x()) * bpp,
                                  hit.translated(-x * 64, -y * 64), stride, d->defaultPixel, why)) return false;
        }
    return true;
}

bool KisNativeResultProtocol::readCurrent(quint8 *bytes, const QRect &pixels, qsizetype stride, QString *why) const
{
    if (!bounds(bytes, pixels, stride, d->descriptor.format.pixelStride)) return false;
    // Same manager read protection as the public packed-read contract. A raw
    // Tile may still pin an older selection, so its cached pixels cannot be
    // the authority for this new current read. Consume PageSlot directly.
    QReadLocker adapterGate(&KisTiledDataManagerTest::gate(d->adapter));
    const auto bpp = d->descriptor.format.pixelStride;
    for (int y = floorTile(pixels.top()); y <= floorTile(pixels.bottom()); ++y)
        for (int x = floorTile(pixels.left()); x <= floorTile(pixels.right()); ++x) {
            Impl::Selection selected;
            { QMutexLocker lock(&d->gate); if (d->closing) return false; auto it = d->pages.find({y, x}); if (it != d->pages.end()) selected = it->second.current; }
            const QRect hit = pixels.intersected(QRect(x * 64, y * 64, 64, 64));
            if (!d->readSelection(selected, bytes + qint64(hit.y() - pixels.y()) * stride + (hit.x() - pixels.x()) * bpp,
                hit.translated(-x * 64, -y * 64), stride, d->descriptor.format.defaultPixel, why)) return false;
        }
    return true;
}

bool KisNativeResultProtocol::configureLimits(const KisPageBackingLimits &limits, QString *why)
{ return d->budget.configureLimits(limits, why); }

bool KisNativeResultProtocol::swapIdleForTest(QString *why)
{
    Impl::Resource *chosen = nullptr;
    { QMutexLocker lock(&d->gate); for (auto &p : d->pages) if (p.second.idle) { chosen = p.second.idle; break; } }
#ifdef NR104_CENSUS
    qWarning("104 swap candidate present=%d resident=%d", bool(chosen), chosen && chosen->tile->data());
#endif
    if (!chosen || !KisTileDataStore::instance()->trySwapTileData(chosen->tile)) {
        error(why, "real swap refused"); return false;
    }
    return true;
}

void KisNativeResultProtocol::retryRetirements()
{ for (auto &r : d->resources) if (r.physical && r.terminal) d->reclassifyUnselected(&r); }

bool KisNativeResultProtocol::close(QString *why)
{
    if (!d) return true;
    {
        QMutexLocker lock(&d->gate);
        if (!d->closing) {
            d->closing = true;
            for (auto &p : d->pages) {
                p.second.idle = nullptr;
            }
            for (auto &r : d->resources) { r.idle = false; r.nextIdle = nullptr; }
        }
    }
    // Manager wrappers release dormant native leases before final real free.
    { QWriteLocker adapterGate(&KisTiledDataManagerTest::gate(d->adapter));
      KisTiledDataManagerTest::bridge(d->adapter, nullptr);
      KisTiledDataManagerTest::table(d->adapter).clear(); KisTiledDataManagerTest::extent(d->adapter).clear(); }
    d->finishClosing();
    for (auto &r : d->resources) if (r.physical && !r.current && !r.retained && !r.pending) d->retire(&r);
    QMutexLocker lock(&d->gate);
    if (d->active || d->domainMoves) { error(why, "closing; original work still owns its funded tail"); return false; }
    return true;
}

KisNativeResultProtocol::Counts KisNativeResultProtocol::counts() const { QMutexLocker lock(&d->gate); return d->counts; }
KisPageBackingUsage KisNativeResultProtocol::usage() const { return d->budget.usage(); }
