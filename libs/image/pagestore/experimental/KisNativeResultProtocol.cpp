#ifdef NR104_INSTALL_AUDIT
#include <dlfcn.h>
#include <cstdio>
#endif
#include "KisPixelWriteCursor.h"
#include <QRegion>
#include <QThread>
#include <boost/range/iterator_range.hpp>
/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultProtocol.h"
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
#ifdef NR104_INSTALL_AUDIT
struct NativeInstallAudit {
    struct Counts { size_t allocations, bytes, frees; };
    void (*start)() = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "auditStart"));
    Counts (*stop)() = reinterpret_cast<Counts (*)()>(dlsym(RTLD_DEFAULT, "auditStop"));
    void (*stack)() = reinterpret_cast<void (*)()>(dlsym(RTLD_DEFAULT, "auditStack"));
    bool active = false;
    NativeInstallAudit() { if (!start || !stop || !stack) qFatal("Native install audit requires the actual allocator interposer"); }
    void begin() { active = true; start(); }
    void end() {
        if (!std::exchange(active, false)) return;
        const auto c = stop();
        if (c.allocations || c.frees) { stack(); qFatal("NATIVE_INSTALL_AUDIT allocations=%zu bytes=%zu frees=%zu", c.allocations, c.bytes, c.frees); }
    }
    ~NativeInstallAudit() { end(); }
};
#endif
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

thread_local KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::iteratorHead = nullptr;

struct KisNativeResultProtocol::Impl::WriteLease final : KisTilePageStoreLease {
    std::shared_ptr<Impl> owner;
    Body *body = nullptr;
    KisTileData *tile = nullptr;
    bool writes = true, fixedPinned = false;
    ~WriteLease() override { finish(); }
    KisTileData *tileData() const override { return body || fixedPinned ? tile : nullptr; }
    bool writable() const override { return writes && body && !body->failed; }
    void markDirty() override { if (body && writes) body->rawDirty = true; }
    bool reusableReadCache() const override { return !body; }
    bool readPinned() const override { return fixedPinned; }
    ReadPinResult repinRead() override {
        // A public raw Tile already owns this exact COW backing. The original
        // lease keeps its bridge alive, without rebuilding a read capability.
        if (!fixedPinned) fixedPinned = tile->blockSwapping();
        return fixedPinned ? ReadPinResult::Ready : ReadPinResult::Unavailable;
    }
    bool finish(TileLeaseRelease *afterUnlock = nullptr) override {
        if (!body) { if (std::exchange(fixedPinned, false)) tile->unblockSwapping(); return true; }
        auto *b = std::exchange(body, nullptr);
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
        bool resume;
        { QMutexLocker lock(&owner->gate); resource->busy = false; --owner->domainMoves; resume = resource->terminal; }
        if (resume) owner->deferRetirement(resource);
    }
    void commit(quint64 revision) noexcept override {
        if (!resource) return;
        auto *r = resource;
        bool resume;
        {
            QMutexLocker lock(&owner->gate);
            Q_ASSERT(r->busy && r->domain == source && revision != r->domainRevision);
            fee.commit(change);
            // The actual existing retirement credit follows this allocation;
            // domain movement does not create a second physical obligation.
            if (r->debt.isValid()) owner->budget.moveRetirementHeadroom(
                r->debt.cookie, source, target, owner->descriptor.minimumByteSize());
            r->domain = target; r->domainRevision = revision;
            r->busy = false; --owner->domainMoves; resource = nullptr; resume = r->terminal;
        }
        if (resume) owner->deferRetirement(r);
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
        auto undo = qScopeGuard([&] {
            if (!r) return;
            bool resume;
            { QMutexLocker lock(&p->gate); r->busy = false; --p->domainMoves; resume = r->terminal; }
            if (resume) p->deferRetirement(r);
        });
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
    if (!descriptor.isValid() || descriptor.pageExtent != QSize(64, 64)
        || descriptor.format.pixelStride > std::numeric_limits<qint32>::max() / 4096
        || !budget.configureSharedNonPayloadBudget(parent, &why)) throw std::runtime_error(why.toStdString());
    completions = std::allocate_shared<KisCompletionRegistry>(Alloc<KisCompletionRegistry>(&budget), parent, Alloc<KisCompletionRegistry>(&budget));
    provider = std::allocate_shared<KisTiles3PageReplicaProvider>(Alloc<KisTiles3PageReplicaProvider>(&budget), Alloc<KisTiles3PageReplicaProvider>(&budget));
    KisCpuResidentReplicaProviderConfig config;
    static std::atomic<quint64> providerIdentity{0x5000000000000000ULL};
    auto identity = providerIdentity.load(std::memory_order_relaxed);
    do { if (identity == std::numeric_limits<quint64>::max()) throw std::overflow_error("native provider identity exhausted"); }
    while (!providerIdentity.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed));
    config.provider = {identity}; config.providerEpoch = {1}; config.budgetBytes = providerBytes;
    if (!provider->configure(config, completions, &why, parent)) throw std::runtime_error(why.toStdString());
    admission = std::allocate_shared<Admission>(Alloc<Admission>::retained(&budget), shared_from_this());
    if (!provider->registerBackingDomainAdmission(admission, &why)) throw std::runtime_error(why.toStdString());
    const auto weak = weak_from_this();
    terminalJob = kisPreparePageStoreReclamation([this] { runTerminals(); }, &budget,
        [](void *p) { static_cast<Impl *>(p)->finishTerminalJob(); }, this);
    terminalDelay = KisPageReclamationDelay(KisPageReadinessCallback([weak] {
        if (auto owner = weak.lock()) {
            QMutexLocker lock(&owner->terminalGate);
            owner->terminalDelayed = false;
            owner->startTerminalLocked();
        }
    }, &budget), &budget);
    failedReadScope = std::allocate_shared<KisPageStoreIteratorReadScope>(Alloc<KisPageStoreIteratorReadScope>(&budget));
    blocks.reserve(8);
    if (KisNativeResultAdapter::hasBackend(adapter)) throw std::runtime_error("native protocol requires an external selection adapter");
    if (KisNativeResultAdapter::pixelSize(adapter) != descriptor.format.pixelStride || KisNativeResultAdapter::table(adapter).numTiles())
        throw std::runtime_error("native protocol requires an empty compatible adapter");
    if (std::memcmp(adapter.defaultPixel(), descriptor.format.defaultPixel.constData(), descriptor.format.pixelStride)) {
        auto *tile = KisTileDataStore::instance()->createDefaultTileData(descriptor.format.pixelStride,
            reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()));
        if (!tile) throw std::bad_alloc();
        tile->ref();
        KisNativeResultAdapter::installDefault(adapter, reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()), tile);
        tile->deref();
    }
    defaultBacking = KisNativeResultAdapter::table(adapter).refAndFetchDefaultTileData();
    KisNativeResultAdapter::bridge(adapter, this);
}

KisNativeResultProtocol::Impl::~Impl()
{
    // Facade/capabilities have already stopped every new access. Real storage
    // goes away before its local and process metadata allocators are destroyed.
    Q_ASSERT(active == 0 && views.empty() && !terminalRunning);
    // A queued/running job retains Impl. Waiting physical capabilities also
    // retain it through their original owner; no terminal self-owner exists.
    // At final destruction cancel cold notifications before direct release.
    terminalDelay.reset();
    for (auto &r : resources) {
        r.terminalSubscription.reset();
        r.terminalRegistered = r.terminalQueued = r.terminalProcessing = false;
        r.terminalNext = nullptr;
    }
    terminalHead = terminalTail = nullptr; terminalCount = 0;
    for (auto &p : pages) {
        if (p.second.current.resource) {
            if (closedRootHeld) --p.second.current.resource->retained;
            else --p.second.current.resource->current;
        }
        p.second.current = {}; p.second.idle = nullptr;
    }
    for (auto &r : resources) { r.idle = false; r.nextIdle = nullptr; }
    for (auto &r : resources) if (r.physical) { const bool released = retireNow(&r); Q_ASSERT(released); }
    Q_ASSERT(physical.empty());
    if (defaultBacking) defaultBacking->deref();
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
    Q_ASSERT(!b->clients && !b->borrows);
    b->iterator = b->raw = b->revoked = false; b->thread = nullptr; b->iteratorOwner = nullptr; b->iteratorNext = nullptr;
    b->rows.clear(); b->moves.clear(); b->defaultReaders.clear(); b->history.reset();
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
    const auto limit = budget.maxTransientVersionsPerPage();
    { QMutexLocker lock(&gate); if (page.transient >= limit) { error(why, "page transient-result budget exhausted"); return nullptr; } }
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
    KisPageStoreDiagnosticTimer preparing(nullptr, KisPageStoreDiagnosticPhase::WriteProviderPrepare, 1);
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
    { QMutexLocker lock(&gate); r->transient = true; ++page.transient; }
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
    if (!r) return;
    {
        QMutexLocker notification(&terminalGate);
        if (!r->physical.load(std::memory_order_acquire)) return;
        if (r->terminalRegistered) { enqueueTerminalLocked(r); startTerminalLocked(); return; }
        // The synchronous fast terminal is also the one original node's
        // consumer. Another requester can notify it, never free/recycle it
        // while that consumer is still inside the real provider service.
        if (r->terminalProcessing) { r->terminalAgain = true; return; }
        r->terminalProcessing = true; r->terminalAgain = false;
    }
    const bool done = retireNow(r);
    {
        QMutexLocker notification(&terminalGate);
        r->terminalProcessing = false;
        if (!done) {
            if (!r->terminalRegistered) { r->terminalRegistered = true;
            if (r->terminalSerial != std::numeric_limits<quint64>::max()) ++r->terminalSerial;
            ++terminalCount; }
            enqueueTerminalLocked(r); startTerminalLocked();
        }
    }
    if (done) {
        QMutexLocker lock(&gate);
        if (!r->physical && r->terminalSerial != std::numeric_limits<quint64>::max()) { r->nextFree = freeResources; freeResources = r; }
    }
}

bool KisNativeResultProtocol::Impl::retireNow(Resource *r) noexcept
{
    if (!r) return true;
    {
        QMutexLocker lock(&gate);
        if (!r->physical) return true;
        settleTransient(r);
        if (r->current || r->retained || r->pending) { r->terminal = true; return true; }
        if (r->busy) { r->terminal = true; return false; }
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
        QMutexLocker lock(&gate);
        if (r->charged != Class::RetirementDebt && r->debt.isValid()) {
            KisBackingBudgetDelta change;
            delta(change, r->charged, r->domain, -qint64(descriptor.minimumByteSize()));
            delta(change, Class::RetirementDebt, r->domain, qint64(descriptor.minimumByteSize()));
            r->debt.commit(change);
            r->charged = Class::RetirementDebt;
        }
        r->busy = false; r->terminal = true; return false;
    }
    completeTerminal(r);
    budget.releaseLive(r->charged, r->domain, descriptor.minimumByteSize());
    r->debt.release();
    r->binding.reset();
    Physical::node_type released;
    {
        QMutexLocker lock(&gate);
        released = physical.extract(r->physicalSlot);
        r->physical = false; r->busy = r->terminal = false; r->tile = nullptr;
        r->charged = Class::Count; r->handle = {}; r->physicalSlot = 0;
        COUNT(retired, 1);
    }
    return true;
}

// Only true deferred retirements acquire this continuation. The reusable
// executor node and fallback deadline were funded at owner initialization.
void KisNativeResultProtocol::Impl::enqueueTerminalLocked(Resource *r) noexcept
{
    if (!r->terminalRegistered) return;
    if (r->terminalProcessing) { r->terminalAgain = true; return; }
    if (r->terminalQueued) return;
    r->terminalQueued = true; r->terminalNext = nullptr;
    if (terminalTail) terminalTail->terminalNext = r;
    else terminalHead = r;
    terminalTail = r;
}

void KisNativeResultProtocol::Impl::startTerminalLocked() noexcept
{
    if (terminalRunning || terminalDelayed || !terminalHead) return;
    terminalRunning = true; terminalJobOwner = shared_from_this();
    if (!kisEnqueuePageStoreReclamation(terminalJob.get())) {
        // Permanent runtime stop rejects scheduling. Actual holders still
        // own Impl; their last release destroys the now-unpinned resources.
        // Never create a self-cycle that survives the original consumer.
        terminalRunning = false; terminalJobOwner.reset();
    }
}

void KisNativeResultProtocol::Impl::deferRetirement(Resource *r) noexcept
{
    QMutexLocker lock(&terminalGate);
    if (!r->terminalRegistered) {
        r->terminalRegistered = true;
            if (r->terminalSerial != std::numeric_limits<quint64>::max()) ++r->terminalSerial;
            ++terminalCount;
    }
    enqueueTerminalLocked(r); startTerminalLocked();
}

void KisNativeResultProtocol::Impl::queueTerminal(Resource *r, quint64 serial) noexcept
{
    QMutexLocker lock(&terminalGate);
    if (!r->terminalRegistered || r->terminalSerial != serial) return;
    // A real readiness event supersedes a fallback deadline. The deadline's
    // later notification is harmless; it carries no allocation authority.
    terminalDelayed = false;
    enqueueTerminalLocked(r); startTerminalLocked();
}

void KisNativeResultProtocol::Impl::completeTerminal(Resource *r) noexcept
{
    r->terminalSubscription.reset();
    QMutexLocker lock(&terminalGate);
    if (!r->terminalRegistered) return;
    Q_ASSERT(r->terminalProcessing && !r->terminalQueued);
    // The worker still uses the original node after the physical free. It
    // unlinks the terminal lifetime before returning this slot to the cache.
}

void KisNativeResultProtocol::Impl::runTerminals() noexcept
{
    // Work is bounded by actual debt, not image/resource capacity. Each pass
    // attempts at most 32 records, then yields to the original executor.
    for (size_t i = 0; i < 32; ++i) {
        Resource *r;
        {
            QMutexLocker lock(&terminalGate);
            r = terminalHead; if (!r) break;
            terminalHead = r->terminalNext;
            if (!terminalHead) terminalTail = nullptr;
            r->terminalNext = nullptr; r->terminalQueued = false;
            r->terminalProcessing = true; r->terminalAgain = false;
        }
        r->terminalSubscription.reset();
        Resource *settlement = nullptr;
        { QMutexLocker lock(&gate); claimReleased(r, settlement); }
        settleReleased(settlement, false);
        const bool done = retireNow(r);
        KisPageReadinessStatus readiness = KisPageReadinessStatus::Ready;
        if (!done) {
            try {
                auto weak = weak_from_this();
                const auto serial = r->terminalSerial;
                readiness = r->binding->watchRetirementReadiness(r->handle.allocationIdentity(),
                    KisPageReadinessCallback([weak, r, serial] {
                        if (auto owner = weak.lock()) owner->queueTerminal(r, serial);
                    }, &budget), &r->terminalSubscription);
            } catch (const std::bad_alloc &) { readiness = KisPageReadinessStatus::Unavailable; }
        }
        bool delayed = false;
        {
            QMutexLocker lock(&terminalGate);
            r->terminalProcessing = false;
            if (done) {
                r->terminalRegistered = false; r->terminalAgain = false;
                Q_ASSERT(terminalCount); --terminalCount;
            }
            if (r->terminalRegistered && (r->terminalAgain || readiness != KisPageReadinessStatus::Waiting)) {
                enqueueTerminalLocked(r);
                // Physical foreign users and refused watcher capacity need
                // a bounded cold retry, never an immediate busy-spin.
                if (!done && readiness != KisPageReadinessStatus::Waiting) terminalDelayed = true;
            }
            delayed = terminalDelayed;
        }
        if (done && !r->physical && r->terminalSerial != std::numeric_limits<quint64>::max()) {
            QMutexLocker lock(&gate); r->nextFree = freeResources; freeResources = r;
        }
        if (delayed) break;
    }
}

void KisNativeResultProtocol::Impl::finishTerminalJob() noexcept
{
    std::shared_ptr<Impl> finished;
    {
        QMutexLocker lock(&terminalGate);
        finished = std::move(terminalJobOwner); terminalRunning = false;
        if (terminalDelayed && terminalHead) terminalDelay.arm(10);
        else startTerminalLocked();
    }
    // The last owner, job and notification storage are destroyed outside all
    // gates and only after the executor has stopped touching this job node.
}

void KisNativeResultProtocol::Impl::claimReleased(Resource *r, Resource *&head) noexcept
{
    if (!r || !r->physical) return;
    settleTransient(r);
    if (r->busy) { if (!r->settling) r->terminal = true; return; }
    const auto wanted = r->desired();
    if (wanted == r->charged && !(wanted == Class::IdleCapacity && (closing || r->terminal))) {
        if (wanted == Class::IdleCapacity && !r->idle) park(r);
        return;
    }
    r->busy = r->settling = true; r->settlementNext = head; head = r;
}

void KisNativeResultProtocol::Impl::settleReleased(Resource *head, bool retirePhysical) noexcept
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
        if (terminal && retirePhysical) retire(r);
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

KisNativeResultProtocol::Body *KisNativeResultProtocol::Impl::iteratorBody() const
{
    for (auto *b = iteratorHead; b; b = b->iteratorNext)
        if (b->iteratorOwner == this && !b->revoked) return b;
    return nullptr;
}

void KisNativeResultProtocol::Impl::detachIterator(Body *b)
{
    auto **link = &iteratorHead;
    while (*link && *link != b) link = &(*link)->iteratorNext;
    Q_ASSERT(*link == b);
    if (*link) *link = b->iteratorNext;
    b->iteratorNext = nullptr;
}

bool KisNativeResultProtocol::Impl::finishRawResult(Body *body) noexcept
{
    auto owner = std::move(body->deferredOwner);
    if (!body->rawDirty || body->failed) {
        const bool ok = !body->failed || body->revoked; owner->cancelBody(body); return ok;
    }
    owner->finishProducer(*body);
    QString failure;
    if (owner->acceptBody(*body, nullptr, &failure)) { owner->returnBody(body); return true; }
    KisStrokeJobFailureContext::reportFailure(failure.isEmpty()
        ? QStringLiteral("Native raw result acceptance failed") : failure);
    owner->cancelBody(body); return false;
}

void KisNativeResultProtocol::Impl::finishRaw(void *opaque) noexcept
{
    auto *body = static_cast<Body *>(opaque);
    Q_ASSERT(body->borrows);
    if (--body->borrows == 0 && !body->clients) body->deferredOwner->finishRawResult(body);
}

bool KisNativeResultProtocol::Impl::finishIterator(Body *b, bool success)
{
    b->failed |= !success;
    Q_ASSERT(b->clients);
    if (--b->clients) return !b->failed || b->revoked;
    detachIterator(b);
    return b->borrows ? (!b->failed || b->revoked) : finishRawResult(b);
}

bool KisNativeResultProtocol::Impl::bindRows(Body &b, const QRect &rect, QString *why, bool append)
{
    const QRect tiles(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                      QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    const quint64 total = quint64(tiles.width()) * quint64(tiles.height());
    if (!total || total > std::numeric_limits<quint32>::max()
        || !KisTileHashTable::supportsCoordinates(tiles.left(), tiles.top())
        || !KisTileHashTable::supportsCoordinates(tiles.right(), tiles.bottom())) return false;
    const size_t first = append ? b.rows.size() : 0;
    b.rows.resize(first + size_t(total)); b.moves.reserve(b.rows.size() * 2);
    Pages staged(std::less<Key>{}, pages.get_allocator());
    {
        QMutexLocker lock(&gate);
        size_t at = first;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto it = pages.find({y, x});
            if (it != pages.end()) b.rows[at].page = &it->second;
        }
    }
    size_t at = first;
    for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at)
        if (!b.rows[at].page) {
            auto it = staged.try_emplace({y, x}).first;
            it->second.key = {{1}, {x, y}};
        }
    {
        QMutexLocker lock(&gate);
        if (closing) return false;
        pages.merge(staged);
        at = first;
        for (int y = tiles.top(); y <= tiles.bottom(); ++y) for (int x = tiles.left(); x <= tiles.right(); ++x, ++at) {
            auto &row = b.rows[at];
            if (!row.page) row.page = &pages.find({y, x})->second;
            if (row.page->writer) { error(why, "same-page writer conflict"); return false; }
            row.page->writer = &b; row.page->writerIndex = at; row.before = row.page->current;
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
    if (!range.isEmpty() && !KisNativeResultAdapter::extent(adapter).prepareTileRange(range)) {
        error(why, "adapter extent capacity refused; Ready retained"); return false;
    }
    auto &table = KisNativeResultAdapter::table(adapter);
    for (auto &row : b.rows) if (row.changed) {
        const auto update = row.after.resource ? KisTileHashTable::Update::Present : KisTileHashTable::Update::Absent;
        if (!row.adapter && !(row.location && row.after.resource))
            row.adapter = table.prepareTileUpdate(row.page->key.page.column, row.page->key.page.row, update, &row.location);
    }
    for (auto &row : b.rows) if (row.changed && !row.after.resource)
        row.location = table.borrowTileForPreparedUpdate(row.page->key.page.column, row.page->key.page.row);
    if (b.defaultTile) {
        b.defaultReaders.clear();
        KisTileHashTableConstIterator it(&table);
        while (!it.isDone()) {
            auto tile = it.tile(); bool affected;
            { QMutexLocker lock(&gate); auto p = pages.find({tile->row(), tile->col()}); affected = p == pages.end() || !p->second.current.resource; }
            if (affected) b.defaultReaders.push_back(std::move(tile));
            it.next();
        }
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
    const QRect &rect, qsizetype stride, const Faults *faults, QString *why, size_t first, size_t last)
{
    const int bpp = descriptor.format.pixelStride;
    const bool producer = !data;
    if (rect.isEmpty() || (!producer && !bounds(data, rect, stride, bpp))) { error(why, "invalid packed input"); return false; }
    if (b.rows.empty() && !bindRows(b, rect, why)) return false;
    last = std::min(last, b.rows.size());
    const auto payloadFor = [&](const Body::Row &row) -> const quint8 * {
        return producer ? nullptr : data + (qint64(row.page->key.page.row) * 64 + row.local.top() - rect.top()) * stride
            + (qint64(row.page->key.page.column) * 64 + row.local.left() - rect.left()) * bpp;
    };
    // Original rows retain original positions. Positive destination admission
    // is one actual budget group, while physical exclusion remains per backing.
    KisBackingBudgetDelta reuseChange;
    size_t reuseCandidates = 0;
    const auto transientLimit = budget.maxTransientVersionsPerPage();
    for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) {
        auto &p = *row.page;
        if (row.selectionOnly) continue;
        if (row.remove) { row.changed = bool(row.before.resource); row.after = {}; continue; }
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
        if (p.transient >= transientLimit) {
            error(why, "page transient-result budget exhausted"); return false;
        }
        row.after.generation = p.nextGeneration++; row.changed = true;
        if (!forceFresh && p.idle && !p.idle->busy) {
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
        for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (auto *idle = row.idleCandidate) {
            if (!(faults && faults->rejectPhysical)) row.writable = KisNativeResultProtocol::claimNative(
                idle->binding, idle->handle.allocation.generation, {row.page->key, {row.after.generation}});
            if (!row.writable.isValid()) continue;
            delta(succeeded, idle->charged, idle->domain, -qint64(descriptor.minimumByteSize()));
            delta(succeeded, Class::ActivePending, idle->domain, qint64(descriptor.minimumByteSize()));
        }
        // All unclaimed positive shares are returned before Fresh can reserve.
        fee.commit(succeeded);
        for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (auto *idle = std::exchange(row.idleCandidate, nullptr)) {
            if (row.writable.isValid()) {
                QMutexLocker lock(&gate);
                idle->handle.version = {row.page->key, {row.after.generation}}; ++idle->handle.allocation.generation;
                idle->charged = Class::ActivePending; idle->pending = 1; idle->transient = true; ++row.page->transient; idle->busy = false;
                row.target = idle; row.ownsTarget = true; COUNT(handoff, 1);
            } else {
                { QMutexLocker lock(&gate); idle->busy = false; }
                retire(idle);
            }
        }
    }
    size_t made = 0;
    for (auto &row : boost::make_iterator_range(b.rows.begin() + first, b.rows.begin() + last)) if (row.changed && !row.remove && !row.selectionOnly) {
        const auto local = row.local;
        const auto *payload = payloadFor(row);
        auto *source = row.before.resource;
        const bool reused = row.target != nullptr;
        if (!row.target) {
            row.target = fresh(*row.page, {row.page->key, {row.after.generation}}, payload, stride,
                               row.discard ? nullptr : source, local, why);
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
            if (reused && !row.discard && (producer || local != QRect(0, 0, 64, 64))) {
                if (source) {
                    const void *before = source->binding->acquireRead(source->handle.allocationIdentity(), false);
                    if (!before) return false;
                    std::memcpy(destination, before, size_t(descriptor.minimumByteSize()));
                    source->binding->releaseRead();
                } else for (int i = 0; i < 4096; ++i)
                    std::memcpy(destination + i * bpp, descriptor.format.defaultPixel.constData(), size_t(bpp));
            }
            if (producer) {
                if (!row.discard && !source && !reused) for (int i = 0; i < 4096; ++i)
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
    if (b.history && (b.history->phase == KisNativeHistory::Phase::ClosingAbort
        || b.history->phase == KisNativeHistory::Phase::Failed)) return false;
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
    { QMutexLocker lock(&gate);
      if (b.asyncPending || !b.ready || b.accepted || b.failed
          || (b.history && (b.history->phase == KisNativeHistory::Phase::ClosingAbort
              || b.history->phase == KisNativeHistory::Phase::Failed))) return false; }
    const size_t changed = size_t(std::count_if(b.rows.begin(), b.rows.end(), [](const auto &r) { return r.changed; }));
    if (!changed && !b.defaultTile) {
        QMutexLocker lock(&gate);
        for (auto &r : b.rows) if (r.page->writer == &b) r.page->writer = nullptr;
        b.accepted = true; return true;
    }
    KisPageStoreDiagnosticTimer publishing(nullptr, KisPageStoreDiagnosticPhase::WritePublishHost, changed);
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
#ifdef NR104_INSTALL_AUDIT
    NativeInstallAudit installAudit;
#endif
    QWriteLocker adapterGate(&KisNativeResultAdapter::gate(adapter));
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
#ifdef NR104_INSTALL_AUDIT
        installAudit.begin();
#endif
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
            settleTransient(m.resource);
            if (m.after == Class::IdleCapacity && !m.resource->idle) park(m.resource);
        }
        if (currentHistory) {
            QRect changedBounds;
            for (const auto &row : b.rows) if (row.changed)
                changedBounds |= QRect(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
            currentHistory->setExtent(currentHistory->extent() | changedBounds);
        }
        epoch = b.cutEpoch; b.accepted = true;
        if (b.defaultTile) {
            std::swap(descriptor.format.defaultPixel, b.defaultPixel);
            std::swap(defaultBacking, b.defaultTile); ++defaultRevision;
        }
        COUNT(results, changed);
    }
    // Real adapter work stays inside the manager cut but outside S/B: legacy
    // Tile read takes its swap barrier before asking this bridge for selection.
    auto &table = KisNativeResultAdapter::table(adapter);
    auto &extent = KisNativeResultAdapter::extent(adapter);
    if (b.defaultTile) {
        KisNativeResultAdapter::installDefault(adapter, reinterpret_cast<const quint8 *>(descriptor.format.defaultPixel.constData()), defaultBacking);
        for (auto &tile : b.defaultReaders) tile->invalidatePageStoreReadCache();
    }
    for (auto &row : b.rows) if (row.changed) {
        if (!row.after.resource) {
            // The exact adapter capability dies after the no-fail cut, in its
            // original row. Detach atomically with cache extraction so a raw
            // reader cannot acquire a new bridge lease in between.
            if (auto *tile = row.location) {
                QMutexLocker tileLock(&tile->m_swapBarrierLock);
                tile->m_pageStoreBridge.storeRelease(nullptr);
                if (!tile->m_lockCounter) row.detachedRead = std::move(tile->m_pageStoreLeases[KisTile::ReadLease]);
            }
            if (table.removePreparedTile(row.adapter)) extent.notifyTileRemoved(row.page->key.page.column, row.page->key.page.row);
            continue;
        }
        KisTileSP installed;
        bool created = false;
        auto *tile = row.location;
        if (!tile) { installed = table.installPreparedTile(row.adapter, created); tile = installed.data(); }
        Q_ASSERT(tile);
        {
            QMutexLocker tileLock(&tile->m_swapBarrierLock);
            if (tile->m_pageStoreBridge.loadRelaxed() != this || tile->m_pageStoreOldDataView.loadRelaxed()) {
                Q_ASSERT(!tile->m_lockCounter);
                row.detachedRead = std::move(tile->m_pageStoreLeases[KisTile::ReadLease]);
                tile->m_pageStoreOldDataView.storeRelaxed(0);
                tile->m_pageStoreBridge.storeRelease(this);
            }
            tile->m_pageStoreNativeReadReady.storeRelease(0);
        }
        if (created) extent.notifyTileAdded(row.page->key.page.column, row.page->key.page.row);
#ifdef NR104_CENSUS
        { QMutexLocker countLock(&gate); COUNT(adapterInstalled, 1); }
#endif
    }
    adapterGate.unlock();
#ifdef NR104_INSTALL_AUDIT
    installAudit.end();
    std::fprintf(stderr, "NATIVE_INSTALL_AUDIT_OK changed=%zu default=%d restore=%d\n", changed, int(b.defaultTile != nullptr), int(b.restore));
#endif
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
    const QRect &local, qsizetype stride, const KisPageByteArray &defaultPixel, QString *why) const
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
        Body *body = iteratorBody();
        if (body && body->failed) return {};
        const bool shared = body != nullptr;
        auto cancel = qScopeGuard([&] {
            if (!body) return;
            if (shared) body->failed = true;
            else { body->deferredOwner.reset(); cancelBody(body); }
        });
        try {
            std::unique_ptr<WriteLease> result;
            if (cache && *cache) {
                auto *dormant = dynamic_cast<WriteLease *>(cache->get());
                if (dormant && dormant->owner.get() == this && !dormant->body && !dormant->fixedPinned)
                    result.reset(static_cast<WriteLease *>(cache->release()));
            }
            if (!result) result = std::make_unique<WriteLease>();
            result->writes = true;
            if (!body) { body = checkout(); if (!body) return {}; body->deferredOwner = shared_from_this();
                body->raw = true; body->thread = QThread::currentThreadId(); }
            size_t index = body->rows.size();
            {
                QMutexLocker lock(&gate);
                auto it = pages.find({row, col});
                if (it != pages.end() && it->second.writer == body) index = it->second.writerIndex;
            }
            if (index == body->rows.size()) {
                const QRect rect(col * 64, row * 64, 64, 64);
                if (!bindRows(*body, rect, nullptr, true) ||
                    !prepareTargets(*body, nullptr, rect, 0, nullptr, nullptr, index)) return {};
            }
            result->owner = shared_from_this(); result->body = body; result->tile = body->rows[index].target->tile;
            ++body->borrows; body = nullptr;
            return TileLease(result.release());
        } catch (const std::bad_alloc &) { return {}; }
    }
    if (auto *b = iteratorBody()) {
        Resource *target = nullptr;
        { QMutexLocker lock(&gate); auto it = pages.find({row, col});
          if (it != pages.end() && it->second.writer == b) target = b->rows[it->second.writerIndex].target; }
        if (target) {
            auto lease = std::make_unique<WriteLease>();
            lease->owner = shared_from_this(); lease->body = b; lease->tile = target->tile; lease->writes = false;
            ++b->borrows; return TileLease(lease.release());
        }
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
        { QMutexLocker lock(&gate);
          result->tile = defaultBacking; result->tile->ref(); result->revision = defaultRevision; }
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
    {
    QString why;
    KisPageStoreProductPolicy policy;
    if (!kisDerivePageStoreProductPolicy(&policy, &why)) throw std::runtime_error(why.toStdString());
    auto parent = kisAcquireProductNonPayloadBudget(policy, &why);
    if (!parent) throw std::bad_alloc();
    d = std::allocate_shared<Impl>(Alloc<Impl>{}, adapter, descriptor, limits, std::move(parent), policy.providerBytes);
    d->initialize();
}

KisNativeResultProtocol::KisNativeResultProtocol(KisTiledDataManager &adapter,
    const KisPageAllocationDescriptor &descriptor)
    : KisNativeResultProtocol(adapter, descriptor, [] {
        KisPageStoreProductPolicy policy; QString why;
        if (!kisDerivePageStoreProductPolicy(&policy, &why)) throw std::runtime_error(why.toStdString());
        return policy.limits;
    }()) {}

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
    if (d->viewSerial == std::numeric_limits<quint64>::max()) return {};
    s.d->epoch = d->epoch; s.d->serial = ++d->viewSerial; s.d->defaultRevision = d->defaultRevision;
    s.d->defaultPixel = d->descriptor.format.defaultPixel;
    s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref();
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
    auto &table = KisNativeResultAdapter::table(d->adapter);
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
    s.d->defaultPixel = d->descriptor.format.defaultPixel; s.d->defaultRevision = d->defaultRevision;
    s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref();
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
    { QMutexLocker lock(&d->gate);
      s.d->defaultPixel = d->descriptor.format.defaultPixel; s.d->defaultRevision = d->defaultRevision;
      s.d->defaultBacking = d->defaultBacking; s.d->defaultBacking->ref(); }
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
        } else row.adapter = KisNativeResultAdapter::table(d->adapter).prepareTileUpdate(
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
        row.adapter = KisNativeResultAdapter::table(d->adapter).prepareTileUpdate(
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
    QReadLocker adapterGate(&KisNativeResultAdapter::gate(d->adapter));
    const auto bpp = d->descriptor.format.pixelStride;
    auto *live = d->iteratorBody();
    for (int y = floorTile(pixels.top()); y <= floorTile(pixels.bottom()); ++y)
        for (int x = floorTile(pixels.left()); x <= floorTile(pixels.right()); ++x) {
            Impl::Selection selected; const quint8 *pending = nullptr;
            { QMutexLocker lock(&d->gate); if (d->closing) return false;
              auto it = d->pages.find({y, x}); if (it != d->pages.end()) {
                  selected = it->second.current;
                  if (live && it->second.writer == live) pending = live->rows[it->second.writerIndex].pixels;
              } }

            const QRect hit = pixels.intersected(QRect(x * 64, y * 64, 64, 64));
            if (pending) {
                for (int row = 0; row < hit.height(); ++row)
                    std::memcpy(bytes + qint64(hit.y() - pixels.y() + row) * stride + (hit.x() - pixels.x()) * bpp,
                        pending + ((hit.y() - y * 64 + row) * 64 + hit.x() - x * 64) * bpp, hit.width() * bpp);
                continue;
            }
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

bool KisNativeResultProtocol::close(QString *why)
{
    if (!d) return true;
    KisMementoSP unfinished;
    { QMutexLocker lock(&d->gate); unfinished = d->currentHistory; d->currentHistory = {}; }
    unfinished = {};
    Impl::Resource *releasedIdle = nullptr;
    {
        QMutexLocker lock(&d->gate);
        if (!d->closing) {
            d->closing = true;
            // Detach the original idle chains under their owning gate. Active
            // births may still be growing the Resource arena; never scan it
            // unlocked or discover pending work through physical capacity.
            for (auto &p : d->pages) while (auto *r = p.second.idle) {
                p.second.idle = r->nextIdle; r->nextIdle = nullptr; r->idle = false;
                d->claimReleased(r, releasedIdle);
            }
        }
    }
    // Manager wrappers release dormant native leases before final real free.
    { QWriteLocker adapterGate(&KisNativeResultAdapter::gate(d->adapter));
      KisNativeResultAdapter::bridge(d->adapter, nullptr);
      KisNativeResultAdapter::table(d->adapter).clear(); KisNativeResultAdapter::extent(d->adapter).clear(); }
    d->settleReleased(releasedIdle);
    d->finishClosing();
    QMutexLocker lock(&d->gate);
    if (d->active || d->domainMoves) { error(why, "closing; original work still owns its funded tail"); return false; }
    return true;
}

KisNativeResultProtocol::Counts KisNativeResultProtocol::counts() const { QMutexLocker lock(&d->gate); return d->counts; }
KisPageBackingUsage KisNativeResultProtocol::usage() const { return d->budget.usage(); }

std::unique_ptr<KisNativeResultProtocol> KisNativeResultProtocol::createProduct(
    KisTiledDataManager &adapter, quint32 bpp, const quint8 *pixel)
{
    KisSurfaceEpochState state;
    state.surface = {1}; state.logicalPageExtent = QSize(64, 64);
    state.layoutRevision = state.rowAlignment = state.defaultPixelRevision = state.extentRevision = 1;
    auto &f = state.format;
    f.formatId = 104; f.colorModelId = "RAW"; f.colorDepthId = "U8";
    f.profileFingerprint = "native-result"; f.channelOrder = "raw"; f.packing = "interleaved";
    f.defaultPixel = QByteArray(reinterpret_cast<const char *>(pixel), bpp);
    f.channelCount = bpp; f.pixelStride = bpp; f.pixelAlignment = 1;
    f.hasAlpha = false; f.alphaSemantic = KisSurfaceAlphaSemantic::Opaque;
    f.endianness = KisSurfaceEndianness::NativeEndian; f.codecVersion = 1;
    return std::make_unique<KisNativeResultProtocol>(adapter, state.allocationDescriptor());
}

KisMementoSP KisNativeResultProtocol::beginHistory(QString *why)
try {
    QMutexLocker historySequence(&d->historyGate);
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) return d->currentHistory;
        if (d->closing || d->active
            || d->historySequence == std::numeric_limits<quint64>::max()) return {};
    }
    auto history = std::allocate_shared<KisNativeHistory>(Alloc<KisNativeHistory>::retained(&d->budget));
    history->before = capture(why); if (!history->before) return {};
    KisMementoSP result = KisMemento::createPageStoreMemento();
    result->saveOldDefaultPixel(reinterpret_cast<const quint8 *>(history->before.d->defaultPixel.constData()),
                               d->descriptor.format.pixelStride);
    // The original history gate sequences public memento calls. Capacity
    // precedes S; weak entries never extend pixel/Selection lifetimes.
    d->history.reserve(d->history.size() + 1);
    {
        QMutexLocker lock(&d->gate);
        if (d->closing || d->active || d->currentHistory) return {};
        history->sequence = ++d->historySequence;
        d->history.push_back(history);
        result->m_nativeHistory = std::move(history); d->currentHistory = result;
    }
    return result;
} catch (const std::bad_alloc &) { error(why, "history admission refused"); return {}; }

bool KisNativeResultProtocol::commitHistory(QString *why)
try {
    QMutexLocker historySequence(&d->historyGate);
    KisMementoSP current;
    {
        QMutexLocker lock(&d->gate); current = d->currentHistory;
        if (!current) return !d->active;
        auto &phase = current->m_nativeHistory->phase;
        if (phase == KisNativeHistory::Phase::Failed || phase == KisNativeHistory::Phase::ClosingAbort) return false;
        phase = KisNativeHistory::Phase::ClosingCommit;
        if (d->active) return false;
    }
    auto after = capture(why); if (!after) return false;
    current->saveNewDefaultPixel(reinterpret_cast<const quint8 *>(after.d->defaultPixel.constData()),
                                d->descriptor.format.pixelStride);
    {
        QMutexLocker lock(&d->gate);
        if (d->active || current != d->currentHistory) return false;
        current->m_nativeHistory->after = std::move(after);
        current->m_nativeHistory->phase = KisNativeHistory::Phase::Sealed;
        d->currentHistory = {};
    }
    return true;
} catch (const std::bad_alloc &) { error(why, "history finalization refused"); return false; }

bool KisNativeResultProtocol::abortHistory(const KisMementoSP &current, QString *why)
{
    QMutexLocker historySequence(&d->historyGate);
    { QMutexLocker lock(&d->gate);
      if (!current || current != d->currentHistory || !current->m_nativeHistory) return false;
      current->m_nativeHistory->phase = KisNativeHistory::Phase::ClosingAbort;
      if (d->active) return false; }
    if (!current->m_nativeHistory || !restore(current->m_nativeHistory->before, why)) return false;
    { QMutexLocker lock(&d->gate); d->currentHistory = {}; }
    auto mutableCurrent = current; mutableCurrent->m_nativeHistory.reset();
    return true;
}

bool KisNativeResultProtocol::restoreHistory(const KisMementoSP &memento, bool before, QString *why)
{
    QMutexLocker historySequence(&d->historyGate);
    if (!memento || !memento->m_nativeHistory || !memento->m_nativeHistory->before.d
        || memento->m_nativeHistory->before.d->owner != d || !commitHistory(why)) return false;
    return restore(before ? memento->m_nativeHistory->before : memento->m_nativeHistory->after, why);
}

bool KisNativeResultProtocol::hasHistory() const
{ QMutexLocker lock(&d->gate); return bool(d->currentHistory); }

void KisNativeResultProtocol::purgeHistory(const KisMementoSP &oldest)
{
    QMutexLocker historySequence(&d->historyGate);
    if (!oldest || !oldest->m_nativeHistory || !oldest->m_nativeHistory->before.d
        || oldest->m_nativeHistory->before.d->owner != d) return;
    const auto through = oldest->m_nativeHistory->sequence;
    bool includesCurrent;
    { QMutexLocker lock(&d->gate); includesCurrent = d->currentHistory && d->currentHistory->m_nativeHistory->sequence <= through; }
    if (includesCurrent && !commitHistory(nullptr)) {
        KisStrokeJobFailureContext::reportFailure(QStringLiteral("native history purge cannot close active or failed work"));
        return;
    }
    // Release actual historical consumers, outside the selection gate. No
    // resource/page scan or replacement publication is needed for a purge.
    auto end = std::remove_if(d->history.begin(), d->history.end(), [&](auto &weak) {
        auto entry = weak.lock();
        if (!entry) return true;
        if (entry->sequence > through) return false;
        if (d->currentHistory && d->currentHistory->m_nativeHistory == entry) return false;
        entry->before = {}; entry->after = {}; return true;
    });
    d->history.erase(end, d->history.end());
}

KisTileSP KisNativeResultProtocol::oldTile(qint32 col, qint32 row, bool *present, QString *why) const
try {
    Snapshot view;
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) view = d->currentHistory->m_nativeHistory->before;
    }
    if (!view) return {};
    Impl::Selection selected;
    { QMutexLocker lock(&d->gate); selected = d->viewSelection(*view.d, {row, col}); }
    if (present) *present = selected.resource != nullptr;
    KisTileData *tile = nullptr;
    if (auto *r = selected.resource) {
        if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return {};
        auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
        // The actual public oldData tile owns a COW reference before the
        // physical pin ends. Future handoff observes that real foreign owner.
        return new KisTile(col, row, r->tile, nullptr);
    }
    // The captured immutable default already owns its real backing.
    // An oldData wrapper consumes that fact instead of recreating storage.
    return new KisTile(col, row, view.d->defaultBacking, nullptr);
} catch (const std::bad_alloc &) { error(why, "oldData public view refused"); return {}; }

bool KisNativeResultProtocol::clear(QString *why)
try {
    {
        QMutexLocker bodies(&d->bodyGate);
        QMutexLocker lock(&d->gate);
        for (auto &item : d->pages) if (auto *writer = item.second.writer)
            if ((!writer->iterator && !writer->raw) || writer->thread != QThread::currentThreadId()) {
                error(why, "clear conflicts with explicit or foreign result work"); return false;
            }
        for (auto &body : d->bodies) if (body.iterator && body.clients) {
            if (body.thread != QThread::currentThreadId()) { error(why, "clear conflicts with foreign iterator"); return false; }
        }
        for (auto &body : d->bodies) if (body.iterator && body.clients) body.failed = body.revoked = true;
        for (auto &item : d->pages) if (auto *writer = item.second.writer) {
            writer->failed = writer->revoked = true; item.second.writer = nullptr;
        }
    }
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &row = b.rows[n++]; row.page = &item.second;
            if (row.page->writer) return false;
            row.page->writer = &b; row.before = row.page->current;
            row.changed = row.before.resource != nullptr; row.after = {};
        }
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "clear preparation refused"); return false; }

KisNativeResultProtocol::Snapshot KisNativeResultProtocol::captureOldData(QString *why)
{
    { QMutexLocker lock(&d->gate); if (d->currentHistory) return d->currentHistory->m_nativeHistory->before; }
    return capture(why);
}

bool KisNativeResultProtocol::mutate(const QRect &rect, const PageProducer &produce,
    bool discardCovered, const QByteArray &sparseDefault, QString *why)
try {
    if (rect.isEmpty()) return true;
    if (d->iteratorBody()) {
        // A live public writable capability is the original producer. Fill,
        // clear and planar kernels consume its rows just like pixelOperation;
        // they never open a conflicting second native result.
        return pixelOperation({rect}, [&](KisPixelWriteCursor *cursor) {
            for (int y = floorTile(rect.top()); y <= floorTile(rect.bottom()); ++y)
                for (int x = floorTile(rect.left()); x <= floorTile(rect.right()); ++x) {
                    const QRect hit = rect.intersected(QRect(x * 64, y * 64, 64, 64));
                    cursor->moveTo(hit.left(), hit.top());
                    auto *pixel = cursor->rawData(); if (!pixel) return false;
                    const auto stride = cursor->rowStride(hit.left(), hit.top());
                    const QRect local = hit.translated(-x * 64, -y * 64);
                    auto *page = pixel - local.y() * stride - local.x() * d->descriptor.format.pixelStride;
                    if (!produce(x, y, page, stride, local)) return false;
                }
            return true;
        }, {}, why);
    }
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    if (!d->bindRows(b, rect, why)) return false;
    const bool removeCovered = !sparseDefault.isEmpty() && sparseDefault == d->descriptor.format.defaultPixel;
    for (auto &row : b.rows) {
        const bool full = row.local == QRect(0, 0, 64, 64);
        row.remove = full && removeCovered; row.discard = full && discardCovered;
    }
    if (!d->prepareTargets(b, nullptr, rect, 0, nullptr, why)) return false;
    for (auto &row : b.rows) if (row.pixels && !produce(row.page->key.page.column, row.page->key.page.row,
        row.pixels, 64 * d->descriptor.format.pixelStride, row.local)) return false;
    d->finishProducer(b);
    return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "native pixel operation preparation refused"); return false; }

bool KisNativeResultProtocol::fill(const QRect &rect, const QByteArray &pixel, QString *why)
{
    if (pixel.size() != d->descriptor.format.pixelStride) return false;
    return mutate(rect, [&](qint32, qint32, quint8 *data, quint32 stride, const QRect &local) {
        for (int y = local.top(); y <= local.bottom(); ++y)
            for (int x = local.left(); x <= local.right(); ++x)
                std::memcpy(data + y * stride + x * pixel.size(), pixel.constData(), pixel.size());
        return true;
    }, true, pixel, why);
}

KisNativeResultProtocol::ReadAccess::~ReadAccess()
{
    if (binding && pixels) binding->releaseRead();
    if (constant) { if (pixels) constant->unblockSwapping(); constant->deref(); }
}
KisNativeResultProtocol::ReadAccess::ReadAccess(ReadAccess &&other) noexcept
{ *this = std::move(other); }
KisNativeResultProtocol::ReadAccess &KisNativeResultProtocol::ReadAccess::operator=(ReadAccess &&other) noexcept
{
    if (this == &other) return *this;
    if (binding && pixels) binding->releaseRead();
    if (constant) { if (pixels) constant->unblockSwapping(); constant->deref(); }
    owner = std::move(other.owner); binding = std::move(other.binding);
    constant = std::exchange(other.constant, nullptr); pixels = std::exchange(other.pixels, nullptr);
    stride = std::exchange(other.stride, 0); selected = other.selected;
    return *this;
}

KisPageVersion KisNativeResultProtocol::Snapshot::versionAt(qint32 col, qint32 row) const
{
    if (!d) return {};
    QMutexLocker lock(&d->owner->gate);
    const auto selection = d->owner->viewSelection(*d, {row, col});
    return {{{1}, {col, row}}, {selection.generation}, d->defaultRevision};
}

KisNativeResultProtocol::ReadAccess KisNativeResultProtocol::Snapshot::readPage(
    qint32 col, qint32 row, QString *why) const
{
    ReadAccess result;
    if (!d || !KisTileHashTable::supportsCoordinates(col, row)
        || (d->explicitItems && !d->range.contains(QRect(col * 64, row * 64, 64, 64)))) return result;
    Impl::Selection selection;
    { QMutexLocker lock(&d->owner->gate); selection = d->owner->viewSelection(*d, {row, col}); }
    result.owner = d->owner;
    result.stride = 64 * d->owner->descriptor.format.pixelStride;
    result.selected = {{{1}, {col, row}}, {selection.generation}, d->defaultRevision};
    if (auto *r = selection.resource) {
        result.binding = r->binding;
        result.pixels = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
    } else {
        result.constant = d->defaultBacking;
        if (result.constant) {
            result.constant->ref();
            if (result.constant->blockSwapping()) result.pixels = result.constant->data();
        }
    }
    if (!result.pixels) error(why, "native selected read pin refused");
    return result;
}

std::shared_ptr<const KisPageStoreIteratorReadScope> KisNativeResultProtocol::captureIteratorScope(
    bool writable, std::shared_ptr<const KisPageStoreIteratorReadScope> existing, QString *why)
try {
    if (existing) {
        if (existing->m_nativeIdentity == d.get() && existing->m_beforeOnly == writable) return existing;
        error(why, "iterator selection owner or mode mismatch"); return d->failedReadScope;
    }
    auto result = std::allocate_shared<KisPageStoreIteratorReadScope>(Alloc<KisPageStoreIteratorReadScope>::retained(&d->budget));
    result->m_nativeIdentity = d.get(); result->m_beforeOnly = writable;
    if (!writable && d->iteratorBody()) { result->m_nativeLiveWriter = true; return result; }
    {
        QMutexLocker lock(&d->gate);
        if (d->currentHistory) result->m_nativeBefore = d->currentHistory->m_nativeHistory->before;
    }
    if (!writable) {
        result->m_nativeCurrent = capture(why);
        if (!result->m_nativeCurrent) return d->failedReadScope;
        if (!result->m_nativeBefore) result->m_nativeBefore = result->m_nativeCurrent;
    }
    return result;
} catch (const std::bad_alloc &) { error(why, "iterator selection admission refused"); return d->failedReadScope; }

bool KisNativeResultProtocol::copyFrom(const Snapshot &source, const QRect &rect, QString *why)
try {
    if (!source.d || source.d->owner->descriptor.format.pixelStride != d->descriptor.format.pixelStride) return false;
    if (rect.isEmpty()) return true;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body; if (!d->bindRows(b, rect, why)) return false;
    const auto bpp = d->descriptor.format.pixelStride;
    using Imported = std::map<Impl::Resource *, Impl::Resource *, std::less<Impl::Resource *>,
        Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>>;
    Imported imported(std::less<Impl::Resource *>{}, Alloc<std::pair<Impl::Resource *const, Impl::Resource *>>(&d->budget));
    for (auto &row : b.rows) {
        if (row.local != QRect(0, 0, 64, 64)) continue;
        Impl::Selection selected;
        { QMutexLocker lock(&source.d->owner->gate);
          selected = source.d->owner->viewSelection(*source.d, {row.page->key.page.row, row.page->key.page.column}); }
        if (!selected.resource) { row.remove = source.d->defaultPixel == d->descriptor.format.defaultPixel; row.discard = !row.remove; continue; }
        if (source.d->owner == d && selected.resource == row.before.resource) { row.selectionOnly = true; continue; }
        { QMutexLocker lock(&d->gate);
          if (row.page->nextGeneration == std::numeric_limits<quint64>::max()) return false;
          row.after.generation = row.page->nextGeneration++; }
        auto *r = selected.resource;
        if (source.d->owner == d) row.after.resource = r;
        else {
            auto node = imported.try_emplace(r, nullptr).first;
            if (!node->second) {
                if (!r->binding->acquireRead(r->handle.allocationIdentity(), false)) return false;
                auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
                auto sourceTile = d->provider->captureCompletedTileSource(source.d->owner->descriptor, r->tile, why);
                if (!sourceTile) return false;
                row.target = d->fresh(*row.page, {row.page->key, {row.after.generation}}, nullptr, 0, nullptr,
                    QRect(0, 0, 64, 64), why, sourceTile);
                if (!row.target) return false;
                row.ownsTarget = true; node->second = row.target;
            }
            row.after.resource = node->second;
        }
        row.changed = row.selectionOnly = true;
    }
    if (!d->prepareTargets(b, nullptr, rect, 0, nullptr, why)) return false;
    for (auto &row : b.rows) if (row.pixels) {
        const auto local = row.local;
        const QRect hit = local.translated(row.page->key.page.column * 64, row.page->key.page.row * 64);
        if (!source.read(row.pixels + local.y() * 64 * bpp + local.x() * bpp, hit, 64 * bpp, why)) return false;
    }
    d->finishProducer(b); return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "copy admission refused"); return false; }

bool KisNativeResultProtocol::trim(const QRect &kept, QString *why)
try {
    if (kept.isEmpty()) return clear(why);
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.resize(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        size_t n = 0;
        for (auto &item : d->pages) {
            auto &row = b.rows[n++]; row.page = &item.second;
            if (row.page->writer) return false;
            row.page->writer = &b; row.before = row.page->current;
            row.local = QRect(0, 0, 64, 64);
            const QRect tile(row.page->key.page.column * 64, row.page->key.page.row * 64, 64, 64);
            row.selectionOnly = !row.before.resource || kept.contains(tile);
            row.remove = !row.selectionOnly && !kept.intersects(tile);
        }
    }
    // Original rows enumerate actual page slots; never expand a sparse image
    // into its bounding rectangle just to perform a trim.
    if (!d->prepareTargets(b, nullptr, kept, 0, nullptr, why)) return false;
    const auto bpp = d->descriptor.format.pixelStride;
    for (auto &row : b.rows) if (row.pixels) {
        const int ox = row.page->key.page.column * 64, oy = row.page->key.page.row * 64;
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            if (!kept.contains(ox + x, oy + y)) std::memcpy(row.pixels + (y * 64 + x) * bpp,
                d->descriptor.format.defaultPixel.constData(), bpp);
    }
    d->finishProducer(b); return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "trim admission refused"); return false; }

void *KisNativeResultProtocol::IteratorSession::operator new(size_t n) { return kisAllocatePageProcessStorage(n, alignof(IteratorSession)); }
void KisNativeResultProtocol::IteratorSession::operator delete(void *p) noexcept { kisFreePageProcessStorage(p, sizeof(IteratorSession), alignof(IteratorSession)); }
KisNativeResultProtocol::IteratorSession::~IteratorSession() { if (body) owner->finishIterator(std::exchange(body, nullptr), false); }
bool KisNativeResultProtocol::IteratorSession::finish() { return !body || owner->finishIterator(std::exchange(body, nullptr), true); }
bool KisNativeResultProtocol::IteratorSession::isActive() const { return body && !body->failed && !owner->closing; }

std::unique_ptr<KisTiledDataManagerIteratorWriteScope> KisNativeResultProtocol::beginIterator(QString *why)
try {
    auto session = std::make_unique<IteratorSession>(); session->owner = d;
    auto *b = d->iteratorBody();
    if (b && b->failed) { error(why, "original iterator has failed"); return {}; }
    if (!b) {
        b = d->checkout(); if (!b) return {};
        b->iterator = true; b->thread = QThread::currentThreadId(); b->iteratorOwner = d.get(); b->deferredOwner = d;
        b->iteratorNext = Impl::iteratorHead; Impl::iteratorHead = b;
    }
    ++b->clients; session->body = b;
    return KisTiledDataManagerIteratorWriteScope::fromNative(std::move(session));
} catch (const std::bad_alloc &) { error(why, "iterator storage refused"); return {}; }

KisTileSP KisNativeResultProtocol::writableTile(qint32 col, qint32 row, QString *why)
try {
    if (!KisTileHashTable::supportsCoordinates(col, row)) return {};
    auto &table = KisNativeResultAdapter::table(d->adapter);
    const auto intentFor = [&](KisTileData *initial) {
        KisTileSP intent(new KisTile(col, row, initial, nullptr));
        auto lease = std::make_unique<Impl::WriteLease>(); lease->owner = d; lease->tile = initial;
        intent->setPageStoreBridge(d.get(), false);
        intent->m_pageStoreLeases[KisTile::ReadLease] = std::move(lease);
        intent->m_pageStoreNativeReadReady.storeRelease(1);
        return intent;
    };
    if (auto tile = table.getExistingTileForPreparedUpdate(col, row)) {
        QMutexLocker gate(&tile->m_swapBarrierLock);
        if (!tile->m_lockCounter || tile->m_pageStoreWriteLockCount) return tile;
        // A new writer acquisition does not join an unrelated old reader's
        // nested tile lock. Direct read/write nesting on the SAME Tile keeps
        // its existing public last-unlock semantics.
        return intentFor(tile->m_tileData);
    }
    if (d->iteratorBody()) {
        // A missing public wrapper is an intent, not a visible result. Its
        // original Body creates the target at lockForWrite; final acceptance
        // installs the canonical table member. No empty result is published.
        auto *defaultTile = table.refAndFetchDefaultTileData();
        auto release = qScopeGuard([&] { defaultTile->deref(); });
        return intentFor(defaultTile);
    }
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return {};
    if (!d->bindRows(*p.body, QRect(col * 64, row * 64, 64, 64), why)) return {};
    auto &entry = p.body->rows.front();
    if (!entry.before.resource) {
        { QMutexLocker lock(&d->gate);
          if (entry.page->nextGeneration == std::numeric_limits<quint64>::max()) return {};
          entry.after.generation = entry.page->nextGeneration++; }
        // Writable presence consumes the already-owned default backing. It
        // needs no pixel loop, producer pin or second default materialization.
        auto source = d->provider->captureCompletedTileSource(d->descriptor, d->defaultBacking, why);
        if (!source) return {};
        entry.target = d->fresh(*entry.page, {entry.page->key, {entry.after.generation}}, nullptr, 0, nullptr,
                             QRect(0, 0, 64, 64), why, source);
        if (!entry.target) return {};
        entry.ownsTarget = entry.changed = true; entry.after.resource = entry.target;
    }
    p.body->ready = true;
    if (!accept(p, why)) return {};
    return table.getExistingTileForPreparedUpdate(col, row);
} catch (const std::bad_alloc &) { error(why, "writable tile intent refused"); return {}; }

bool KisNativeResultProtocol::purge(const QRect &area, QString *why)
try {
    if (area.isEmpty()) return true;
    Prepared p; p.owner = d; p.body = d->checkout(); if (!p.body) return false;
    auto &b = *p.body;
    size_t size;
    { QMutexLocker lock(&d->gate); size = d->pages.size(); }
    b.rows.reserve(size); b.moves.reserve(2 * size);
    {
        QMutexLocker lock(&d->gate); if (size != d->pages.size()) return false;
        for (auto &item : d->pages) {
            auto &page = item.second;
            if (!page.current.resource || !area.intersects(QRect(item.first.second * 64, item.first.first * 64, 64, 64))) continue;
            if (page.writer) { error(why, "purge conflicts with writer"); return false; }
            b.rows.emplace_back(); auto &row = b.rows.back();
            row.page = &page; row.before = page.current; page.writer = &b;
        }
    }
    const int bpp = d->descriptor.format.pixelStride;
    for (auto &row : b.rows) {
        auto *r = row.before.resource;
        auto *pixels = static_cast<const quint8 *>(r->binding->acquireRead(r->handle.allocationIdentity(), false));
        if (!pixels) return false;
        auto unpin = qScopeGuard([&] { r->binding->releaseRead(); });
        bool same = true;
        for (int i = 0; i < 4096 && same; ++i)
            same = std::memcmp(pixels + i * bpp, d->descriptor.format.defaultPixel.constData(), bpp) == 0;
        row.changed = same; row.after = same ? Impl::Selection{} : row.before;
    }
    b.ready = true; return accept(p, why);
} catch (const std::bad_alloc &) { error(why, "purge preparation refused"); return false; }

bool KisNativeResultProtocol::beginHistoryMutation(const KisMementoSP &memento, QString *why)
{
    QMutexLocker lock(&d->gate);
    if (!memento || memento != d->currentHistory || d->active || d->closing) {
        error(why, "native history mutation owner unavailable"); return false;
    }
    auto &phase = memento->m_nativeHistory->phase;
    if (phase != KisNativeHistory::Phase::None && phase != KisNativeHistory::Phase::Open) return false;
    phase = KisNativeHistory::Phase::Open; return true;
}

bool KisNativeResultProtocol::checkpointHistoryMutation(const KisMementoSP &memento, QString *why)
{
    QMutexLocker lock(&d->gate);
    if (!memento || memento != d->currentHistory || memento->m_nativeHistory->phase != KisNativeHistory::Phase::Open || d->active || d->closing) {
        error(why, "native history checkpoint owner unavailable"); return false;
    }
    // Each successful original result already crossed the single all-D cut.
    // Checkpoint consumes that fact; it neither republishes nor reprojects it.
    return true;
}

bool KisNativeResultProtocol::pixelOperation(const QVector<QRect> &rects,
    const std::function<bool(KisPixelWriteCursor *)> &operation, const KisMementoSP &history, QString *why)
try {
    if (!operation) return false;
    {
        QMutexLocker lock(&d->gate);
        if ((history && (history != d->currentHistory || !history->m_nativeHistory
                || history->m_nativeHistory->phase != KisNativeHistory::Phase::Open))
            || (!history && d->currentHistory && d->currentHistory->m_nativeHistory->phase != KisNativeHistory::Phase::None)) {
            error(why, "pixel operation has no open native history owner"); return false;
        }
    }
    QRegion pixels, pages;
    for (const auto &rect : rects) if (!rect.isEmpty()) {
        pixels += rect;
        pages += QRect(QPoint(floorTile(rect.left()), floorTile(rect.top())),
                       QPoint(floorTile(rect.right()), floorTile(rect.bottom())));
    }
    if (pixels.isEmpty()) return true;
    quint64 pageCount = 0;
    for (const auto &part : pages) pageCount += quint64(part.width()) * quint64(part.height());
    KisPageStoreDiagnosticTimer phase(nullptr, KisPageStoreDiagnosticPhase::PixelOperationPreflight, pageCount);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationRangePrepare, pageCount);
    Prepared p;
    auto *body = d->iteratorBody();
    if (body && body->failed) { error(why, "original producer has failed"); return false; }
    const bool joined = body != nullptr;
    if (joined) ++body->clients;
    else { p.owner = d; p.body = d->checkout(); body = p.body; if (!body) return false; }
    bool succeeded = false, entered = false;
    auto releaseJoined = qScopeGuard([&] {
        if (joined && body) d->finishIterator(body, succeeded);
    });
    {
        QMutexLocker lock(&d->gate);
        if (history) {
            if (history != d->currentHistory || !history->m_nativeHistory
                || history->m_nativeHistory->phase != KisNativeHistory::Phase::Open) return false;
            body->history = history->m_nativeHistory;
        }
    }
    auto poison = qScopeGuard([&] {
        if (succeeded || !entered || !body || !body->history) return;
        QMutexLocker lock(&d->gate);
        if (body->history->phase != KisNativeHistory::Phase::ClosingAbort)
            body->history->phase = KisNativeHistory::Phase::Failed;
    });
    QRegion missing = pages;
    if (joined) {
        // Reuse the original producer's claims and physical pointers. Only
        // missing page runs append rows; no second operation or translation.
        for (const auto &part : pages) for (int y = part.top(); y <= part.bottom(); ++y)
            for (int x = part.left(); x <= part.right(); ++x) {
                bool owned;
                { QMutexLocker lock(&d->gate); auto it = d->pages.find({y, x}); owned = it != d->pages.end() && it->second.writer == body; }
                if (owned) missing -= QRect(x, y, 1, 1);
            }
    }
    for (const auto &part : missing) {
        if (!KisTileHashTable::supportsCoordinates(part.left(), part.top()) ||
            !KisTileHashTable::supportsCoordinates(part.right(), part.bottom())) return false;
        if (!d->bindRows(*body, QRect(part.x() * 64, part.y() * 64, part.width() * 64, part.height() * 64), why, true)) return false;
    }
    class Cursor final : public KisPixelWriteCursor {
    public:
        Impl &owner; Body &body; const QRegion &declared; QString *why;
        Body::Row *row = nullptr; qint32 px = 0, py = 0; bool failed = false;
        Cursor(Impl &o, Body &b, const QRegion &r, QString *w) : owner(o), body(b), declared(r), why(w) {}
        void moveTo(qint32 x, qint32 y) override {
            px = x; py = y; row = nullptr;
            if (!declared.contains(QPoint(x, y))) { failed = true; return; }
            QMutexLocker lock(&owner.gate);
            auto it = owner.pages.find({floorTile(y), floorTile(x)});
            if (it == owner.pages.end() || it->second.writer != &body) { failed = true; return; }
            row = &body.rows[it->second.writerIndex];
        }
        quint8 *rawData() override {
            if (!row || failed || body.failed) return nullptr;
            if (body.history) {
                const auto phase = body.history->phase.load(std::memory_order_acquire);
                if (phase == KisNativeHistory::Phase::ClosingAbort || phase == KisNativeHistory::Phase::Failed) { failed = true; return nullptr; }
            }
            if (!row->target) {
                const auto index = size_t(row - body.rows.data());
                const QRect rect(row->page->key.page.column * 64, row->page->key.page.row * 64, 64, 64);
                if (!owner.prepareTargets(body, nullptr, rect, 0, nullptr, why, index, index + 1)) { failed = true; return nullptr; }
            }
            return row->pixels + (py - row->page->key.page.row * 64) * 64 * owner.descriptor.format.pixelStride
                + (px - row->page->key.page.column * 64) * owner.descriptor.format.pixelStride;
        }
        qint32 numContiguousColumns(qint32 x) const override { return 64 - (x - floorTile(x) * 64); }
        qint32 numContiguousRows(qint32 y) const override { return 64 - (y - floorTile(y) * 64); }
        qint32 rowStride(qint32, qint32) const override { return 64 * owner.descriptor.format.pixelStride; }
        qint32 x() const override { return px; }
        qint32 y() const override { return py; }
    } cursor(*d, *body, pixels, why);
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationBody, 1); entered = true;
    if (!operation(&cursor) || cursor.failed) { error(why, "native pixel producer failed"); return false; }
    phase.next(KisPageStoreDiagnosticPhase::PixelOperationFinish, pageCount);
    if (joined) {
        body->rawDirty = true;
        succeeded = true;
        return d->finishIterator(std::exchange(body, nullptr), true);
    }
    d->finishProducer(*body); succeeded = accept(p, why);
    if (succeeded) body = nullptr;
    return succeeded;
} catch (const std::bad_alloc &) { error(why, "native pixel operation storage refused"); return false; }

KisNativeResultProtocol::Layout KisNativeResultProtocol::layout()
{
    // Per responsibility, not a public projection embedded into each row.
    // These are reviewed representation ceilings, never instruction budgets.
    static_assert(sizeof(Impl::Page) <= 128);
    static_assert(sizeof(Impl::Resource) <= 384);
    static_assert(sizeof(Body) <= 256);
    static_assert(sizeof(Body::Row) <= 192);
    static_assert(sizeof(Impl::Edge) <= 112);
    static_assert(sizeof(View) <= 144);
    return {sizeof(Impl::Page), sizeof(Impl::Resource), sizeof(Body), sizeof(Body::Row), sizeof(Impl::Edge), sizeof(View)};
}

KisNativeResultProtocol::State KisNativeResultProtocol::state() const
{
    State out;
    // Cold diagnostic only, never used to discover or drive operation work.
    QMutexLocker growth(&d->bodyGate);
    {
        QMutexLocker lock(&d->gate);
        out.pageSlots = d->pages.size(); out.physicalResources = d->physical.size();
        out.resourceCapacity = d->resources.size(); out.activeResults = d->active;
        out.logicalViews = d->views.size(); out.domainMoves = d->domainMoves; out.reservedEdges = d->edgeReserved;
        for (const auto &body : d->bodies) out.rowCapacity += body.rows.capacity();
    }
    { QMutexLocker lock(&d->terminalGate); out.terminalRegistrations = d->terminalCount; }
    return out;
}

KisTileSP KisNativeResultProtocol::selectReadTile(KisTileSP candidate, bool *present, QString *why)
try {
    if (!candidate || !present) return {};
    if (d->iteratorBody()) { candidate->setPageStoreBridge(d.get(), false); return candidate; }
    const bool indexed = *present;
    TileLease previous;
    QMutexLocker tileGate(&candidate->m_swapBarrierLock);
    if (candidate->m_pageStoreWriteLockCount) return candidate;
    auto &cache = candidate->m_pageStoreLeases[KisTile::ReadLease];
    auto selected = d->acquireTile(candidate->col(), candidate->row(), KisTilePageStoreBridge::Access::ReadSelection,
        candidate->m_lockCounter ? nullptr : &cache);
    if (!selected || !selected->tileData()) { error(why, "native current tile selection refused"); return {}; }
    const bool selectedDefault = dynamic_cast<Impl::DefaultLease *>(selected.get()) != nullptr;
    auto *data = selected->tileData();
    KisTileSP result = candidate;
    if (candidate->m_lockCounter && data != candidate->m_tileData) {
        result = new KisTile(candidate->col(), candidate->row(), data, nullptr);
        selected->finish();
        result->m_pageStoreNativeReadReady.storeRelease(1);
    } else if (!candidate->m_lockCounter) {
        result->replacePageStoreReadCacheLocked(data);
        selected->finish(); previous = std::move(cache);
        if (indexed) cache = std::move(selected);
        else result->m_pageStoreBridge.storeRelease(nullptr);
        result->m_pageStoreNativeReadReady.storeRelease(1);
    } else selected->finish();
    *present = !selectedDefault;
    return result;
} catch (const std::bad_alloc &) { error(why, "native current tile selection storage refused"); return {}; }
