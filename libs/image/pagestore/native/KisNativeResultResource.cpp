/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"

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
        if (!p || bytes != p->descriptor.minimumByteSize() || target == Domain::Unknown || source == target) {
#ifdef NR104_CENSUS
            qWarning("104 domain birth mismatch bytes=%llu expected=%llu", bytes, p ? p->descriptor.minimumByteSize() : 0);
#endif
            return {};
        }
        Resource *r;
        {
            QMutexLocker lock(&p->gate);
            auto it = p->physical.find(physical.slot);
            // Residency callbacks can arrive inside the provider's own copy
            // service. Consume the original registered identity here; calling
            // providerId()/providerEpoch() would re-lock that provider mutex.
            if (it == p->physical.end() || !(it->second->handle.provider == physical.provider)
                || !(it->second->handle.providerEpoch == physical.providerEpoch)
                || it->second->busy || it->second->domain != source
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
        if (before->handle.version.key == version.key) {
            made = provider->prepareSynchronousWriteCopy(nextOperation(), before->handle, version, descriptor, KisPagePriority::Interactive);
        } else {
            // A shared immutable Resource may be selected by several logical
            // pages. Its physical source is valid without pretending its birth
            // key is the destination key of this new COW result.
            auto input = provider->captureCompletedTileSource(descriptor, before->tile, why);
            if (!input) return nullptr;
            made = provider->prepareSynchronousSource(nextOperation(), input, version, descriptor,
                KisReplicaSourceUse::WritableCopy, KisPagePriority::Interactive);
        }
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

