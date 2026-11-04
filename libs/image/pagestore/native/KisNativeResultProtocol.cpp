/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "KisNativeResultState_p.h"

KisNativeResultProtocol &KisNativeResultTestAccess::protocol(KisTiledDataManager &manager)
{
    Q_ASSERT(manager.m_nativeResult);
    return *manager.m_nativeResult;
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
    bool cancelHistory = false, initiateClose = false;
    {
        QMutexLocker historySequence(&d->historyGate);
        {
            QMutexLocker lock(&d->gate); unfinished = d->currentHistory; d->currentHistory = {};
            initiateClose = !d->closing; d->closing = true;
            d->notifyWriters();
            if (unfinished && unfinished->m_nativeHistory) {
                unfinished->m_nativeHistory->phase = KisNativeHistory::Phase::ClosingAbort;
                cancelHistory = !unfinished->m_nativeHistory->workers;
            }
        }
        if (cancelHistory) d->cancelHistorySegment(unfinished->m_nativeHistory);
    }
    unfinished = {};
    Impl::Resource *releasedIdle = nullptr;
    {
        QMutexLocker lock(&d->gate);
        if (initiateClose) {
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
        out.acceptedEpoch = d->epoch;
        for (const auto &page : d->pages) out.historyEdges += page.second.history.size();
        for (const auto &body : d->bodies) out.rowCapacity += body.rows.capacity();
    }
    { QMutexLocker lock(&d->terminalGate); out.terminalRegistrations = d->terminalCount; }
    return out;
}

