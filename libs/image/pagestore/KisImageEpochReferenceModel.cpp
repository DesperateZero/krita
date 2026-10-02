/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisImageEpochReferenceModel.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QScopeGuard>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

class KisImageEpochPageRoot
{
public:
    ~KisImageEpochPageRoot();
    KisImageEpochPageRoot(const KisPageVersion &pageVersion,
                          const QSharedPointer<const KisImageEpochPageRoot> &leftChild,
                          const QSharedPointer<const KisImageEpochPageRoot> &rightChild,
                          qint32 nodeHeight,
                          qsizetype nodeCount)
        : version(pageVersion)
        , left(leftChild)
        , right(rightChild)
        , height(nodeHeight)
        , count(nodeCount)
    {
        firstSurface = left ? left->firstSurface : version.key.surface.value;
        lastSurface = right ? right->lastSurface : version.key.surface.value;
        minColumn = maxColumn = version.key.page.column;
        minRow = maxRow = version.key.page.row;
        for (const auto *child : {left.data(), right.data()}) {
            if (!child)
                continue;
            minColumn = qMin(minColumn, child->minColumn);
            maxColumn = qMax(maxColumn, child->maxColumn);
            minRow = qMin(minRow, child->minRow);
            maxRow = qMax(maxRow, child->maxRow);
        }
    }

    KisPageVersion version;
    QSharedPointer<const KisImageEpochPageRoot> left;
    QSharedPointer<const KisImageEpochPageRoot> right;
    qint32 height = 1;
    qsizetype count = 1;
    // All fields are derived immutable index data, never visibility authority.
    // A surface is a contiguous range in the tree's ordering. Its bounds can
    // consume whole interior subtrees and visit only the two boundary paths.
    quint64 firstSurface = 0;
    quint64 lastSurface = 0;
    qint32 minColumn = 0, maxColumn = 0, minRow = 0, maxRow = 0;
};

namespace
{

std::atomic<quint64> s_nextTransactionId{1};
std::atomic<quint64> s_nextSnapshotToken{1};

bool pageKeyLess(const KisPageKey &lhs, const KisPageKey &rhs)
{
    if (lhs.surface.value != rhs.surface.value) {
        return lhs.surface.value < rhs.surface.value;
    }
    if (lhs.page.row != rhs.page.row)
        return lhs.page.row < rhs.page.row;
    return lhs.page.column < rhs.page.column;
}

void sortManifest(QVector<KisPageVersion> *manifest)
{
    std::sort(manifest->begin(), manifest->end(), [](const KisPageVersion &lhs, const KisPageVersion &rhs) {
        return pageKeyLess(lhs.key, rhs.key);
    });
}

using PageRoot = QSharedPointer<const KisImageEpochPageRoot>;

struct TreeReleaseBatch {
    std::deque<PageRoot> roots;
};
thread_local TreeReleaseBatch *activeTreeRelease = nullptr;
std::atomic<quint64> foregroundNodeDestructions{0}, backgroundNodeDestructions{0};
std::atomic<quint64> treeReleasePasses{0}, maximumTreeReleasePass{0};

void scheduleTreeRelease(const std::shared_ptr<TreeReleaseBatch> &batch)
{
    kisSchedulePageStoreReclamation([batch] {
        Q_ASSERT(!activeTreeRelease);
        activeTreeRelease = batch.get();
        quint64 dropped = 0;
        // Node destruction moves its children into this same queue, so even
        // a uniquely owned large tree neither recurses nor becomes one huge
        // worker job. Shared branches are simply released, never traversed.
        while (!batch->roots.empty() && dropped < 128) {
            PageRoot root = std::move(batch->roots.front());
            batch->roots.pop_front();
            root.clear();
            ++dropped;
        }
        activeTreeRelease = nullptr;
        treeReleasePasses.fetch_add(1, std::memory_order_relaxed);
        auto maximum = maximumTreeReleasePass.load(std::memory_order_relaxed);
        while (maximum < dropped
               && !maximumTreeReleasePass.compare_exchange_weak(maximum, dropped, std::memory_order_relaxed)) { }
        if (!batch->roots.empty())
            scheduleTreeRelease(batch);
    });
}

qint32 pageRootHeight(const PageRoot &root)
{
    return root ? root->height : 0;
}

qsizetype pageRootCount(const PageRoot &root)
{
    return root ? root->count : 0;
}

struct PageBounds {
    qint32 minColumn = 1, maxColumn = 0, minRow = 1, maxRow = 0;
    bool isEmpty() const { return minColumn > maxColumn; }
    void include(qint32 left, qint32 right, qint32 top, qint32 bottom)
    {
        const bool wasEmpty = isEmpty();
        minColumn = wasEmpty ? left : qMin(minColumn, left);
        maxColumn = wasEmpty ? right : qMax(maxColumn, right);
        minRow = wasEmpty ? top : qMin(minRow, top);
        maxRow = wasEmpty ? bottom : qMax(maxRow, bottom);
    }
};

void collectPageBounds(const PageRoot &root, quint64 surface, PageBounds *bounds)
{
    if (!root || surface < root->firstSurface || surface > root->lastSurface)
        return;
    if (root->firstSurface == surface && root->lastSurface == surface) {
        bounds->include(root->minColumn, root->maxColumn, root->minRow, root->maxRow);
        return;
    }
    if (root->version.key.surface.value == surface) {
        const auto page = root->version.key.page;
        bounds->include(page.column, page.column, page.row, page.row);
    }
    collectPageBounds(root->left, surface, bounds);
    collectPageBounds(root->right, surface, bounds);
}

void collectPageBoundsExcluding(
    const PageRoot &root, quint64 surface,
    const KisPageKey *first, const KisPageKey *last, PageBounds *bounds)
{
    if (!root || surface < root->firstSurface || surface > root->lastSurface)
        return;
    if (first == last) {
        collectPageBounds(root, surface, bounds);
        return;
    }
    auto split = std::lower_bound(first, last, root->version.key, pageKeyLess);
    collectPageBoundsExcluding(root->left, surface, first, split, bounds);
    if (split != last && *split == root->version.key) {
        ++split;
    } else if (root->version.key.surface.value == surface) {
        const auto page = root->version.key.page;
        bounds->include(page.column, page.column, page.row, page.row);
    }
    collectPageBoundsExcluding(root->right, surface, split, last, bounds);
}

PageRoot makePageRoot(const KisPageVersion &version, const PageRoot &left = {}, const PageRoot &right = {})
{
    return QSharedPointer<KisImageEpochPageRoot>::create(version,
                                                         left,
                                                         right,
                                                         qMax(pageRootHeight(left), pageRootHeight(right)) + 1,
                                                         pageRootCount(left) + pageRootCount(right) + 1);
}

PageRoot rotatePageRootLeft(const PageRoot &root)
{
    const PageRoot pivot = root->right;
    const PageRoot moved = makePageRoot(root->version, root->left, pivot->left);
    return makePageRoot(pivot->version, moved, pivot->right);
}

PageRoot rotatePageRootRight(const PageRoot &root)
{
    const PageRoot pivot = root->left;
    const PageRoot moved = makePageRoot(root->version, pivot->right, root->right);
    return makePageRoot(pivot->version, pivot->left, moved);
}

PageRoot balancePageRoot(const PageRoot &root)
{
    if (!root)
        return {};
    const qint32 balance = pageRootHeight(root->left) - pageRootHeight(root->right);
    if (balance > 1) {
        PageRoot adjusted = root;
        if (pageRootHeight(root->left->left) < pageRootHeight(root->left->right)) {
            adjusted = makePageRoot(root->version, rotatePageRootLeft(root->left), root->right);
        }
        return rotatePageRootRight(adjusted);
    }
    if (balance < -1) {
        PageRoot adjusted = root;
        if (pageRootHeight(root->right->right) < pageRootHeight(root->right->left)) {
            adjusted = makePageRoot(root->version, root->left, rotatePageRootRight(root->right));
        }
        return rotatePageRootLeft(adjusted);
    }
    return root;
}

PageRoot insertPageRoot(const PageRoot &root, const KisPageVersion &version)
{
    if (!root)
        return makePageRoot(version);
    if (version.key == root->version.key) {
        return makePageRoot(version, root->left, root->right);
    }
    if (pageKeyLess(version.key, root->version.key)) {
        return balancePageRoot(makePageRoot(root->version, insertPageRoot(root->left, version), root->right));
    }
    return balancePageRoot(makePageRoot(root->version, root->left, insertPageRoot(root->right, version)));
}

const KisImageEpochPageRoot *minimumPageRoot(const PageRoot &root)
{
    const KisImageEpochPageRoot *node = root.data();
    while (node && node->left)
        node = node->left.data();
    return node;
}

PageRoot removePageRoot(const PageRoot &root, const KisPageKey &key)
{
    if (!root)
        return {};
    if (pageKeyLess(key, root->version.key)) {
        return balancePageRoot(makePageRoot(root->version, removePageRoot(root->left, key), root->right));
    }
    if (pageKeyLess(root->version.key, key)) {
        return balancePageRoot(makePageRoot(root->version, root->left, removePageRoot(root->right, key)));
    }
    if (!root->left)
        return root->right;
    if (!root->right)
        return root->left;
    const KisImageEpochPageRoot *successor = minimumPageRoot(root->right);
    return balancePageRoot(
        makePageRoot(successor->version, root->left, removePageRoot(root->right, successor->version.key)));
}

bool resolvePageRoot(const PageRoot &root, const KisPageKey &key, KisPageVersion *version)
{
    const KisImageEpochPageRoot *node = root.data();
    while (node) {
        if (key == node->version.key) {
            if (version)
                *version = node->version;
            return true;
        }
        node = pageKeyLess(key, node->version.key) ? node->left.data() : node->right.data();
    }
    if (version)
        *version = {};
    return false;
}

template<class Append>
void appendPageRoot(const PageRoot &root, const Append &append)
{
    if (!root)
        return;
    appendPageRoot(root->left, append);
    append(root->version);
    appendPageRoot(root->right, append);
}

template<class Versions>
PageRoot buildPageRoot(const Versions &manifest, qsizetype begin, qsizetype end)
{
    if (begin >= end)
        return {};
    const qsizetype middle = begin + (end - begin) / 2;
    return makePageRoot(manifest.at(middle),
                        buildPageRoot(manifest, begin, middle),
                        buildPageRoot(manifest, middle + 1, end));
}

bool resolveSurface(const QVector<KisSurfaceEpochState> &surfaces, KisSurfaceId surface, KisSurfaceEpochState *state)
{
    for (const KisSurfaceEpochState &candidate : surfaces) {
        if (candidate.surface == surface) {
            if (state)
                *state = candidate;
            return true;
        }
    }
    if (state)
        *state = {};
    return false;
}

bool matchesRetainedRootMetadata(const KisImageEpochRootSnapshot &root, const KisRetainedImageEpochSnapshot &retained)
{
    // The retained token selects an immutable root owned by this model. The
    // caller's manifest, when present, is a serialization payload and never
    // drives validation or restore; comparing/exporting it here would turn
    // every history operation back into an O(N) hot-path scan.
    const KisImageEpochSnapshot &snapshot = retained.snapshot;
    return root.epoch() == snapshot.epoch && root.graphRevision() == snapshot.graphRevision
        && root.defaultPixelRevision() == snapshot.defaultPixelRevision
        && root.extentRevision() == snapshot.extentRevision && root.propertyRevision() == snapshot.propertyRevision
        && root.pageCount() == retained.retainedPageCount() && root.surfaces() == snapshot.surfaces;
}

} // namespace

KisImageEpochPageRoot::~KisImageEpochPageRoot()
{
    (kisOnPageStoreReclamationThread() ? backgroundNodeDestructions : foregroundNodeDestructions)
        .fetch_add(1, std::memory_order_relaxed);
    if (!left && !right)
        return;
    if (activeTreeRelease) {
        if (left)
            activeTreeRelease->roots.push_back(std::move(left));
        if (right)
            activeTreeRelease->roots.push_back(std::move(right));
    } else {
        auto batch = std::make_shared<TreeReleaseBatch>();
        if (left)
            batch->roots.push_back(std::move(left));
        if (right)
            batch->roots.push_back(std::move(right));
        scheduleTreeRelease(batch);
    }
}

KisPageTreeReclamationStatistics kisPageTreeReclamationStatistics()
{
    return {foregroundNodeDestructions.load(std::memory_order_relaxed),
            backgroundNodeDestructions.load(std::memory_order_relaxed),
            treeReleasePasses.load(std::memory_order_relaxed),
            maximumTreeReleasePass.load(std::memory_order_relaxed)};
}

bool KisImageEpochRootSnapshot::isValid() const
{
    // Fields are private and immutable after the model validates construction.
    // Rebuilding a temporary surface set here made every prepared install
    // allocate again, even though no root field could have changed.
    return m_validated;
}

bool KisImageEpochRootSnapshot::validate() const
{
    if (!m_epoch.isValid() || m_commitSequence == 0 || m_graphRevision == 0 || m_defaultPixelRevision == 0
        || m_extentRevision == 0 || m_propertyRevision == 0) {
        return false;
    }
    if ((m_commitSequence == 1) == m_previousEpoch.isValid())
        return false;

    QSet<quint64> surfaceIds;
    surfaceIds.reserve(m_surfaces.size());
    for (const KisSurfaceEpochState &surface : m_surfaces) {
        if (!surface.isValid() || surfaceIds.contains(surface.surface.value)) {
            return false;
        }
        surfaceIds.insert(surface.surface.value);
    }
    return true;
}

QVector<KisPageVersion> KisImageEpochRootSnapshot::manifest() const
{
    QVector<KisPageVersion> result;
    result.reserve(pageRootCount(m_pageRoot));
    appendPageRoot(m_pageRoot, [&](const auto &version) { result.append(version); });
    return result;
}

void KisImageEpochRootSnapshot::copyPageVersions(KisPageVersion *output) const
{
    appendPageRoot(m_pageRoot, [&](const auto &version) { *output++ = version; });
}

qsizetype KisImageEpochRootSnapshot::pageCount() const
{
    return pageRootCount(m_pageRoot);
}

qint32 KisImageEpochRootSnapshot::pageTreeHeight() const
{
    return pageRootHeight(m_pageRoot);
}

bool KisImageEpochRootSnapshot::containsPage(const KisPageKey &key, KisPageVersion *version) const
{
    if (!m_epoch.isValid() || m_commitSequence == 0 || !key.isValid()) {
        if (version)
            *version = {};
        return false;
    }
    return resolvePageRoot(m_pageRoot, key, version);
}

KisImageEpochSnapshot KisImageEpochRootSnapshot::snapshot() const
{
    KisImageEpochSnapshot result;
    result.epoch = m_epoch;
    result.graphRevision = m_graphRevision;
    result.defaultPixelRevision = m_defaultPixelRevision;
    result.extentRevision = m_extentRevision;
    result.propertyRevision = m_propertyRevision;
    result.manifest = manifest();
    result.surfaces = m_surfaces;
    return result;
}

bool KisImageEpochRootSnapshot::resolve(const KisPageKey &key, KisPageVersion *version) const
{
    // Roots are fully validated once, before they are installed in the model.
    // Revalidating the complete immutable manifest for every single-page read
    // turns an iterator walk into O(page_count^2).
    if (!m_epoch.isValid() || m_commitSequence == 0 || !key.isValid()) {
        if (version)
            *version = {};
        return false;
    }
    if (resolvePageRoot(m_pageRoot, key, version))
        return true;

    KisSurfaceEpochState surface;
    if (!resolveSurface(m_surfaces, key.surface, &surface)) {
        if (version)
            *version = {};
        return false;
    }
    if (version) {
        *version = {key, KisPageGeneration{1}, surface.defaultPixelRevision};
    }
    return true;
}

bool KisImageEpochRootSnapshot::surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
{
    return m_epoch.isValid() && m_commitSequence != 0 && surface.isValid()
        && resolveSurface(m_surfaces, surface, state);
}

bool KisImageEpochRootSnapshot::contentExtentAfterDelta(KisSurfaceId surface,
                                                        QSize pageExtent,
                                                        const KisPreparedPageSet &delta,
                                                        QRect *extent) const
{
    if (extent)
        *extent = {};
    if (!extent || !m_epoch.isValid() || !surface.isValid() || pageExtent.width() <= 0 || pageExtent.height() <= 0)
        return false;
    // This is a bounds query, not a new page root. Partition sorted removals
    // along their search paths and consume cached bounds for untouched
    // subtrees. Do not construct/dispose persistent nodes just to measure them.
    std::vector<KisPageKey> removals;
    removals.reserve(size_t(delta.removedPages.size()));
    for (const auto &key : delta.removedPages) {
        if (!key.isValid())
            return false;
        if (key.surface == surface)
            removals.push_back(key);
    }
    std::sort(removals.begin(), removals.end(), pageKeyLess);
    removals.erase(std::unique(removals.begin(), removals.end()), removals.end());
    std::vector<KisPageKey> additions;
    additions.reserve(size_t(delta.proofs.size()));
    for (const auto &proof : delta.proofs) {
        const auto &version = proof.authority.version;
        if (!version.isValid())
            return false;
        additions.push_back(version.key);
    }
    return contentExtentAfterPages(surface, pageExtent, removals.data(), removals.size(),
                                   additions.data(), additions.size(), extent);
}

bool KisImageEpochRootSnapshot::contentExtentAfterPages(
    KisSurfaceId surface, QSize pageExtent, const KisPageKey *removals, size_t removalCount,
    const KisPageKey *additions, size_t additionCount, QRect *extent) const
{
    if (extent) *extent = {};
    if (!extent || !m_epoch.isValid() || !surface.isValid()
        || pageExtent.width() <= 0 || pageExtent.height() <= 0) return false;
    PageBounds bounds;
    // Avoid pointer arithmetic on an empty array's null data().
    collectPageBoundsExcluding(m_pageRoot, surface.value, removals,
                               removalCount ? removals + removalCount : removals, &bounds);
    for (size_t i = 0; i < additionCount; ++i) {
        if (additions[i].surface == surface) {
            const auto page = additions[i].page;
            bounds.include(page.column, page.column, page.row, page.row);
        }
    }
    if (bounds.isEmpty())
        return true;
    const qint64 left = qint64(bounds.minColumn) * pageExtent.width();
    const qint64 top = qint64(bounds.minRow) * pageExtent.height();
    const qint64 right = (qint64(bounds.maxColumn) + 1) * pageExtent.width() - 1;
    const qint64 bottom = (qint64(bounds.maxRow) + 1) * pageExtent.height() - 1;
    // QRect stores signed int dimensions as well as coordinates. Individually
    // representable endpoints alone do not make their union representable.
    const qint64 limit = std::numeric_limits<int>::max();
    if (left < std::numeric_limits<int>::min() || top < std::numeric_limits<int>::min() || right > limit
        || bottom > limit || right - left + 1 > limit || bottom - top + 1 > limit)
        return false;
    *extent = QRect(int(left), int(top), int(right - left + 1), int(bottom - top + 1));
    return true;
}

template<class T>
using EpochArray = std::vector<T, KisMutationStorageAllocator<T>>;

struct EpochTransactionChanges {
    explicit EpochTransactionChanges(const KisMutationStorageAllocator<EpochTransactionChanges> &storage)
        : changes(storage), surfaceChanges(storage), removedPages(storage) {}
    EpochArray<KisPageVersion> changes;
    EpochArray<KisSurfaceEpochChange> surfaceChanges;
    EpochArray<KisPageKey> removedPages;
    bool empty() const { return changes.empty() && surfaceChanges.empty() && removedPages.empty(); }
    bool matches(const KisPreparedPageSet &input) const
    {
        if (changes.size() != size_t(input.proofs.size()) || surfaceChanges.size() != size_t(input.surfaceChanges.size())
            || removedPages.size() != size_t(input.removedPages.size())) return false;
        for (const auto &proof : input.proofs) {
            const auto &version = proof.authority.version;
            const auto found = std::lower_bound(changes.begin(), changes.end(), version.key,
                [](const auto &value, const auto &key) { return pageKeyLess(value.key, key); });
            if (found == changes.end() || !(*found == version)) return false;
        }
        for (const auto &surface : input.surfaceChanges)
            if (std::find(surfaceChanges.begin(), surfaceChanges.end(), surface) == surfaceChanges.end()) return false;
        return std::all_of(input.removedPages.begin(), input.removedPages.end(),
            [&](const auto &key) { return std::binary_search(removedPages.begin(), removedPages.end(), key, pageKeyLess); });
    }
};

struct EpochTransactionRecord {
    KisPageTransaction transaction;
    KisPageTransactionState state = KisPageTransactionState::Invalid;
    std::shared_ptr<const EpochTransactionChanges> delta;
    quint64 revision = 0;
    quint64 nextFinished = 0;
    bool isActive() const
    {
        return state == KisPageTransactionState::Open || state == KisPageTransactionState::Prepared;
    }
};

struct EpochRootRecord {
    KisImageEpochRootSnapshot root;
    quint64 nextCandidate = 0;
    quint64 previousProtected = 0;
    quint64 nextProtected = 0;
};

class KisImageEpochReferenceModel::Private
{
public:
    bool operational() const { return current.epoch().isValid(); }

    const EpochTransactionRecord *activeTransaction(KisPageTransactionId transaction) const
    {
        const auto it = transactions.constFind(transaction.value);
        return it != transactions.constEnd() && it->isActive() ? &it.value() : nullptr;
    }

    // Logical admission is independent of deferred physical root destruction.
    // Every caller holds mutex, including final release/finish/publication.
    // Once an old root loses its last protection it cannot be resurrected by
    // an epoch number while its tree happens to remain in the retirement queue.
    bool admitsRoot(quint64 epoch) const
    {
        return epoch != 0
            && (epoch == current.epoch().value || retainedRootCounts.contains(epoch)
                || transactionBaseCounts.contains(epoch));
    }

    const KisImageEpochRootSnapshot &protectedRoot(quint64 epoch) const
    {
        // Admission, token ownership and active transactions are checked under
        // this same mutex. Collection cannot remove their published roots.
        const auto it = roots.constFind(epoch);
        Q_ASSERT(admitsRoot(epoch) && it != roots.constEnd() && it->root.isValid());
        return it->root;
    }

    struct ReachabilityScan {
        quint64 cookie = 0;
        KisPageKey key;
        quint64 next = 0;
        quint64 last = 0;
        bool invalidated = false;
    };

    void linkProtected(quint64 epoch)
    {
        auto record = roots.find(epoch);
        Q_ASSERT(record != roots.end());
        if (record->previousProtected || record->nextProtected || protectedHead == epoch) return;
        record->previousProtected = protectedTail;
        if (protectedTail) roots.find(protectedTail)->nextProtected = epoch;
        else protectedHead = epoch;
        protectedTail = epoch;
    }

    void unlinkUnprotected(quint64 epoch)
    {
        if (admitsRoot(epoch)) return;
        auto record = roots.find(epoch);
        Q_ASSERT(record != roots.end());
        const quint64 previous = record->previousProtected;
        const quint64 next = record->nextProtected;
        if (!previous && !next && protectedHead != epoch) return;
        // Adjust at most 16 value cursors under the same protection gate. No
        // QHash iterator, root copy, or second retention table escapes it.
        for (auto &scan : reachabilityScans) {
            if (!scan.cookie) continue;
            if (scan.next == epoch) scan.next = scan.last == epoch ? 0 : next;
            if (scan.last == epoch) scan.last = previous;
        }
        if (previous) roots.find(previous)->nextProtected = next;
        else protectedHead = next;
        if (next) roots.find(next)->previousProtected = previous;
        else protectedTail = previous;
        record->previousProtected = record->nextProtected = 0;
    }

    void invalidateChangedScans(const KisImageEpochRootSnapshot &next)
    {
        if (!activeReachabilityScans) return;
        for (auto &scan : reachabilityScans) {
            if (!scan.cookie || scan.invalidated) continue;
            KisPageVersion beforeVersion, afterVersion;
            current.resolve(scan.key, &beforeVersion);
            next.resolve(scan.key, &afterVersion);
            if (!(beforeVersion == afterVersion)) scan.invalidated = true;
        }
    }

    void queueRoot(quint64 epoch)
    {
        if (!epoch)
            return;
        Q_ASSERT(roots.isDetached());
        auto record = roots.find(epoch);
        Q_ASSERT(record != roots.end() && record->root.epoch().isValid());
        if (record->nextCandidate || rootCandidatesTail == epoch)
            return;
        if (rootCandidatesTail)
            roots.find(rootCandidatesTail)->nextCandidate = epoch;
        else
            rootCandidatesHead = epoch;
        rootCandidatesTail = epoch;
    }
    void finishTransaction(const KisPageTransaction &transaction)
    {
        Q_ASSERT(transactions.isDetached() && transactionBaseCounts.isDetached());
        auto it = transactionBaseCounts.find(transaction.baseEpoch.value);
        Q_ASSERT(it != transactionBaseCounts.end() && it.value() > 0);
        if (--it.value() == 0) {
            transactionBaseCounts.erase(it);
            unlinkUnprotected(transaction.baseEpoch.value);
            queueRoot(transaction.baseEpoch.value);
        }
        --activeTransactions;
        auto record = transactions.find(transaction.id.value);
        Q_ASSERT(record != transactions.end() && !record->isActive()
                 && record->nextFinished == 0);
        if (finishedTransactionsTail)
            transactions.find(finishedTransactionsTail)->nextFinished = transaction.id.value;
        else
            finishedTransactionsHead = transaction.id.value;
        finishedTransactionsTail = transaction.id.value;
        ++finishedTransactionCount;
    }
    mutable QMutex mutex;
    // Native stores attach before initialization; standalone oracle models use
    // their own controller. Core/Qt registry bootstrap remains separate debt.
    std::unique_ptr<KisBackingBudgetController> standaloneBudget;
    KisBackingBudgetController *budget = nullptr;
    quint64 nextEpoch = 1;
    KisImageEpochRootSnapshot current;
    // Registry values own their retirement links. Queuing needs neither an
    // allocation nor a separate dedup set after metadata installation or final
    // protection release. Use identities, not QHash value pointers/iterators:
    // rehash and erase can relocate values. These registries are never copied,
    // so ordinary lookups/erases do not detach a shared QHash during install.
    QHash<quint64, EpochRootRecord> roots;
    qsizetype reservedRootCount = 0;
    QHash<quint64, EpochTransactionRecord> transactions;
    QHash<quint64, qsizetype> transactionBaseCounts;
    qsizetype activeTransactions = 0;
    quint64 finishedTransactionsHead = 0;
    quint64 finishedTransactionsTail = 0;
    qsizetype finishedTransactionCount = 0;
    quint64 rootCandidatesHead = 0;
    quint64 rootCandidatesTail = 0;
    QHash<quint64, KisImageEpochId> retainedSnapshots;
    QHash<quint64, quint64> retainedRootCounts;
    quint64 protectedHead = 0;
    quint64 protectedTail = 0;
    quint64 nextReachabilityCookie = 1;
    qsizetype activeReachabilityScans = 0;
    std::array<ReachabilityScan, ReachabilityScanLimit> reachabilityScans{};
};

KisImageEpochReferenceModel::PreparedRootReservation::~PreparedRootReservation()
{
    cancel();
}

KisImageEpochReferenceModel::PreparedRootReservation &
KisImageEpochReferenceModel::PreparedRootReservation::operator=(PreparedRootReservation &&other) noexcept
{
    if (this != &other) {
        cancel();
        m_owner = std::move(other.m_owner);
        m_root = std::move(other.m_root);
    }
    return *this;
}

void KisImageEpochReferenceModel::PreparedRootReservation::cancel()
{
    if (!isValid())
        return;
    // Only an invalid, unpublished placeholder belongs to this capability.
    // Keep the owner alive until after its mutex is unlocked.
    auto owner = std::move(m_owner);
    QMutexLocker locker(&owner->mutex);
    const auto it = owner->roots.find(m_root.epoch().value);
    Q_ASSERT(it != owner->roots.end() && !it->root.epoch().isValid()
             && !it->nextCandidate && owner->rootCandidatesTail != m_root.epoch().value);
    owner->roots.erase(it);
    --owner->reservedRootCount;
}

KisImageEpochReferenceModel::KisImageEpochReferenceModel()
    : d(new Private)
{
}

KisImageEpochReferenceModel::~KisImageEpochReferenceModel() = default;

void KisImageEpochReferenceModel::attachBackingBudget(KisBackingBudgetController &budget)
{
    QMutexLocker lock(&d->mutex);
    Q_ASSERT(!d->operational() && !d->budget);
    d->budget = &budget;
}

bool KisImageEpochReferenceModel::initialize(const KisImageEpochSnapshot &initial, QString *error)
{
    if (!initial.isValid() || initial.epoch.value == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial image epoch is invalid or exhausted"));
        return false;
    }

    QMutexLocker locker(&d->mutex);
    if (d->operational()) {
        KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is already initialized"));
        return false;
    }

    if (!d->budget) {
        d->standaloneBudget = std::make_unique<KisBackingBudgetController>();
        d->budget = d->standaloneBudget.get();
    }

    KisImageEpochRootSnapshot root;
    root.m_epoch = initial.epoch;
    root.m_commitSequence = 1;
    root.m_graphRevision = initial.graphRevision;
    root.m_defaultPixelRevision = initial.defaultPixelRevision;
    root.m_extentRevision = initial.extentRevision;
    root.m_propertyRevision = initial.propertyRevision;
    QVector<KisPageVersion> manifest = initial.manifest;
    root.m_surfaces = initial.surfaces;
    sortManifest(&manifest);
    for (qsizetype i = 0; i < manifest.size(); ++i) {
        if (!manifest.at(i).isValid() || (i > 0 && !pageKeyLess(manifest.at(i - 1).key, manifest.at(i).key))) {
            KisPageStoreDetail::setError(error, QStringLiteral("initial image epoch manifest is invalid"));
            return false;
        }
    }
    root.m_pageRoot = buildPageRoot(manifest, 0, manifest.size());
    root.m_validated = root.validate();
    if (!root.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial image epoch root is invalid"));
        return false;
    }

    d->roots.insert(root.epoch().value, EpochRootRecord{root});
    d->current = root;
    d->linkProtected(root.epoch().value);
    d->nextEpoch = root.epoch().value + 1;
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisPageTransaction KisImageEpochReferenceModel::beginTransaction(KisImageEpochId baseEpoch, QString *error)
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || !baseEpoch.isValid() || !d->admitsRoot(baseEpoch.value)) {
        KisPageStoreDetail::setError(error,
                 QStringLiteral(
                     "transaction base epoch is no longer protected, unavailable, or identity space is exhausted"));
        return {};
    }

    KisPageTransaction transaction;
    transaction.id = KisPageStoreDetail::allocateMonotonicId<KisPageTransactionId>(&s_nextTransactionId);
    if (!transaction.id.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction identity space is exhausted"));
        return {};
    }
    transaction.baseEpoch = baseEpoch;

    EpochTransactionRecord record;
    record.transaction = transaction;
    record.state = KisPageTransactionState::Open;
    auto &baseCount = d->transactionBaseCounts[transaction.baseEpoch.value];
    const auto cancelEmptyCount = qScopeGuard([&] {
        if (!baseCount) d->transactionBaseCounts.remove(transaction.baseEpoch.value);
    });
    d->transactions.insert(transaction.id.value, record);
    ++baseCount;
    ++d->activeTransactions;
    KisPageStoreDetail::setError(error, {});
    return transaction;
}

bool KisImageEpochReferenceModel::prepare(const KisPreparedPageSet &preparedPages, QString *error)
{
    return prepareImpl(preparedPages, false, error);
}

bool KisImageEpochReferenceModel::preparePublication(const KisPreparedPageSet &preparedPages, QString *error)
{
    return prepareImpl(preparedPages, true, error);
}

bool KisImageEpochReferenceModel::prepareImpl(const KisPreparedPageSet &preparedPages, bool complete, QString *error) try
{
    if (!preparedPages.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page set is invalid"));
        return false;
    }

    // Working and superseded storage outlive the locker, so actual deallocation
    // and budget observers run outside the epoch gate on every exit.
    std::shared_ptr<const EpochTransactionChanges> previous;
    std::shared_ptr<EpochTransactionChanges> next;
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(preparedPages.transaction.value);
    if (!d->operational() || transactionIt == d->transactions.end()
        || !transactionIt->isActive()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction is not open for preparation"));
        return false;
    }
    // A complete retry with identical facts reuses the accepted immutable
    // value and revision; it cannot invalidate an already prepared root.
    if (complete && transactionIt->delta && transactionIt->delta->matches(preparedPages)) {
        KisPageStoreDetail::setError(error, {});
        return true;
    }
    if (transactionIt->revision == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction revision space is exhausted"));
        return false;
    }

    const auto base = d->protectedRoot(transactionIt->transaction.baseEpoch.value);
    const quint64 revision = transactionIt->revision;
    previous = complete ? nullptr : transactionIt->delta;
    locker.unlock();
    const auto storage = KisMutationStorageAllocator<EpochTransactionChanges>::retained(d->budget);
    next = std::allocate_shared<EpochTransactionChanges>(storage, storage);
    const size_t previousChanges = previous ? previous->changes.size() : 0;
    next->changes.reserve(previousChanges + size_t(preparedPages.proofs.size()));
    next->surfaceChanges.reserve((previous ? previous->surfaceChanges.size() : 0)
                                 + size_t(preparedPages.surfaceChanges.size()));
    next->removedPages.reserve((previous ? previous->removedPages.size() : 0)
                               + size_t(preparedPages.removedPages.size()));
    if (previous) {
        next->changes.insert(next->changes.end(), previous->changes.begin(), previous->changes.end());
        next->surfaceChanges.insert(next->surfaceChanges.end(), previous->surfaceChanges.begin(), previous->surfaceChanges.end());
        next->removedPages.insert(next->removedPages.end(), previous->removedPages.begin(), previous->removedPages.end());
    }
    for (const KisPreparedPageProof &proof : preparedPages.proofs) {
        const KisPageVersion &after = proof.authority.version;
        KisPageVersion before;
        const bool hadBefore = base.resolve(after.key, &before);
        if (hadBefore && after.generation.value <= before.generation.value) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared generation does not advance its base"));
            return false;
        }

        // The original prefix remains sorted while unique new input keys are
        // appended. Paid capacity was admitted before any element changes.
        const auto end = next->changes.begin() + previousChanges;
        const auto existing = std::lower_bound(next->changes.begin(), end, after.key,
            [](const auto &value, const auto &key) { return pageKeyLess(value.key, key); });
        if (existing != end && existing->key == after.key) {
            if (after.generation.value <= existing->generation.value) {
                KisPageStoreDetail::setError(error, QStringLiteral("prepared generation does not advance the transaction chain"));
                return false;
            }
            *existing = after;
        } else {
            next->changes.push_back(after);
        }
    }

    std::sort(next->changes.begin(), next->changes.end(), [](const KisPageVersion &lhs, const KisPageVersion &rhs) {
        return pageKeyLess(lhs.key, rhs.key);
    });
    for (const KisSurfaceEpochChange &change : preparedPages.surfaceChanges) {
        KisSurfaceEpochState baseState;
        if (!base.surfaceState(change.before.surface, &baseState)
            || !(baseState == change.before)) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared surface metadata does not match the transaction base"));
            return false;
        }
        const auto existing = std::find_if(next->surfaceChanges.begin(), next->surfaceChanges.end(),
            [&](const auto &value) { return value.after.surface == change.after.surface; });
        if (existing != next->surfaceChanges.end()) {
            if (!(*existing == change)) {
                KisPageStoreDetail::setError(error, QStringLiteral("surface metadata was prepared with competing values"));
                return false;
            }
        } else {
            next->surfaceChanges.push_back(change);
        }
    }
    for (const KisPageKey &key : preparedPages.removedPages) {
        if (!base.containsPage(key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("removed page is absent from the transaction base or also written"));
            return false;
        }
        next->removedPages.push_back(key);
    }
    std::sort(next->removedPages.begin(), next->removedPages.end(), pageKeyLess);
    next->removedPages.erase(std::unique(next->removedPages.begin(), next->removedPages.end()), next->removedPages.end());
    for (const auto &key : next->removedPages) {
        const auto written = std::lower_bound(next->changes.begin(), next->changes.end(), key,
            [](const auto &value, const auto &key) { return pageKeyLess(value.key, key); });
        if (written != next->changes.end() && written->key == key) {
            KisPageStoreDetail::setError(error, QStringLiteral("removed page is also written in the transaction"));
            return false;
        }
    }
    locker.relock();
    transactionIt = d->transactions.find(preparedPages.transaction.value);
    if (transactionIt == d->transactions.end() || !transactionIt->isActive() || transactionIt->revision != revision) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction changed during write set preparation"));
        return false;
    }
    // Keep the replaced immutable value alive until after the gate unlock.
    previous = std::move(transactionIt->delta);
    transactionIt->delta = std::move(next);
    transactionIt->state = KisPageTransactionState::Prepared;
    ++transactionIt->revision;
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("epoch transaction storage admission failed"));
    return false;
}

KisImageEpochCommitResult KisImageEpochReferenceModel::commit(const KisPageTransaction &transaction)
{
    KisImageEpochCommitResult result;
    auto candidate = prepareCommit(transaction, &result);
    if (!candidate.isValid())
        return result;
    return installCommit(std::move(candidate), nullptr, nullptr);
}

KisImageEpochReferenceModel::PreparedCommit
KisImageEpochReferenceModel::prepareCommit(const KisPageTransaction &transaction, KisImageEpochCommitResult *failure) try
{
    KisImageEpochCommitResult localFailure;
    KisImageEpochCommitResult &result = failure ? *failure : localFailure;
    result = {};
    std::shared_ptr<const EpochTransactionChanges> prepared;
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(transaction.id.value);
    if (!d->operational() || !transaction.isValid() || transactionIt == d->transactions.end()
        || !(transactionIt->transaction == transaction)
        || transactionIt->state != KisPageTransactionState::Prepared
        || !transactionIt->delta || transactionIt->delta->empty()) {
        result.error = QStringLiteral("transaction is not prepared for commit");
        return {};
    }

    const KisImageEpochRootSnapshot base = d->protectedRoot(transaction.baseEpoch.value);

    if (d->nextEpoch == 0 || d->nextEpoch == std::numeric_limits<quint64>::max()
        || d->current.commitSequence() == std::numeric_limits<quint64>::max()) {
        result.error = QStringLiteral("image epoch identity space is exhausted");
        return {};
    }
    const KisImageEpochId epoch{d->nextEpoch++};
    const quint64 revision = transactionIt->revision;
    prepared = transactionIt->delta;
    const KisImageEpochRootSnapshot current = d->current;
    // All inputs below are immutable snapshots. No QHash iterator or mutable
    // transaction state may be used until it is freshly looked up on install.
    locker.unlock();

    for (const KisPageVersion &change : prepared->changes) {
        KisPageVersion baseVersion;
        KisPageVersion currentVersion;
        const bool baseContains = base.resolve(change.key, &baseVersion);
        const bool currentContains = current.resolve(change.key, &currentVersion);
        if (baseContains != currentContains || (baseContains && !(baseVersion == currentVersion))) {
            result.status = KisImageEpochCommitStatus::Conflict;
            result.error = QStringLiteral("modified page changed after the transaction base epoch");
            return {};
        }
    }
    for (const KisSurfaceEpochChange &change : prepared->surfaceChanges) {
        KisSurfaceEpochState currentState;
        if (!current.surfaceState(change.before.surface, &currentState) || !(currentState == change.before)) {
            result.status = KisImageEpochCommitStatus::Conflict;
            result.error = QStringLiteral("surface metadata changed after the transaction base epoch");
            return {};
        }
    }
    for (const KisPageKey &key : prepared->removedPages) {
        KisPageVersion baseVersion;
        KisPageVersion currentVersion;
        if (!base.containsPage(key, &baseVersion) || !current.containsPage(key, &currentVersion)
            || !(baseVersion == currentVersion)) {
            result.status = KisImageEpochCommitStatus::Conflict;
            result.error = QStringLiteral("removed page changed after the transaction base epoch");
            return {};
        }
    }

    KisImageEpochRootSnapshot root;
    root.m_epoch = epoch;
    root.m_previousEpoch = current.epoch();
    root.m_commitSequence = current.commitSequence() + 1;
    root.m_graphRevision = current.graphRevision();
    root.m_defaultPixelRevision = current.defaultPixelRevision();
    root.m_extentRevision = current.extentRevision();
    root.m_propertyRevision = current.propertyRevision();
    root.m_pageRoot = current.m_pageRoot;
    root.m_surfaces = current.m_surfaces;
    const qsizetype pageDeltaSize = qsizetype(prepared->changes.size() + prepared->removedPages.size());
    const qsizetype currentPageCount = current.pageCount();
    const bool densePageDelta = pageDeltaSize > 0 && pageDeltaSize >= (currentPageCount + 3) / 4;
    if (densePageDelta) {
        // Repeated path-copy is ideal for sparse K, but needlessly allocates
        // K*log(N) nodes when most leaves change. A dense delta is rebuilt
        // once in O(N+K); because N <= 4K here, fixed-K commits can never
        // fall into this path as the document grows.
        EpochArray<KisPageVersion> manifest(prepared->changes.get_allocator());
        manifest.reserve(size_t(currentPageCount) + prepared->changes.size());
        auto change = prepared->changes.cbegin();
        auto removed = prepared->removedPages.cbegin();
        appendPageRoot(current.m_pageRoot, [&](const KisPageVersion &version) {
            while (change != prepared->changes.end() && pageKeyLess(change->key, version.key))
                manifest.push_back(*change++);
            while (removed != prepared->removedPages.end() && pageKeyLess(*removed, version.key))
                ++removed;
            if (removed != prepared->removedPages.end() && *removed == version.key) return;
            manifest.push_back(change != prepared->changes.end() && change->key == version.key ? *change++ : version);
        });
        manifest.insert(manifest.end(), change, prepared->changes.end());
        root.m_pageRoot = buildPageRoot(manifest, 0, manifest.size());
    } else {
        for (const KisPageVersion &change : prepared->changes) {
            root.m_pageRoot = insertPageRoot(root.m_pageRoot, change);
        }
        for (const KisPageKey &key : prepared->removedPages) {
            root.m_pageRoot = removePageRoot(root.m_pageRoot, key);
        }
    }
    for (const KisSurfaceEpochChange &change : prepared->surfaceChanges) {
        bool replaced = false;
        for (KisSurfaceEpochState &surface : root.m_surfaces) {
            if (surface.surface == change.after.surface) {
                surface = change.after;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            result.error = QStringLiteral("committed surface metadata has no registered base");
            return {};
        }
        root.m_defaultPixelRevision = qMax(root.m_defaultPixelRevision, change.after.defaultPixelRevision);
        root.m_extentRevision = qMax(root.m_extentRevision, change.after.extentRevision);
    }
    root.m_validated = root.validate();
    if (!root.isValid()) {
        result.error = QStringLiteral("committed image epoch root failed validation");
        return {};
    }

    locker.relock();
    transactionIt = d->transactions.find(transaction.id.value);
    if (!(d->current.epoch() == current.epoch()) || transactionIt == d->transactions.end()
        || transactionIt->revision != revision || transactionIt->state != KisPageTransactionState::Prepared) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or transaction changed during candidate preparation");
        return {};
    }
    // Reserve the hash node before publication, but never expose a candidate
    // through root(), retention, transaction bases, or reachability queries.
    d->roots.insert(root.epoch().value, EpochRootRecord{});
    ++d->reservedRootCount;
    PreparedCommit candidate;
    candidate.m_owner = d;
    candidate.m_root = root;
    candidate.m_transaction = transaction.id;
    candidate.m_revision = revision;
    return candidate;
}
catch (const std::bad_alloc &) {
    if (failure) failure->error = QStringLiteral("epoch root preparation storage admission failed");
    return {};
}

KisImageEpochCommitResult KisImageEpochReferenceModel::installCommit(PreparedCommit &&candidate,
                                                                     void *context,
                                                                     InstallMetadataFunction installMetadata)
{
    // Consume even on rejection; cancellation must run after the mutex unlock.
    PreparedCommit consumed(std::move(candidate));
    KisImageEpochCommitResult result;
    if (!consumed.isValid() || consumed.m_owner != d) {
        result.error = QStringLiteral("prepared epoch candidate has a different owner or was consumed");
        return result;
    }
    QMutexLocker locker(&d->mutex);
    const auto &root = consumed.m_root;
    auto transactionIt = d->transactions.find(consumed.m_transaction.value);
    if (!(d->current.epoch() == root.previousEpoch()) || transactionIt == d->transactions.end()
        || transactionIt->revision != consumed.m_revision
        || transactionIt->state != KisPageTransactionState::Prepared) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or transaction changed before candidate installation");
        return result;
    }
    if (!installReservedRoot(consumed, context, installMetadata, &result))
        return result;

    const KisPageTransaction transaction = transactionIt->transaction;
    transactionIt->state = KisPageTransactionState::Committed;
    d->finishTransaction(transaction);
    d->queueRoot(root.previousEpoch().value);
    return result;
}

bool KisImageEpochReferenceModel::installReservedRoot(
    PreparedRootReservation &candidate, void *context, InstallMetadataFunction installMetadata,
    KisImageEpochCommitResult *result)
{
    auto rootIt = d->roots.find(candidate.m_root.epoch().value);
    Q_ASSERT(rootIt != d->roots.end() && !rootIt->root.epoch().isValid());
    if (installMetadata && !installMetadata(context, candidate.m_root.epoch())) {
        result->error = QStringLiteral("prepared metadata publication was rejected");
        return false;
    }
    rootIt.value().root = candidate.m_root;
    d->invalidateChangedScans(candidate.m_root);
    const quint64 previous = d->current.epoch().value;
    d->current = candidate.m_root;
    d->linkProtected(d->current.epoch().value);
    d->unlinkUnprotected(previous);
    --d->reservedRootCount;
    candidate.m_owner.clear();
    result->status = KisImageEpochCommitStatus::Committed;
    result->root = candidate.m_root;
    return true;
}

KisImageEpochReferenceModel::PreparedRootReservation
KisImageEpochReferenceModel::prepareRestore(const KisRetainedImageEpochSnapshot &retained,
                                            KisImageEpochCommitResult *failure)
{
    KisImageEpochCommitResult localFailure;
    KisImageEpochCommitResult &result = failure ? *failure : localFailure;
    result = {};
    QMutexLocker locker(&d->mutex);
    const auto retainedIt = d->retainedSnapshots.constFind(retained.token.value);
    if (!d->operational() || !retained.isValid() || retainedIt == d->retainedSnapshots.constEnd()
        || !(retainedIt.value() == retained.snapshot.epoch)
        || !matchesRetainedRootMetadata(d->protectedRoot(retained.snapshot.epoch.value), retained)) {
        result.error = QStringLiteral("retained image epoch is not owned by this model");
        return {};
    }
    if (d->activeTransactions != 0) {
        result.error = QStringLiteral("historical restore conflicts with an active transaction");
        return {};
    }
    if (d->nextEpoch == 0 || d->nextEpoch == std::numeric_limits<quint64>::max()
        || d->current.commitSequence() == std::numeric_limits<quint64>::max()) {
        result.error = QStringLiteral("image epoch identity space is exhausted");
        return {};
    }

    // The token owns this already validated immutable content. Only the new
    // publication identity changes, after the exhaustion checks above.
    KisImageEpochRootSnapshot restored = d->protectedRoot(retained.snapshot.epoch.value);
    restored.m_epoch = KisImageEpochId{d->nextEpoch++};
    restored.m_previousEpoch = d->current.epoch();
    restored.m_commitSequence = d->current.commitSequence() + 1;
    // Reserve the index node before installation can change live metadata.
    // The model gate excludes captures through the complete restore batch.
    d->roots.insert(restored.epoch().value, {});
    ++d->reservedRootCount;
    PreparedRootReservation candidate;
    candidate.m_owner = d;
    candidate.m_root = restored;
    return candidate;
}

KisImageEpochCommitResult KisImageEpochReferenceModel::installRestore(PreparedRootReservation &&candidate,
                                                                      void *context,
                                                                      InstallMetadataFunction installMetadata)
{
    PreparedRootReservation consumed(std::move(candidate));
    KisImageEpochCommitResult result;
    if (!consumed.isValid() || consumed.m_owner != d) {
        result.error = QStringLiteral("prepared restore candidate has a different owner or was consumed");
        return result;
    }
    QMutexLocker locker(&d->mutex);
    const KisImageEpochRootSnapshot &restored = consumed.m_root;
    if (!(d->current.epoch() == restored.previousEpoch()) || d->activeTransactions != 0) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or transaction changed before restore installation");
        return result;
    }
    if (!installReservedRoot(consumed, context, installMetadata, &result))
        return result;
    d->queueRoot(restored.previousEpoch().value);
    return result;
}

bool KisImageEpochReferenceModel::abort(const KisPageTransaction &transaction, QString *error)
{
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(transaction.id.value);
    if (!d->operational() || !transaction.isValid() || transactionIt == d->transactions.end()
        || !(transactionIt->transaction.baseEpoch == transaction.baseEpoch)
        || !transactionIt->isActive()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction cannot be aborted"));
        return false;
    }
    transactionIt->state = KisPageTransactionState::Aborted;
    d->finishTransaction(transaction);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::captureCommittedRoot() const
{
    QMutexLocker locker(&d->mutex);
    return d->current;
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::retainRootLocked(
    const KisImageEpochRootSnapshot &root,
    bool completeManifest)
{
    KisRetainedImageEpochSnapshot retained;
    retained.token = KisPageStoreDetail::allocateMonotonicId<KisImageEpochSnapshotToken>(&s_nextSnapshotToken);
    if (!retained.token.isValid())
        return {};
    if (completeManifest) {
        retained.snapshot = root.snapshot();
    } else {
        retained.snapshot.epoch = root.epoch();
        retained.snapshot.graphRevision = root.graphRevision();
        retained.snapshot.defaultPixelRevision = root.defaultPixelRevision();
        retained.snapshot.extentRevision = root.extentRevision();
        retained.snapshot.propertyRevision = root.propertyRevision();
        retained.snapshot.surfaces = root.surfaces();
    }
    if (!completeManifest)
        retained.pageCount = root.pageCount();
    auto &rootCount = d->retainedRootCounts[root.epoch().value];
    const auto cancelEmptyCount = qScopeGuard([&] {
        if (!rootCount) d->retainedRootCounts.remove(root.epoch().value);
    });
    d->retainedSnapshots.insert(retained.token.value, root.epoch());
    ++rootCount;
    return retained;
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureRetainedSnapshot()
{
    QMutexLocker locker(&d->mutex);
    return captureCurrentRetainedRootLocked(true);
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureRetainedRoot()
{
    QMutexLocker locker(&d->mutex);
    return captureCurrentRetainedRootLocked(false);
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureCurrentRetainedRootLocked(
    bool completeManifest)
{
    if (!d->operational())
        return {};
    return retainRootLocked(d->protectedRoot(d->current.epoch().value), completeManifest);
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::retainSnapshot(KisImageEpochId epoch)
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || !epoch.isValid() || !d->admitsRoot(epoch.value)) {
        return {};
    }
    return retainRootLocked(d->protectedRoot(epoch.value), false);
}

bool KisImageEpochReferenceModel::validateRetainedSnapshot(const KisRetainedImageEpochSnapshot &retained) const
{
    if (!retained.isValid())
        return false;
    QMutexLocker locker(&d->mutex);
    const auto retainedIt = d->retainedSnapshots.constFind(retained.token.value);
    if (!d->operational() || retainedIt == d->retainedSnapshots.constEnd()
        || !(retainedIt.value() == retained.snapshot.epoch)) {
        return false;
    }
    return matchesRetainedRootMetadata(d->protectedRoot(retained.snapshot.epoch.value), retained);
}

bool KisImageEpochReferenceModel::releaseSnapshot(KisImageEpochSnapshotToken token,
                                                  QString *error,
                                                  bool *rootBecameUnretained)
{
    if (rootBecameUnretained)
        *rootBecameUnretained = false;
    QMutexLocker locker(&d->mutex);
    auto snapshotIt = d->retainedSnapshots.find(token.value);
    if (!d->operational() || !token.isValid() || snapshotIt == d->retainedSnapshots.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("image epoch snapshot token is stale"));
        return false;
    }
    const quint64 epoch = snapshotIt->value;
    d->retainedSnapshots.erase(snapshotIt);
    auto countIt = d->retainedRootCounts.find(epoch);
    Q_ASSERT(countIt != d->retainedRootCounts.end() && countIt.value() > 0);
    if (--countIt.value() == 0) {
        d->retainedRootCounts.erase(countIt);
        d->unlinkUnprotected(epoch);
        if (epoch != d->current.epoch().value && !d->transactionBaseCounts.contains(epoch)) {
            d->queueRoot(epoch);
            if (rootBecameUnretained)
                *rootBecameUnretained = true;
        }
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

qsizetype KisImageEpochReferenceModel::retainedSnapshotCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->retainedSnapshots.size();
}

qsizetype KisImageEpochReferenceModel::rootCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->roots.size() - d->reservedRootCount;
}

qsizetype KisImageEpochReferenceModel::activeTransactionCount() const
{
    QMutexLocker locker(&d->mutex);
    return d->activeTransactions;
}

qsizetype KisImageEpochReferenceModel::collectFinishedTransactions(qsizetype budget)
{
    std::shared_ptr<const EpochTransactionChanges> released;
    QMutexLocker locker(&d->mutex);
    const qsizetype limit = budget < 0 ? d->finishedTransactionCount : qMin(budget, d->finishedTransactionCount);
    qsizetype removed = 0;
    while (removed < limit && d->finishedTransactionsHead) {
        const auto record = d->transactions.find(d->finishedTransactionsHead);
        Q_ASSERT(record != d->transactions.end() && !record->isActive());
        d->finishedTransactionsHead = record->nextFinished;
        released = std::move(record->delta);
        d->transactions.erase(record);
        --d->finishedTransactionCount;
        if (!d->finishedTransactionsHead) d->finishedTransactionsTail = 0;
        ++removed;
        locker.unlock();
        released.reset();
        locker.relock();
    }
    return removed;
}

qsizetype KisImageEpochReferenceModel::collectUnretainedRoots(qsizetype budget)
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return 0;
    qsizetype removed = 0;
    while (d->rootCandidatesHead && budget != 0) {
        if (budget > 0)
            --budget;
        const quint64 epoch = d->rootCandidatesHead;
        const auto record = d->roots.find(epoch);
        Q_ASSERT(record != d->roots.end());
        d->rootCandidatesHead = record->nextCandidate;
        if (!d->rootCandidatesHead)
            d->rootCandidatesTail = 0;
        record->nextCandidate = 0;
        if (!d->admitsRoot(epoch)) {
            d->roots.erase(record);
            ++removed;
        }
    }
    return removed;
}

bool KisImageEpochReferenceModel::hasCollectionWork() const
{
    QMutexLocker locker(&d->mutex);
    return d->finishedTransactionsHead != 0 || d->rootCandidatesHead != 0;
}

KisImageEpochReferenceModel::ReachabilityStart
KisImageEpochReferenceModel::beginReachabilityScan(const KisPageKey &key)
{
    QMutexLocker lock(&d->mutex);
    if (!d->operational() || !key.isValid() ||
        d->nextReachabilityCookie == std::numeric_limits<quint64>::max()) return {};
    for (auto &scan : d->reachabilityScans) {
        if (scan.cookie) continue;
        scan = {};
        scan.cookie = d->nextReachabilityCookie++;
        ++d->activeReachabilityScans;
        scan.key = key;
        // Capture the current version identity first (one charged root visit). A
        // later publication with the same key value needs no restart, even if
        // this particular root loses protection before the next slice.
        const auto current = d->roots.constFind(d->current.epoch().value);
        Q_ASSERT(current != d->roots.cend() && !current->nextProtected);
        scan.last = current->previousProtected;
        scan.next = scan.last ? d->protectedHead : 0;
        KisPageVersion version;
        d->current.resolve(key, &version);
        return {scan.cookie, version};
    }
    return {};
}

KisImageEpochReferenceModel::ReachabilitySlice
KisImageEpochReferenceModel::advanceReachabilityScan(quint64 cookie, qsizetype rootBudget)
{
    QMutexLocker lock(&d->mutex);
    ReachabilitySlice result;
    if (!cookie) return result;
    for (auto &scan : d->reachabilityScans) {
        if (scan.cookie != cookie) continue;
        if (scan.invalidated) return result;
        result.valid = true;
        const auto budget = std::clamp(rootBudget, qsizetype(0), ReachabilityRootBudget);
        while (scan.next && result.rootsVisited < budget) {
            const auto root = d->roots.constFind(scan.next);
            Q_ASSERT(root != d->roots.cend() && d->admitsRoot(scan.next));
            KisPageVersion version;
            root->root.resolve(scan.key, &version);
            result.versions[size_t(result.rootsVisited++)] = version;
            scan.next = scan.next == scan.last ? 0 : root->nextProtected;
        }
        result.complete = scan.next == 0;
        return result;
    }
    return result;
}

void KisImageEpochReferenceModel::endReachabilityScan(quint64 cookie)
{
    if (!cookie) return;
    QMutexLocker lock(&d->mutex);
    for (auto &scan : d->reachabilityScans) {
        if (scan.cookie == cookie) { scan = {}; --d->activeReachabilityScans; return; }
    }
}

QSet<KisPageVersion> KisImageEpochReferenceModel::reachablePageVersions(const QVector<KisPageKey> &registeredKeys,
                                                                        quint64 *visitedRoots) const
{
    QMutexLocker locker(&d->mutex);
    QSet<KisPageVersion> result;
    if (visitedRoots)
        *visitedRoots = 0;
    if (!d->operational() || registeredKeys.isEmpty())
        return result;
    auto visit = [&](quint64 epoch) {
        const auto root = d->roots.constFind(epoch);
        Q_ASSERT(root != d->roots.constEnd() && root->root.epoch().isValid());
        if (visitedRoots)
            ++*visitedRoots;
        for (const KisPageKey &key : registeredKeys) {
            KisPageVersion version;
            if (root->root.resolve(key, &version))
                result.insert(version);
        }
    };
    // Enumerate protected identities, not every retired tree or snapshot
    // token. The two existing counted indices avoid a second authority and a
    // temporary deduplication set. Current and multiply-protected roots are
    // resolved exactly once. Unprotected roots cannot acquire new admission.
    const quint64 current = d->current.epoch().value;
    visit(current);
    for (auto it = d->retainedRootCounts.constBegin(); it != d->retainedRootCounts.constEnd(); ++it) {
        if (it.key() != current)
            visit(it.key());
    }
    for (auto it = d->transactionBaseCounts.constBegin(); it != d->transactionBaseCounts.constEnd(); ++it) {
        if (it.key() != current && !d->retainedRootCounts.contains(it.key()))
            visit(it.key());
    }
    return result;
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::root(KisImageEpochId epoch) const
{
    QMutexLocker locker(&d->mutex);
    return d->operational() && d->admitsRoot(epoch.value) ? d->protectedRoot(epoch.value)
                                                        : KisImageEpochRootSnapshot();
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::retainedRoot(KisImageEpochSnapshotToken token,
                                                                    KisImageEpochId epoch) const
{
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || !token.isValid() || !epoch.isValid() || !(d->retainedSnapshots.value(token.value) == epoch))
        return {};
    return d->protectedRoot(epoch.value);
}

KisPageTransactionSnapshot KisImageEpochReferenceModel::transaction(KisPageTransactionId id) const
{
    KisPageTransactionSnapshot result;
    std::shared_ptr<const EpochTransactionChanges> delta;
    QMutexLocker locker(&d->mutex);
    const auto found = d->transactions.constFind(id.value);
    if (found == d->transactions.cend()) return {};
    result.transaction = found->transaction;
    result.state = found->state;
    delta = found->delta;
    locker.unlock();
    if (delta) {
        result.changes = QVector<KisPageVersion>(delta->changes.begin(), delta->changes.end());
        result.surfaceChanges = QVector<KisSurfaceEpochChange>(delta->surfaceChanges.begin(), delta->surfaceChanges.end());
        result.removedPages = QVector<KisPageKey>(delta->removedPages.begin(), delta->removedPages.end());
    }
    return result;
}

KisPageTransaction KisImageEpochReferenceModel::activeTransaction(KisPageTransactionId id) const
{
    QMutexLocker lock(&d->mutex);
    const auto *record = d->activeTransaction(id);
    return record ? record->transaction : KisPageTransaction{};
}

bool KisImageEpochReferenceModel::resolve(const KisPageKey &key,
                                          const KisPageReadView &view,
                                          KisPageVersion *version) const
{
    const auto fail = [version] {
        if (version) *version = {};
        return false;
    };
    if (!view.isValidFor(key))
        return fail();

    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return fail();

    switch (view.kind) {
    case KisPageReadViewKind::CurrentCommittedEpoch:
        return d->current.resolve(key, version);
    case KisPageReadViewKind::CommittedEpoch:
        if (!(d->retainedSnapshots.value(view.retention.value) == view.epoch))
            return fail();
        return d->protectedRoot(view.epoch.value).resolve(key, version);
    case KisPageReadViewKind::TransactionBaseEpoch: {
        const auto *transaction = d->activeTransaction(view.transaction);
        if (!transaction)
            return fail();
        return d->protectedRoot(transaction->transaction.baseEpoch.value).resolve(key, version);
    }
    case KisPageReadViewKind::TransactionOverlay: {
        const auto *transaction = d->activeTransaction(view.transaction);
        if (!transaction)
            return fail();
        if (transaction->delta) {
            const auto &changes = transaction->delta->changes;
            const auto change = std::lower_bound(changes.begin(), changes.end(), key,
                [](const auto &value, const auto &key) { return pageKeyLess(value.key, key); });
            if (change != changes.end() && change->key == key) {
                if (version) *version = *change;
                return true;
            }
        }
        return d->protectedRoot(transaction->transaction.baseEpoch.value).resolve(key, version);
    }
    case KisPageReadViewKind::ExactVersion: {
        if (!(d->retainedSnapshots.value(view.retention.value) == view.epoch))
            return fail();
        KisPageVersion retainedVersion;
        if (!d->protectedRoot(view.epoch.value).resolve(key, &retainedVersion)
            || !(retainedVersion == view.exactVersion))
            return fail();
        if (version)
            *version = retainedVersion;
        return true;
    }
    }
    return fail();
}

bool KisImageEpochReferenceModel::surfaceState(KisSurfaceId surface,
                                               const KisPageReadView &view,
                                               KisSurfaceEpochState *state) const
{
    if (!surface.isValid() || !state)
        return false;

    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return false;

    switch (view.kind) {
    case KisPageReadViewKind::CurrentCommittedEpoch:
        return d->current.surfaceState(surface, state);
    case KisPageReadViewKind::CommittedEpoch:
    case KisPageReadViewKind::ExactVersion:
        if (!view.epoch.isValid() || !(d->retainedSnapshots.value(view.retention.value) == view.epoch)) {
            return false;
        }
        return d->protectedRoot(view.epoch.value).surfaceState(surface, state);
    case KisPageReadViewKind::TransactionBaseEpoch:
    case KisPageReadViewKind::TransactionOverlay: {
        const auto *transaction = d->activeTransaction(view.transaction);
        if (!transaction) {
            return false;
        }
        return d->protectedRoot(transaction->transaction.baseEpoch.value).surfaceState(surface, state);
    }
    }
    return false;
}
