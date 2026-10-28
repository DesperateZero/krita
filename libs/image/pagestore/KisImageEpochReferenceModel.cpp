/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisImageEpochReferenceModel.h"
#include "KisPageStoreReclamation_p.h"
#include "KisPageWriteCoordinator_p.h"
#include "KisPagePublicationCoordinator_p.h"
#include "KisPageViewValue_p.h"

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QScopeGuard>

#include <algorithm>
#include <atomic>
#include <limits>
#include <map>
#include <memory>
#include <utility>
#include <vector>
#include <boost/iterator/transform_iterator.hpp>

template<class T>
using EpochArray = std::vector<T, KisMutationStorageAllocator<T>>;

template<class Value>
using EpochRegistry = std::map<quint64, Value, std::less<quint64>,
    KisMutationStorageAllocator<std::pair<const quint64, Value>>>;

template<class Value>
typename EpochRegistry<Value>::node_type prepareEpochNode(const EpochRegistry<Value> &registry,
                                                         quint64 key, Value value = {})
{
    EpochRegistry<Value> prepared(registry.key_comp(), registry.get_allocator());
    prepared.emplace(key, std::move(value));
    return prepared.extract(prepared.begin());
}

class KisImageEpochSurfaces : public EpochArray<KisSurfaceEpochState>
{
public:
    using EpochArray<KisSurfaceEpochState>::vector;
};

namespace {
struct EpochValueUnavailable {};

class EpochOracleValues final : public KisPageViewValueStorage
{
public:
    explicit EpochOracleValues(KisMutationStorageAllocator<KisPageViewValue> storage)
        : storage(std::move(storage)) {}
    KisPageViewValue *pin(const KisPageVersion &version, KisVersionSlotId = {},
                         const KisPagePrivateWrite * = nullptr) override
    {
        auto *value = storage.allocate(1);
        new (value) KisPageViewValue;
        value->version = version;
        retain(value);
        return value;
    }
    KisPageViewValue *prepareInitial(const KisPageVersion &version) override { return pin(version); }
private:
    void releaseMemory(const KisPageKey &, KisVersionSlotId, KisPageViewValue *value) noexcept override
    {
        std::destroy_at(value);
        storage.deallocate(value, 1);
    }
    KisMutationStorageAllocator<KisPageViewValue> storage;
};

// Borrowed construction input only; no row, tree node or transaction owns
// this projection. Existing tree/delta values carry their final identity.
// A genuine external identity is resolved once when its new node is built.
struct EpochPageInput {
    const KisPageVersion *identity;
    KisPageViewValue *value = nullptr;
    KisVersionSlotId slot;
    const KisPagePrivateWrite *original = nullptr;
    const KisPageVersion &version() const { return *identity; }
    KisPageViewValue *pin(KisPageViewValueStorage &storage) const
    {
        if (value) { KisPageViewValueStorage::retain(value); return value; }
        auto *result = storage.pin(*identity, slot, original);
        if (!result) throw EpochValueUnavailable{};
        return result;
    }
};
}

class KisImageEpochTreeStorage : public std::enable_shared_from_this<KisImageEpochTreeStorage>
{
public:
    KisImageEpochTreeStorage(KisMutationStorageAllocator<KisImageEpochPageRoot> allocator,
                            std::shared_ptr<KisPageViewValueStorage> values)
        : nodes(std::move(allocator)), values(std::move(values)) {}
    void prepare(KisBackingBudgetController *budget);
    void enqueue(KisImageEpochPageRoot *node) noexcept;
    KisMutationStorageAllocator<KisImageEpochPageRoot> nodes;
    std::shared_ptr<KisPageViewValueStorage> values;
private:
    void drain() noexcept;
    void finished() noexcept;
    QMutex mutex;
    KisImageEpochPageRoot *first = nullptr;
    KisImageEpochPageRoot *last = nullptr;
    KisPageReclamationJobPointer task;
    std::shared_ptr<KisImageEpochTreeStorage> taskPin;
};

class KisImageEpochPageRoot
{
public:
    KisImageEpochPageRoot(std::shared_ptr<KisImageEpochTreeStorage> owner,
                          KisPageViewValue *pageValue,
                          const boost::intrusive_ptr<const KisImageEpochPageRoot> &leftChild,
                          const boost::intrusive_ptr<const KisImageEpochPageRoot> &rightChild,
                          qint32 nodeHeight,
                          qsizetype nodeCount)
        : storage(std::move(owner))
        , value(pageValue)
        , left(leftChild)
        , right(rightChild)
        , height(nodeHeight)
        , count(nodeCount)
    {
        const auto &version = value->version;
        firstSurface = left ? left->firstSurface : version.key.surface.value;
        lastSurface = right ? right->lastSurface : version.key.surface.value;
        minColumn = maxColumn = version.key.page.column;
        minRow = maxRow = version.key.page.row;
        for (const auto *child : {left.get(), right.get()}) {
            if (!child)
                continue;
            minColumn = qMin(minColumn, child->minColumn);
            maxColumn = qMax(maxColumn, child->maxColumn);
            minRow = qMin(minRow, child->minRow);
            maxRow = qMax(maxRow, child->maxRow);
        }
    }

    ~KisImageEpochPageRoot() { Q_ASSERT(!liveParents); storage->values->release(value); }

    // Only admitted root records and live parents contribute semantic edges.
    // Immutable subtrees are shared: an already-live node does not revisit its
    // children. Activate the new root before deactivating the old root.
    void activate() const noexcept
    {
        Q_ASSERT(liveParents != std::numeric_limits<quint64>::max());
        if (liveParents++) return;
        const auto before = value->rootEdges.fetch_add(1, std::memory_order_release);
        Q_ASSERT(before != std::numeric_limits<quint64>::max()); Q_UNUSED(before);
        if (left) left->activate();
        if (right) right->activate();
    }
    void deactivate() const noexcept
    {
        Q_ASSERT(liveParents);
        if (--liveParents) return;
        if (left) left->deactivate();
        if (right) right->deactivate();
        const auto before = value->rootEdges.fetch_sub(1, std::memory_order_release);
        Q_ASSERT(before); Q_UNUSED(before);
    }

    std::shared_ptr<KisImageEpochTreeStorage> storage;
    // Nodes have strong owners only. The original allocation carries the
    // count; its zero transition hands this same node to the existing queue.
    // The constructor's first reference is adopted by makePageRoot.
    mutable std::atomic<size_t> references{1};
    mutable quint64 liveParents = 0; // Original epoch gate; never a memory owner.
    KisImageEpochPageRoot *nextReleased = nullptr;
    KisPageViewValue *value;
    boost::intrusive_ptr<const KisImageEpochPageRoot> left;
    boost::intrusive_ptr<const KisImageEpochPageRoot> right;
    qint32 height = 1;
    qsizetype count = 1;
    // All fields are derived immutable index data, never visibility authority.
    // A surface is a contiguous range in the tree's ordering. Its bounds can
    // consume whole interior subtrees and visit only the two boundary paths.
    quint64 firstSurface = 0;
    quint64 lastSurface = 0;
    qint32 minColumn = 0, maxColumn = 0, minRow = 0, maxRow = 0;
};

void intrusive_ptr_add_ref(const KisImageEpochPageRoot *node) noexcept
{
    node->references.fetch_add(1, std::memory_order_relaxed);
}

void intrusive_ptr_release(const KisImageEpochPageRoot *node) noexcept
{
    if (node->references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        node->storage->enqueue(const_cast<KisImageEpochPageRoot *>(node));
}

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

using PageRoot = boost::intrusive_ptr<const KisImageEpochPageRoot>;
std::atomic<quint64> backgroundNodeDestructions{0};
std::atomic<quint64> foregroundNodeDestructions{0};
std::atomic<quint64> treeReleasePasses{0}, maximumTreeReleasePass{0};

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
    if (root->value->version.key.surface.value == surface) {
        const auto page = root->value->version.key.page;
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
    auto split = std::lower_bound(first, last, root->value->version.key, pageKeyLess);
    collectPageBoundsExcluding(root->left, surface, first, split, bounds);
    if (split != last && *split == root->value->version.key) {
        ++split;
    } else if (root->value->version.key.surface.value == surface) {
        const auto page = root->value->version.key.page;
        bounds->include(page.column, page.column, page.row, page.row);
    }
    collectPageBoundsExcluding(root->right, surface, split, last, bounds);
}

PageRoot makeOwnedPageRoot(const std::shared_ptr<KisImageEpochTreeStorage> &storage,
                           KisPageViewValue *value, const PageRoot &left = {}, const PageRoot &right = {})
{
    if (!value) throw EpochValueUnavailable{};
    auto allocator = storage->nodes;
    KisImageEpochPageRoot *node = nullptr;
    try {
        node = allocator.allocate(1);
        std::allocator_traits<decltype(allocator)>::construct(allocator, node, storage, value, left, right,
            qMax(pageRootHeight(left), pageRootHeight(right)) + 1,
            pageRootCount(left) + pageRootCount(right) + 1);
    } catch (...) {
        if (node) allocator.deallocate(node, 1);
        storage->values->release(value);
        throw;
    }
    return PageRoot(node, false);
}

PageRoot makePageRoot(const std::shared_ptr<KisImageEpochTreeStorage> &storage,
                      KisPageViewValue *value, const PageRoot &left = {}, const PageRoot &right = {})
{
    KisPageViewValueStorage::retain(value);
    return makeOwnedPageRoot(storage, value, left, right);
}

PageRoot makeSelectedPageRoot(const std::shared_ptr<KisImageEpochTreeStorage> &storage,
                              const EpochPageInput &input, const PageRoot &left, const PageRoot &right)
{
    return makeOwnedPageRoot(storage, input.pin(*storage->values), left, right);
}

// Build only the final balanced shape. A not-yet-published parent is input,
// not an independently owned node that must be allocated and then retired
// merely to discover that the join needs a single or double rotation.
PageRoot balancePageRoot(const std::shared_ptr<KisImageEpochTreeStorage> &storage,
                         KisPageViewValue *value, const PageRoot &left, const PageRoot &right)
{
    const qint32 balance = pageRootHeight(left) - pageRootHeight(right);
    if (balance > 1) {
        if (pageRootHeight(left->left) < pageRootHeight(left->right)) {
            const auto &pivot = left->right;
            return makePageRoot(storage, pivot->value,
                makePageRoot(storage, left->value, left->left, pivot->left),
                makePageRoot(storage, value, pivot->right, right));
        }
        return makePageRoot(storage, left->value, left->left,
            makePageRoot(storage, value, left->right, right));
    }
    if (balance < -1) {
        if (pageRootHeight(right->right) < pageRootHeight(right->left)) {
            const auto &pivot = right->left;
            return makePageRoot(storage, pivot->value,
                makePageRoot(storage, value, left, pivot->left),
                makePageRoot(storage, right->value, pivot->right, right->right));
        }
        return makePageRoot(storage, right->value,
            makePageRoot(storage, value, left, right->left), right->right);
    }
    return makePageRoot(storage, value, left, right);
}

// A batch may change subtree height by more than one. Join down the taller
// spine before balancing; a single rotation at the old root is insufficient.
PageRoot joinPageRoot(const std::shared_ptr<KisImageEpochTreeStorage> &storage,
                      KisPageViewValue *value, const PageRoot &left, const PageRoot &right)
{
    if (pageRootHeight(left) > pageRootHeight(right) + 1) {
        return balancePageRoot(storage, left->value, left->left,
            joinPageRoot(storage, value, left->right, right));
    }
    if (pageRootHeight(right) > pageRootHeight(left) + 1) {
        return balancePageRoot(storage, right->value,
            joinPageRoot(storage, value, left, right->left), right->right);
    }
    return makePageRoot(storage, value, left, right);
}

PageRoot joinPageRoots(const PageRoot &left, const PageRoot &right)
{
    if (!left) return right;
    if (!right) return left;
    // Remove the pivot in the same traversal that joins the ordered trees.
    // The left child's height can shrink by several levels after a batch.
    return joinPageRoot(right->storage, right->value,
        joinPageRoots(left, right->left), right->right);
}

KisPageViewValue *resolvePageValue(const PageRoot &root, const KisPageKey &key)
{
    const KisImageEpochPageRoot *node = root.get();
    while (node) {
        if (key == node->value->version.key) return node->value;
        node = pageKeyLess(key, node->value->version.key) ? node->left.get() : node->right.get();
    }
    return nullptr;
}

bool resolvePageRoot(const PageRoot &root, const KisPageKey &key, KisPageVersion *version)
{
    auto *value = resolvePageValue(root, key);
    if (version) *version = value ? value->version : KisPageVersion{};
    return value;
}

template<class Append>
void appendPageRoot(const PageRoot &root, const Append &append)
{
    if (!root)
        return;
    appendPageRoot(root->left, append);
    append(root->value->version);
    appendPageRoot(root->right, append);
}

template<class Versions>
PageRoot buildPageRoot(const Versions &manifest, qsizetype begin, qsizetype end,
                       const std::shared_ptr<KisImageEpochTreeStorage> &storage)
{
    if (begin >= end)
        return {};
    const qsizetype middle = begin + (end - begin) / 2;
    const auto left = buildPageRoot(manifest, begin, middle, storage);
    const auto right = buildPageRoot(manifest, middle + 1, end, storage);
    return makeOwnedPageRoot(storage, storage->values->prepareInitial(manifest.at(middle)), left, right);
}

// Borrow the immutable inputs for this candidate's construction. The AVL
// frontier is bounded by twice the bit width of its representable node count;
// neither this cursor nor its emitted references need owning storage.
template<class ChangeIterator, class RemovalIterator>
class PageDeltaCursor
{
public:
    PageDeltaCursor(const PageRoot &root, ChangeIterator first, ChangeIterator last,
                    RemovalIterator removedFirst, RemovalIterator removedLast)
        : change(first), changeEnd(last), removed(removedFirst), removedEnd(removedLast)
    { pushLeft(root.get()); }

    EpochPageInput next()
    {
        for (;;) {
            const auto *base = depth ? frontier[depth - 1] : nullptr;
            if (change != changeEnd && (!base || pageKeyLess(change->version().key, base->value->version.key)))
                return *change++;
            if (!base) return {nullptr};
            --depth;
            pushLeft(base->right.get());
            while (removed != removedEnd && pageKeyLess(*removed, base->value->version.key)) ++removed;
            if (removed != removedEnd && *removed == base->value->version.key) {
                ++removed;
                continue;
            }
            return change != changeEnd && change->version().key == base->value->version.key
                ? *change++ : EpochPageInput{&base->value->version, base->value};
        }
    }
private:
    void pushLeft(const KisImageEpochPageRoot *node)
    {
        while (node) {
            Q_ASSERT(depth < frontier.size());
            frontier[depth++] = node;
            node = node->left.get();
        }
    }
    std::array<const KisImageEpochPageRoot *, 2 * std::numeric_limits<qsizetype>::digits> frontier;
    size_t depth = 0;
    ChangeIterator change, changeEnd;
    RemovalIterator removed, removedEnd;
};

template<class Cursor>
PageRoot buildMergedPageRoot(Cursor &input, size_t count,
                             const std::shared_ptr<KisImageEpochTreeStorage> &storage)
{
    if (!count) return {};
    const size_t leftCount = count / 2;
    const auto left = buildMergedPageRoot(input, leftCount, storage);
    const auto value = input.next();
    Q_ASSERT(value.identity);
    // Both original input owners remain captured while the right subtree is
    // built. Advancing the cursor never changes an emitted version's address.
    const auto right = buildMergedPageRoot(input, count - leftCount - 1, storage);
    return makeSelectedPageRoot(storage, value, left, right);
}

template<class Changes, class Removals>
PageRoot applyPageRootDelta(const PageRoot &root,
                           Changes first, Changes last, Removals removedFirst, Removals removedLast,
                           const std::shared_ptr<KisImageEpochTreeStorage> &storage)
{
    if (first == last && removedFirst == removedLast)
        return root;
    if (!root) {
        if (first == last) return {};
        const auto middle = std::next(first, std::distance(first, last) / 2);
        return makeSelectedPageRoot(storage, *middle,
            applyPageRootDelta({}, first, middle, removedFirst, removedFirst, storage),
            applyPageRootDelta({}, std::next(middle), last, removedFirst, removedFirst, storage));
    }
    const auto change = std::lower_bound(first, last, root->value->version.key,
        [](const auto &value, const auto &key) { return pageKeyLess(value.version().key, key); });
    const auto removed = std::lower_bound(removedFirst, removedLast, root->value->version.key, pageKeyLess);
    const bool replaces = change != last && change->version().key == root->value->version.key;
    const bool removes = removed != removedLast && *removed == root->value->version.key;
    const PageRoot left = applyPageRootDelta(root->left, first, change, removedFirst, removed, storage);
    const PageRoot right = applyPageRootDelta(root->right, replaces ? std::next(change) : change, last,
        removes ? std::next(removed) : removed, removedLast, storage);
    if (removes)
        return joinPageRoots(left, right);
    if (!replaces) return joinPageRoot(storage, root->value, left, right);
    auto *value = change->pin(*storage->values);
    const auto release = qScopeGuard([&] { storage->values->release(value); });
    return joinPageRoot(storage, value, left, right);
}

const KisSurfaceEpochState *findSurface(const KisImageEpochSurfaces &surfaces, KisSurfaceId surface)
{
    for (const KisSurfaceEpochState &candidate : surfaces) {
        if (candidate.surface == surface) return &candidate;
    }
    return nullptr;
}

bool resolveSurface(const KisImageEpochSurfaces &surfaces, KisSurfaceId surface, KisSurfaceEpochState *state)
{
    if (const auto *found = findSurface(surfaces, surface)) {
        if (state) *state = *found;
        return true;
    }
    if (state)
        *state = {};
    return false;
}

} // namespace

bool KisImageEpochReferenceModel::matchesRetainedRootMetadata(
    const KisImageEpochRootSnapshot &root, const KisRetainedImageEpochSnapshot &retained)
{
    // The retained token selects an immutable root owned by this model. The
    // caller's manifest, when present, is a serialization payload and never
    // drives validation or restore; comparing/exporting it here would turn
    // every history operation back into an O(N) hot-path scan.
    const KisImageEpochSnapshot &snapshot = retained.snapshot;
    return root.epoch() == snapshot.epoch && root.graphRevision() == snapshot.graphRevision
        && root.defaultPixelRevision() == snapshot.defaultPixelRevision
        && root.extentRevision() == snapshot.extentRevision && root.propertyRevision() == snapshot.propertyRevision
        && root.pageCount() == retained.retainedPageCount()
        && root.surfaceCount() == size_t(snapshot.surfaces.size())
        && std::equal(snapshot.surfaces.begin(), snapshot.surfaces.end(), root.m_surfaces->begin());
}

void KisImageEpochTreeStorage::prepare(KisBackingBudgetController *budget)
{
    task = kisPreparePageStoreReclamation([this] { drain(); }, budget,
        [](void *context) { static_cast<KisImageEpochTreeStorage *>(context)->finished(); }, this);
}

void KisImageEpochTreeStorage::enqueue(KisImageEpochPageRoot *node) noexcept
{
    QMutexLocker lock(&mutex);
    Q_ASSERT(node && !node->references.load(std::memory_order_relaxed)
        && !node->nextReleased && last != node);
    if (last) last->nextReleased = node;
    else first = node;
    last = node;
    if (!taskPin) {
        taskPin = shared_from_this();
        if (!kisEnqueuePageStoreReclamation(task.get())) {
            lock.unlock();
            finished();
        }
    }
}

void KisImageEpochTreeStorage::drain() noexcept
{
    quint64 dropped = 0;
    while (dropped < 128) {
        KisImageEpochPageRoot *node = nullptr;
        {
            QMutexLocker lock(&mutex);
            node = first;
            if (!node) break;
            auto *tail = node;
            quint64 count = 1;
            while (count < 128 - dropped && tail->nextReleased) {
                tail = tail->nextReleased;
                ++count;
            }
            first = tail->nextReleased;
            if (!first) last = nullptr;
            tail->nextReleased = nullptr;
        }
        // Child final references splice their original nodes into this same
        // owner's list. Nothing recurses, allocates, or returns a live charge
        // before the actual node storage has been freed.
        while (node) {
            auto *next = node->nextReleased;
            std::destroy_at(node);
            nodes.deallocate(node, 1);
            ++dropped;
            node = next;
        }
    }
    auto &destructions = kisOnPageStoreReclamationThread()
        ? backgroundNodeDestructions : foregroundNodeDestructions;
    destructions.fetch_add(dropped, std::memory_order_relaxed);
    treeReleasePasses.fetch_add(1, std::memory_order_relaxed);
    auto maximum = maximumTreeReleasePass.load(std::memory_order_relaxed);
    while (maximum < dropped
           && !maximumTreeReleasePass.compare_exchange_weak(maximum, dropped, std::memory_order_relaxed)) {}
}

void KisImageEpochTreeStorage::finished() noexcept
{
    // The executor cached finished/context/reusable before invocation and
    // will not touch this task again. Last owner destruction is gate-free.
    std::shared_ptr<KisImageEpochTreeStorage> released;
    for (;;) {
        QMutexLocker lock(&mutex);
        if (!first) {
            released = std::move(taskPin);
            return;
        }
        if (kisEnqueuePageStoreReclamation(task.get())) return;
        // With the worker stopped, final destruction still uses the original
        // iterative release list. Child frees append to the same pinned owner.
        lock.unlock();
        drain();
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

KisPageSnapshotArray<KisPageVersion> KisImageEpochRootSnapshot::manifest() const
{
    KisPageSnapshotArray<KisPageVersion> result;
    result.reserve(pageRootCount(m_pageRoot));
    appendPageRoot(m_pageRoot, [&](const auto &version) { result.append(version); });
    return result;
}

bool KisImageEpochRootSnapshot::appendRestoreTargets(
    const KisImageEpochRootSnapshot &current, EpochArray<KisPageVersion> *output) const
{
    if (!isValid() || !current.isValid() || !output) return false;
    // Each cursor is an ordered frontier of whole subtrees and single values.
    // Expanding the taller frontier first preserves shared-subtree skips even
    // when an insertion/removal rotated one root. No manifest or second delta
    // is allocated. The AVL height is below twice the bit width of its count.
    struct Cursor {
        struct Frame { const KisImageEpochPageRoot *node; bool value; };
        std::array<Frame, 2 * std::numeric_limits<qsizetype>::digits> frames;
        size_t count = 0;
        explicit Cursor(const PageRoot &root) { push(root.get()); }
        void push(const KisImageEpochPageRoot *node) {
            if (!node) return;
            Q_ASSERT(count < frames.size());
            frames[count++] = {node, false};
        }
        bool empty() const { return count == 0; }
        const Frame &front() const { return frames[count - 1]; }
        int height() const { return empty() || front().value ? 0 : front().node->height; }
        void expand() {
            auto &item = frames[count - 1];
            item.value = true;
            push(item.node->left.get());
        }
        void consume() {
            const auto *node = front().node;
            --count;
            push(node->right.get());
        }
    };
    Cursor from(current.m_pageRoot), to(m_pageRoot);
    while (!from.empty() || !to.empty()) {
        if (from.height() && to.height() && from.front().node == to.front().node) {
            --from.count;
            --to.count;
            continue;
        }
        if (from.height() || to.height()) {
            if (from.height() >= to.height()) from.expand();
            else to.expand();
            continue;
        }
        const auto *before = from.empty() ? nullptr : &from.front().node->value->version;
        const auto *after = to.empty() ? nullptr : &to.front().node->value->version;
        if (before && (!after || pageKeyLess(before->key, after->key))) {
            KisPageVersion target;
            if (!resolve(before->key, &target)) return false;
            output->push_back(target);
            from.consume();
        } else if (after && (!before || pageKeyLess(after->key, before->key))) {
            output->push_back(*after);
            to.consume();
        } else {
            if (!(*before == *after)) output->push_back(*after);
            from.consume();
            to.consume();
        }
    }
    return true;
}

bool KisImageEpochRootSnapshot::forEachPage(
    KisSurfaceId surface, const std::function<bool(const KisPageVersion &)> &visit) const
{
    bool accepted = isValid();
    appendPageRoot(m_pageRoot, [&](const KisPageVersion &version) {
        if (accepted && version.key.surface == surface) accepted = visit(version);
    });
    return accepted;
}

QVector<KisLogicalPageId> KisImageEpochRootSnapshot::allocatedPages(
    KisSurfaceId surface, KisPageKeyStorage &additions, KisPageKeyStorage &removals) const
{
    if (!isValid()) return {};
    // Inputs are the selected publication owner's unique, disjoint keys for
    // this surface. Only D keys need sorting; visit the ordered root directly
    // into the caller's full output, without exporting N versions or keys.
    std::sort(additions.begin(), additions.end(), pageKeyLess);
    std::sort(removals.begin(), removals.end(), pageKeyLess);
    QVector<KisLogicalPageId> pages;
    pages.reserve(pageCount() + qsizetype(additions.size()));
    auto added = additions.cbegin(), removed = removals.cbegin();
    appendPageRoot(m_pageRoot, [&](const KisPageVersion &version) {
        if (!(version.key.surface == surface)) return;
        while (added != additions.cend() && pageKeyLess(*added, version.key)) pages.append((added++)->page);
        while (removed != removals.cend() && pageKeyLess(*removed, version.key)) ++removed;
        if (removed != removals.cend() && *removed == version.key) return;
        pages.append(version.key.page);
        if (added != additions.cend() && *added == version.key) ++added;
    });
    while (added != additions.cend()) pages.append((added++)->page);
    return pages;
}

qsizetype KisImageEpochRootSnapshot::pageCount() const
{
    return pageRootCount(m_pageRoot);
}

qint32 KisImageEpochRootSnapshot::pageTreeHeight() const
{
    return pageRootHeight(m_pageRoot);
}

KisPageViewValue *KisImageEpochRootSnapshot::selectedValue(const KisPageKey &key) const
{
    return m_epoch.isValid() && m_commitSequence && key.isValid() ? resolvePageValue(m_pageRoot, key) : nullptr;
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
    return snapshotWithChanges(nullptr, 0, nullptr, 0, nullptr, 0);
}

KisImageEpochSnapshot KisImageEpochRootSnapshot::snapshotWithChanges(
    KisPageViewValue *const *values, size_t versionCount,
    const KisPageKey *removals, size_t removalCount,
    const KisSurfaceEpochState *changedSurfaces, size_t changedSurfaceCount) const
{
    KisImageEpochSnapshot result;
    result.epoch = m_epoch;
    result.graphRevision = m_graphRevision;
    result.defaultPixelRevision = m_defaultPixelRevision;
    result.extentRevision = m_extentRevision;
    result.propertyRevision = m_propertyRevision;
    if (!versionCount && !removalCount) {
        result.manifest = manifest();
    } else {
        // Count and emit from the same immutable inputs. This pays two linear
        // tree walks instead of an N+D capacity bound, repeated searches, or
        // another N-entry directory. Each version is copied only on emission.
        const auto merge = [&](const auto &append) {
            size_t added = 0, removed = 0;
            appendPageRoot(m_pageRoot, [&](const KisPageVersion &base) {
                while (added < versionCount && pageKeyLess(values[added]->version.key, base.key)) append(values[added++]->version);
                while (removed < removalCount && pageKeyLess(removals[removed], base.key)) ++removed;
                if (added < versionCount && values[added]->version.key == base.key) append(values[added++]->version);
                else if (removed == removalCount || !(removals[removed] == base.key)) append(base);
            });
            while (added < versionCount) append(values[added++]->version);
        };
        qsizetype count = 0;
        merge([&](const KisPageVersion &) {
            if (count == std::numeric_limits<qsizetype>::max()) throw std::bad_alloc();
            ++count;
        });
        result.manifest.reserve(count);
        if (count) merge([&](const KisPageVersion &version) { result.manifest.append(version); });
    }
    result.surfaces = surfaces();
    size_t changed = 0;
    if (changedSurfaceCount) for (auto &surface : result.surfaces) {
        if (changed == changedSurfaceCount || !(changedSurfaces[changed].surface == surface.surface)) continue;
        surface = changedSurfaces[changed++];
        result.defaultPixelRevision = qMax(result.defaultPixelRevision, surface.defaultPixelRevision);
        result.extentRevision = qMax(result.extentRevision, surface.extentRevision);
    }
    Q_ASSERT(changed == changedSurfaceCount); // Staging only replaces existing surfaces.
    return result;
}

KisImageEpochRootSnapshot::ResolutionKind
KisImageEpochRootSnapshot::resolveVersion(const KisPageKey &key, KisPageVersion *version, KisPageViewValue **selected) const
{
    if (selected) *selected = nullptr;
    // Roots are fully validated once, before they are installed in the model.
    // Revalidating the complete immutable manifest for every single-page read
    // turns an iterator walk into O(page_count^2).
    if (!m_epoch.isValid() || m_commitSequence == 0 || !key.isValid()) {
        if (version)
            *version = {};
        return ResolutionKind::Missing;
    }
    if (auto *value = resolvePageValue(m_pageRoot, key)) {
        if (version) *version = value->version;
        if (selected) *selected = value;
        return ResolutionKind::Explicit;
    }

    const auto *surface = m_surfaces ? findSurface(*m_surfaces, key.surface) : nullptr;
    if (!surface) {
        if (version)
            *version = {};
        return ResolutionKind::Missing;
    }
    if (version) {
        *version = {key, KisPageGeneration{1}, surface->defaultPixelRevision};
    }
    return ResolutionKind::Default;
}

bool KisImageEpochRootSnapshot::resolve(const KisPageKey &key, KisPageVersion *version) const
{
    return resolveVersion(key, version) != ResolutionKind::Missing;
}

bool KisImageEpochRootSnapshot::surfaceState(KisSurfaceId surface, KisSurfaceEpochState *state) const
{
    return m_epoch.isValid() && m_commitSequence != 0 && surface.isValid()
        && m_surfaces && resolveSurface(*m_surfaces, surface, state);
}

KisPageSnapshotArray<KisSurfaceEpochState> KisImageEpochRootSnapshot::surfaces() const
{
    return m_surfaces ? KisPageSnapshotArray<KisSurfaceEpochState>(m_surfaces->begin(), m_surfaces->end())
                      : KisPageSnapshotArray<KisSurfaceEpochState>{};
}

size_t KisImageEpochRootSnapshot::surfaceCount() const
{
    return m_surfaces ? m_surfaces->size() : 0;
}

const KisSurfaceEpochState &KisImageEpochRootSnapshot::surfaceAt(size_t index) const
{
    return m_surfaces->at(index);
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

static const KisPageVersion &epochPreparedVersion(KisPageViewValue *value) { return value->version; }
static const KisPageVersion &epochPreparedVersion(const KisPreparedPageProof &proof) { return proof.authority.version; }
static const KisPageVersion &epochPreparedVersion(const KisPagePublicationChange::Replacement &entry) { return entry.version(); }
static const KisPageVersion &epochPreparedVersion(const KisPagePublicationDelta::SelectionMap::value_type &entry)
{
    return entry.second.version();
}

static EpochPageInput epochPageInput(KisPageViewValue *value) { return {&value->version, value}; }
static EpochPageInput epochPageInput(const KisPreparedPageProof &proof) { return {&proof.authority.version}; }
static EpochPageInput epochPageInput(const KisPagePublicationChange::Replacement &entry)
{ return {&entry.version(), entry.originalSelection ? entry.originalSelection->value : nullptr,
          entry.isNativeValue() ? entry.metadataSlot() : KisVersionSlotId{}, entry.borrowed ? entry.native : nullptr}; }
static EpochPageInput epochPageInput(const KisPagePublicationDelta::SelectionMap::value_type &entry)
{ return {&entry.second.version(), entry.second.value}; }

struct EpochInputProjection {
    template<class Entry> EpochPageInput operator()(const Entry &entry) const { return epochPageInput(entry); }
};
template<class Inputs> struct EpochInputRange {
    const Inputs &inputs;
    auto cbegin() const { return boost::make_transform_iterator(inputs.begin(), EpochInputProjection{}); }
    auto cend() const { return boost::make_transform_iterator(inputs.end(), EpochInputProjection{}); }
};

// Ordered original input and owned immutable deltas share validation and
// root construction. The projection borrows each original version by reference.
struct EpochVersionProjection {
    template<class Entry> const KisPageVersion &operator()(const Entry &entry) const
    { return epochPreparedVersion(entry); }
};
template<class Proofs> struct EpochVersionRange {
    const Proofs &proofs;
    auto cbegin() const { return boost::make_transform_iterator(proofs.begin(), EpochVersionProjection{}); }
    auto cend() const { return boost::make_transform_iterator(proofs.end(), EpochVersionProjection{}); }
    auto begin() const { return cbegin(); }
    auto end() const { return cend(); }
    size_t size() const { return proofs.size(); }
    bool empty() const { return !size(); }
};
template<class T> EpochVersionRange(const T &) -> EpochVersionRange<T>;
template<class T> EpochInputRange(const T &) -> EpochInputRange<T>;

template<class Versions, class Surfaces, class Removals>
bool KisImageEpochReferenceModel::validateEpochDelta(const KisImageEpochRootSnapshot &base, const Versions &versions,
    const Surfaces &surfaces, const Removals &removals, size_t *insertions, QString *error)
{
    *insertions = 0;
    const KisPageVersion *previous = nullptr;
    for (const auto &after : versions) {
        KisPageVersion before;
        const auto kind = base.resolveVersion(after.key, &before);
        if (!after.isValid() || (previous && !pageKeyLess(previous->key, after.key))
            || (kind != KisImageEpochRootSnapshot::ResolutionKind::Missing
                && after.generation.value <= before.generation.value)) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared generation does not advance its base or repeats a key"));
            return false;
        }
        *insertions += kind != KisImageEpochRootSnapshot::ResolutionKind::Explicit;
        previous = &after;
    }
    for (const auto &change : surfaces) {
        KisSurfaceEpochState state;
        if (!base.surfaceState(change.before.surface, &state) || !(state == change.before)) {
            KisPageStoreDetail::setError(error, QStringLiteral("prepared surface metadata does not match the transaction base"));
            return false;
        }
    }
    auto written = versions.cbegin();
    const KisPageKey *previousRemoval = nullptr;
    for (const auto &key : removals) {
        while (written != versions.cend() && pageKeyLess(written->key, key)) ++written;
        if ((previousRemoval && !pageKeyLess(*previousRemoval, key)) || !base.containsPage(key)
            || (written != versions.cend() && written->key == key)) {
            KisPageStoreDetail::setError(error, QStringLiteral("removed page is absent, repeated, or also written"));
            return false;
        }
        previousRemoval = &key;
    }
    return true;
}

struct EpochTransactionChanges;
void intrusive_ptr_add_ref(const EpochTransactionChanges *) noexcept;
void intrusive_ptr_release(const EpochTransactionChanges *) noexcept;

struct EpochTransactionChanges {
    // Bound the fused region instead of pushing a formerly separate large
    // array over a native size-class/VM boundary just to save a service call.
    // Every extent still has this one value's lifetime and original payer.
    static constexpr size_t MaximumPackedBytes = 32 * 1024;
    EpochTransactionChanges(boost::intrusive_ptr<KisMutationStorageOwner> owner,
                            std::shared_ptr<KisPageViewValueStorage> values) noexcept
        : storage(std::move(owner)), values(std::move(values)) {}
    ~EpochTransactionChanges()
    {
        for (auto *value : changes) values->release(value);
        if (!separateArrays) return;
        changes.clear(); surfaceChanges.clear(); removedPages.clear();
        if ((separateArrays & 1) && changes.data())
            storage->deallocate(changes.data(), 0, alignof(KisPageViewValue *));
        if ((separateArrays & 2) && surfaceChanges.data())
            storage->deallocate(surfaceChanges.data(), 0, alignof(KisSurfaceEpochChange));
        if ((separateArrays & 4) && removedPages.data())
            storage->deallocate(removedPages.data(), 0, alignof(KisPageKey));
    }
    static boost::intrusive_ptr<EpochTransactionChanges> create(KisBackingBudgetController *budget,
        size_t versions, size_t surfaces, size_t removals, std::shared_ptr<KisPageViewValueStorage> values)
    {
        static_assert(alignof(EpochTransactionChanges) >= alignof(KisPageViewValue *)
            && alignof(EpochTransactionChanges) >= alignof(KisSurfaceEpochChange)
            && alignof(EpochTransactionChanges) >= alignof(KisPageKey));
        size_t bytes = sizeof(EpochTransactionChanges);
        quint8 separate = 0;
        const auto append = [&](auto *type, size_t count, quint8 bit) {
            using Value = std::remove_pointer_t<decltype(type)>;
            size_t next = bytes;
            const auto offset = KisMutationWorkspace::append<Value>(next, count);
            if (count && next > MaximumPackedBytes) separate |= bit;
            else bytes = next;
            return offset;
        };
        const auto v = append(static_cast<KisPageViewValue * *>(nullptr), versions, 1);
        const auto s = append(static_cast<KisSurfaceEpochChange *>(nullptr), surfaces, 2);
        const auto r = append(static_cast<KisPageKey *>(nullptr), removals, 4);
        boost::intrusive_ptr<KisMutationStorageOwner> owner(kisMutationStorageOwner(budget));
        auto *raw = static_cast<char *>(owner->allocate(bytes, alignof(EpochTransactionChanges)));
        boost::intrusive_ptr<EpochTransactionChanges> value(
            new (raw) EpochTransactionChanges(std::move(owner), std::move(values)), false);
        value->separateArrays = separate;
        value->changes.bind(separate & 1 ? static_cast<KisPageViewValue * *>(value->storage->allocate(
            versions * sizeof(KisPageViewValue *), alignof(KisPageViewValue *))) : reinterpret_cast<KisPageViewValue * *>(raw + v), versions);
        value->surfaceChanges.bind(separate & 2 ? static_cast<KisSurfaceEpochChange *>(value->storage->allocate(
            surfaces * sizeof(KisSurfaceEpochChange), alignof(KisSurfaceEpochChange))) : reinterpret_cast<KisSurfaceEpochChange *>(raw + s), surfaces);
        value->removedPages.bind(separate & 4 ? static_cast<KisPageKey *>(value->storage->allocate(
            removals * sizeof(KisPageKey), alignof(KisPageKey))) : reinterpret_cast<KisPageKey *>(raw + r), removals);
        return value;
    }
    mutable std::atomic<size_t> references{1};
    boost::intrusive_ptr<KisMutationStorageOwner> storage;
    std::shared_ptr<KisPageViewValueStorage> values;
    KisMutationWorkspace::Array<KisPageViewValue *> changes;
    KisMutationWorkspace::Array<KisSurfaceEpochChange> surfaceChanges;
    KisMutationWorkspace::Array<KisPageKey> removedPages;
    // Unique explicit additions relative to this immutable transaction base.
    // Rebase derives current membership from its original conflict lookups;
    // a virtual and explicit default can have the same semantic version.
    size_t pageInsertions = 0;
    quint8 separateArrays = 0;
    bool empty() const { return changes.empty() && surfaceChanges.empty() && removedPages.empty(); }

};

void intrusive_ptr_add_ref(const EpochTransactionChanges *value) noexcept
{ value->references.fetch_add(1, std::memory_order_relaxed); }

void intrusive_ptr_release(const EpochTransactionChanges *value) noexcept
{
    if (value->references.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    // Keep the original payer through array destruction and actual physical
    // free, even when the controller/Core facade has already detached.
    auto storage = value->storage;
    auto *released = const_cast<EpochTransactionChanges *>(value);
    std::destroy_at(released);
    storage->deallocate(released, 0, alignof(EpochTransactionChanges));
}

struct EpochTransactionRecord {
    KisPageTransaction transaction;
    KisPageTransactionState state = KisPageTransactionState::Invalid;
    boost::intrusive_ptr<const EpochTransactionChanges> delta;
    quint64 revision = 0;
    quint64 nextFinished = 0;
    bool isActive() const
    {
        return state == KisPageTransactionState::Open || state == KisPageTransactionState::Prepared;
    }
};

struct EpochRootRecord {
    KisImageEpochRootSnapshot root;
    qsizetype transactionBases = 0;
    quint64 retainedSnapshots = 0;
    quint64 nextCandidate = 0;
    quint64 previousProtected = 0;
    quint64 nextProtected = 0;
};

class KisImageEpochReferenceModel::Private
{
public:
    Private(KisBackingBudgetController *controller, std::shared_ptr<KisBackingBudgetController> standalone,
            KisMutationStorageAllocator<Private> storage)
        : standaloneBudget(std::move(standalone)), budget(controller),
          roots(std::less<quint64>{}, storage), transactions(std::less<quint64>{}, storage),
          retainedSnapshots(std::less<quint64>{}, storage) {}
    bool operational() const { return current.epoch().isValid(); }

    bool ownsRetention(KisImageEpochSnapshotToken token, KisImageEpochId epoch) const
    {
        const auto found = retainedSnapshots.find(token.value);
        return token.isValid() && epoch.isValid() && found != retainedSnapshots.end() && found->second == epoch;
    }

    const EpochTransactionRecord *activeTransaction(KisPageTransactionId transaction) const
    {
        const auto it = transactions.find(transaction.value);
        return it != transactions.end() && it->second.isActive() ? &it->second : nullptr;
    }

    // Logical admission is independent of deferred physical root destruction.
    // Every caller holds mutex, including final release/finish/publication.
    // Once an old root loses its last protection it cannot be resurrected by
    // an epoch number while its tree happens to remain in the retirement queue.
    bool admitsRoot(quint64 epoch) const
    {
        const auto record = roots.find(epoch);
        return epoch != 0 && record != roots.end()
            && (epoch == current.epoch().value || record->second.retainedSnapshots
                || record->second.transactionBases);
    }

    const KisImageEpochRootSnapshot &protectedRoot(quint64 epoch) const
    {
        // Admission, token ownership and active transactions are checked under
        // this same mutex. Collection cannot remove their published roots.
        const auto it = roots.find(epoch);
        Q_ASSERT(admitsRoot(epoch) && it != roots.end() && it->second.root.isValid());
        return it->second.root;
    }

    ~Private() { detachRootEdges(); }

    void detachRootEdges() noexcept
    {
        // A prepared reservation may outlive the facade. Its memory ownership
        // must not extend semantic admission after that facade disappears.
        while (protectedHead) {
            auto &record = roots.find(protectedHead)->second;
            if (record.root.m_pageRoot) record.root.m_pageRoot->deactivate();
            protectedHead = record.nextProtected;
            record.previousProtected = record.nextProtected = 0;
        }
        protectedTail = 0;
    }

    void linkProtected(quint64 epoch)
    {
        auto record = roots.find(epoch);
        Q_ASSERT(record != roots.end());
        if (record->second.previousProtected || record->second.nextProtected || protectedHead == epoch) return;
        if (record->second.root.m_pageRoot) record->second.root.m_pageRoot->activate();
        record->second.previousProtected = protectedTail;
        if (protectedTail) roots.find(protectedTail)->second.nextProtected = epoch;
        else protectedHead = epoch;
        protectedTail = epoch;
    }

    // The three protection exits consume their original registry position.
    // Only the last exit unlinks reachability and queues physical disposal;
    // a protected root never enters a speculative collection queue.
    bool retireUnprotectedRoot(EpochRegistry<EpochRootRecord>::iterator record)
    {
        Q_ASSERT(record != roots.end());
        const quint64 epoch = record->first;
        auto &root = record->second;
        if (epoch == current.epoch().value || root.retainedSnapshots || root.transactionBases)
            return false;
        const quint64 previous = root.previousProtected;
        const quint64 next = root.nextProtected;
        Q_ASSERT(previous || next || protectedHead == epoch);
        Q_ASSERT(!root.nextCandidate && rootCandidatesTail != epoch);
        if (root.root.m_pageRoot) root.root.m_pageRoot->deactivate();
        if (previous) roots.find(previous)->second.nextProtected = next;
        else protectedHead = next;
        if (next) roots.find(next)->second.previousProtected = previous;
        else protectedTail = previous;
        root.previousProtected = root.nextProtected = 0;
        if (rootCandidatesTail)
            roots.find(rootCandidatesTail)->second.nextCandidate = epoch;
        else
            rootCandidatesHead = epoch;
        rootCandidatesTail = epoch;
        return true;
    }

    void finishTransaction(const KisPageTransaction &transaction)
    {
        const auto base = roots.find(transaction.baseEpoch.value);
        Q_ASSERT(base != roots.end() && base->second.transactionBases > 0);
        if (--base->second.transactionBases == 0)
            retireUnprotectedRoot(base);
        --activeTransactions;
        auto record = transactions.find(transaction.id.value);
        Q_ASSERT(record != transactions.end() && !record->second.isActive()
                 && record->second.nextFinished == 0);
        if (finishedTransactionsTail)
            transactions.find(finishedTransactionsTail)->second.nextFinished = transaction.id.value;
        else
            finishedTransactionsHead = transaction.id.value;
        finishedTransactionsTail = transaction.id.value;
        ++finishedTransactionCount;
    }
    mutable QMutex mutex;
    // Declared before paid storage so the standalone controller exits last.
    std::shared_ptr<KisBackingBudgetController> standaloneBudget;
    KisBackingBudgetController *budget = nullptr;
    std::shared_ptr<KisImageEpochTreeStorage> treeStorage;
    quint64 nextEpoch = 1;
    KisImageEpochRootSnapshot current;
    // Registry values own their retirement links. Queuing needs neither an
    // allocation nor a separate dedup set after metadata installation or final
    // protection release. Prepared ordered nodes have no bucket growth or
    // shared-container detach; counts belong to their original root records.
    EpochRegistry<EpochRootRecord> roots;
    qsizetype reservedRootCount = 0;
    EpochRegistry<EpochTransactionRecord> transactions;
    qsizetype activeTransactions = 0;
    quint64 finishedTransactionsHead = 0;
    quint64 finishedTransactionsTail = 0;
    qsizetype finishedTransactionCount = 0;
    quint64 rootCandidatesHead = 0;
    quint64 rootCandidatesTail = 0;
    EpochRegistry<KisImageEpochId> retainedSnapshots;
    // Called only with this original epoch gate held. Structural root copies
    // grant no pixel admission; every logical consumer is represented here.
    bool canDetachPreviousRoot(const KisPageTransaction &transaction) const
    {
        const auto epoch = current.epoch().value;
        const auto root = roots.find(epoch);
        return transaction.baseEpoch == current.epoch()
            && protectedHead == epoch && protectedTail == epoch
            && root != roots.end() && root->second.transactionBases == 1
            && root->second.retainedSnapshots == 0;
    }

    quint64 protectedHead = 0;
    quint64 protectedTail = 0;

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
    EpochRegistry<EpochRootRecord>::node_type released;
    QMutexLocker locker(&owner->mutex);
    const auto it = owner->roots.find(m_root.epoch().value);
    Q_ASSERT(it != owner->roots.end() && !it->second.root.epoch().isValid()
             && !it->second.nextCandidate && owner->rootCandidatesTail != m_root.epoch().value);
    released = owner->roots.extract(it);
    --owner->reservedRootCount;
}

KisImageEpochReferenceModel::KisImageEpochReferenceModel() = default;

KisImageEpochReferenceModel::~KisImageEpochReferenceModel()
{
    const auto core = std::atomic_load(&m_core);
    if (core) {
        QMutexLocker lock(&core->mutex);
        core->detachRootEdges();
    }
}

void KisImageEpochReferenceModel::attachBackingBudget(KisBackingBudgetController &budget)
{
    QMutexLocker lock(&m_initializationMutex);
    Q_ASSERT(!std::atomic_load(&m_core) && !m_budget);
    m_budget = &budget;
}

bool KisImageEpochReferenceModel::initialize(const KisImageEpochSnapshot &initial, QString *error)
{
    auto prepared = prepareInitialization(initial, error);
    if (!prepared) return false;
    if (!installInitialization(std::move(prepared))) {
        KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is already initialized"));
        return false;
    }
    return true;
}

std::shared_ptr<KisImageEpochReferenceModel::Private>
KisImageEpochReferenceModel::prepareInitialization(const KisImageEpochSnapshot &initial, QString *error, KisPageMetadataCoordinator *metadata) try
{
    if (!initial.isValid() || initial.epoch.value == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("initial image epoch is invalid or exhausted"));
        return {};
    }

    QMutexLocker locker(&m_initializationMutex);
    if (std::atomic_load(&m_core)) {
        KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is already initialized"));
        return {};
    }

    auto *budget = m_budget;
    locker.unlock();
    std::shared_ptr<KisBackingBudgetController> standalone;
    if (!budget) {
        const auto parent = kisAcquirePageStoreBootstrapBudget(error);
        if (!parent) return {};
        standalone = std::allocate_shared<KisBackingBudgetController>(
            KisMutationStorageAllocator<KisBackingBudgetController>{});
        if (!standalone->configureSharedNonPayloadBudget(parent, error)) return {};
        budget = standalone.get();
    }
    const auto storage = KisMutationStorageAllocator<Private>::retained(budget);
    auto d = std::allocate_shared<Private>(storage, budget, std::move(standalone), storage);
    std::shared_ptr<KisPageViewValueStorage> values;
    if (metadata) {
        values = metadata->viewValues();
        if (!values) throw EpochValueUnavailable{};
    } else values = std::allocate_shared<EpochOracleValues>(storage, storage);
    d->treeStorage = std::allocate_shared<KisImageEpochTreeStorage>(storage, storage, std::move(values));
    d->treeStorage->prepare(budget);

    KisImageEpochRootSnapshot root;
    root.m_epoch = initial.epoch;
    root.m_commitSequence = 1;
    root.m_graphRevision = initial.graphRevision;
    root.m_defaultPixelRevision = initial.defaultPixelRevision;
    root.m_extentRevision = initial.extentRevision;
    root.m_propertyRevision = initial.propertyRevision;
    root.m_surfaces = std::allocate_shared<KisImageEpochSurfaces>(storage,
        initial.surfaces.begin(), initial.surfaces.end(), storage);
    const auto less = [](const auto &a, const auto &b) { return pageKeyLess(a.key, b.key); };
    if (std::is_sorted(initial.manifest.begin(), initial.manifest.end(), less)) {
        // Canonical exports already have tree order. Construct owned nodes
        // directly from this input; no second directory or redundant sort.
        root.m_pageRoot = buildPageRoot(initial.manifest, 0, initial.manifest.size(), d->treeStorage);
    } else {
        // Public initialization also accepts unordered external snapshots.
        EpochArray<KisPageVersion> manifest(initial.manifest.begin(), initial.manifest.end(), storage);
        std::sort(manifest.begin(), manifest.end(), less);
        root.m_pageRoot = buildPageRoot(manifest, 0, manifest.size(), d->treeStorage);
    }
    // isValid() checked the complete external input before any admission.
    root.m_validated = true;

    d->roots.emplace(root.epoch().value, EpochRootRecord{root});
    d->current = root;
    d->nextEpoch = root.epoch().value + 1;
    KisPageStoreDetail::setError(error, {});
    return d;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("epoch initialization storage admission failed"));
    return {};
}
catch (const EpochValueUnavailable &) {
    KisPageStoreDetail::setError(error, QStringLiteral("initial final Value identity could not be prepared"));
    return {};
}

bool KisImageEpochReferenceModel::installInitialization(std::shared_ptr<Private> &&prepared)
{
    QMutexLocker lock(&m_initializationMutex);
    if (!prepared || std::atomic_load(&m_core)) return false;
    prepared->linkProtected(prepared->current.epoch().value);
    std::atomic_store(&m_core, std::move(prepared));
    return true;
}

KisPageTransaction KisImageEpochReferenceModel::beginTransaction(KisImageEpochId baseEpoch, QString *error) try
{
    const auto d = std::atomic_load(&m_core);
    if (!d) { KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is not initialized")); return {}; }
    EpochRegistry<EpochTransactionRecord>::node_type prepared;
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
    locker.unlock();
    prepared = prepareEpochNode(d->transactions, transaction.id.value, record);
    locker.relock();
    if (!d->admitsRoot(baseEpoch.value)) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction base epoch lost protection during admission"));
        return {};
    }
    d->transactions.insert(std::move(prepared));
    ++d->roots.find(transaction.baseEpoch.value)->second.transactionBases;
    ++d->activeTransactions;
    KisPageStoreDetail::setError(error, {});
    return transaction;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("epoch transaction record storage admission failed"));
    return {};
}

std::shared_ptr<KisPageViewValueStorage> KisImageEpochReferenceModel::viewValues() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker lock(&d->mutex);
    return d->treeStorage->values;
}

bool KisImageEpochReferenceModel::prepare(const KisPreparedPageSet &preparedPages, QString *error)
{
    if (!preparedPages.isValid()) {
        KisPageStoreDetail::setError(error, QStringLiteral("prepared page set is invalid"));
        return false;
    }
    return prepareImpl(preparedPages.transaction, preparedPages.proofs, preparedPages.surfaceChanges,
                       preparedPages.removedPages, error);
}

template<class Proofs, class Surfaces, class Removals>
bool KisImageEpochReferenceModel::prepareImpl(
    KisPageTransactionId transaction, const Proofs &proofs, const Surfaces &surfaces,
    const Removals &removals, QString *error) try
{
    const auto d = std::atomic_load(&m_core);
    if (!d) { KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is not initialized")); return false; }

    // Working and superseded storage outlive the locker, so actual deallocation
    // and budget observers run outside the epoch gate on every exit.
    boost::intrusive_ptr<const EpochTransactionChanges> previous;
    boost::intrusive_ptr<EpochTransactionChanges> next;
    KisImageEpochRootSnapshot base;
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(transaction.value);
    if (!d->operational() || transactionIt == d->transactions.end()
        || !transactionIt->second.isActive()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction is not open for preparation"));
        return false;
    }
    if (transactionIt->second.revision == std::numeric_limits<quint64>::max()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction revision space is exhausted"));
        return false;
    }

    base = d->protectedRoot(transactionIt->second.transaction.baseEpoch.value);
    const quint64 revision = transactionIt->second.revision;
    previous = transactionIt->second.delta;
    locker.unlock();
    const size_t previousChanges = previous ? previous->changes.size() : 0;
    const auto capacity = [](size_t before, size_t added) {
        const size_t maximum = size_t(std::numeric_limits<qint64>::max());
        if (before > maximum || added > maximum - before) throw std::bad_alloc();
        return before + added;
    };
    next = EpochTransactionChanges::create(d->budget, capacity(previousChanges, size_t(proofs.size())),
        capacity(previous ? previous->surfaceChanges.size() : 0, size_t(surfaces.size())),
        capacity(previous ? previous->removedPages.size() : 0, size_t(removals.size())), d->treeStorage->values);
    if (previous) {
        next->pageInsertions = previous->pageInsertions;
        for (auto *value : previous->changes) {
            KisPageViewValueStorage::retain(value);
            next->changes.push_back(value);
        }
        next->surfaceChanges.append(previous->surfaceChanges.data(), previous->surfaceChanges.size());
        next->removedPages.append(previous->removedPages.data(), previous->removedPages.size());
    }
    for (const auto &entry : proofs) {
        const KisPageVersion &after = epochPreparedVersion(entry);
        // The original prefix remains sorted while unique new input keys are
        // appended. Paid capacity was admitted before any element changes.
        const auto end = next->changes.begin() + previousChanges;
        const auto existing = std::lower_bound(next->changes.begin(), end, after.key,
            [](auto *value, const auto &key) { return pageKeyLess(value->version.key, key); });
        if (existing != end && (*existing)->version.key == after.key) {
            if (after.generation.value <= (*existing)->version.generation.value) {
                KisPageStoreDetail::setError(error, QStringLiteral("prepared generation does not advance the transaction chain"));
                return false;
            }
            auto *value = epochPageInput(entry).pin(*next->values);
            next->values->release(*existing);
            *existing = value;
        } else {
            next->changes.push_back(epochPageInput(entry).pin(*next->values));
        }
    }

    std::sort(next->changes.begin(), next->changes.end(), [](auto *lhs, auto *rhs) {
        return pageKeyLess(lhs->version.key, rhs->version.key);
    });
    for (const KisSurfaceEpochChange &change : surfaces) {
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
    for (const KisPageKey &key : removals) {
        next->removedPages.push_back(key);
    }
    std::sort(next->removedPages.begin(), next->removedPages.end(), pageKeyLess);
    const auto uniqueEnd = std::unique(next->removedPages.begin(), next->removedPages.end());
    next->removedPages.resize(size_t(uniqueEnd - next->removedPages.begin()));
    if (!validateEpochDelta(base, EpochVersionRange{next->changes}, next->surfaceChanges, next->removedPages,
                            &next->pageInsertions, error)) return false;
    locker.relock();
    transactionIt = d->transactions.find(transaction.value);
    if (transactionIt == d->transactions.end() || !transactionIt->second.isActive() || transactionIt->second.revision != revision) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction changed during write set preparation"));
        return false;
    }
    // Keep the replaced immutable value alive until after the gate unlock.
    previous = std::move(transactionIt->second.delta);
    transactionIt->second.delta = std::move(next);
    transactionIt->second.state = KisPageTransactionState::Prepared;
    ++transactionIt->second.revision;
    KisPageStoreDetail::setError(error, {});
    return true;
}
catch (const std::bad_alloc &) {
    KisPageStoreDetail::setError(error, QStringLiteral("epoch transaction storage admission failed"));
    return false;
}
catch (const EpochValueUnavailable &) {
    KisPageStoreDetail::setError(error, QStringLiteral("prepared final Value is unavailable"));
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
KisImageEpochReferenceModel::prepareCommit(const KisPageTransaction &transaction, KisImageEpochCommitResult *failure,
                                          bool retainAfter, KisSurfaceId derivedExtentSurface)
{
    return prepareCommitImpl<std::nullptr_t>(transaction, nullptr, failure, retainAfter, derivedExtentSurface);
}

KisImageEpochReferenceModel::PreparedCommit
KisImageEpochReferenceModel::prepareCommit(const KisPageTransaction &transaction,
    const KisPageOperationPublication &publication, KisImageEpochCommitResult *failure,
    bool retainAfter, KisSurfaceId derivedExtentSurface)
{
    return prepareCommitImpl(transaction, &publication, failure, retainAfter, derivedExtentSurface);
}

KisImageEpochReferenceModel::PreparedCommit
KisImageEpochReferenceModel::prepareCommit(const KisPageTransaction &transaction,
    const KisPagePublicationDelta &publication, KisImageEpochCommitResult *failure,
    bool retainAfter, KisSurfaceId derivedExtentSurface)
{
    return prepareCommitImpl(transaction, &publication, failure, retainAfter, derivedExtentSurface);
}

template<class Publication>
KisImageEpochReferenceModel::PreparedCommit
KisImageEpochReferenceModel::prepareCommitImpl(const KisPageTransaction &transaction, const Publication *publication,
    KisImageEpochCommitResult *failure, bool retainAfter, KisSurfaceId derivedExtentSurface) try
{
    constexpr bool borrowed = !std::is_same_v<Publication, std::nullptr_t>;
    KisImageEpochCommitResult localFailure;
    KisImageEpochCommitResult &result = failure ? *failure : localFailure;
    result = {};
    const auto d = std::atomic_load(&m_core);
    if (!d) { result.error = QStringLiteral("image epoch model is not initialized"); return {}; }
    boost::intrusive_ptr<const EpochTransactionChanges> prepared;
    EpochRegistry<EpochRootRecord>::node_type preparedNode;
    KisImageEpochRootSnapshot base, current, root;
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(transaction.id.value);
    if (!d->operational() || !transaction.isValid() || transactionIt == d->transactions.end()
        || !(transactionIt->second.transaction == transaction) || !transactionIt->second.isActive()) {
        result.error = QStringLiteral("transaction is not open for commit"); return {};
    }
    const auto expectedState = transactionIt->second.state;
    if constexpr (std::is_same_v<Publication, KisPagePublicationDelta>) {
        if (!publication || !(publication->transaction == transaction.id) || publication->values != d->treeStorage->values) {
            result.error = QStringLiteral("publication Values belong to another storage owner"); return {};
        }
    }
    if constexpr (borrowed) {
        if (!publication || transactionIt->second.delta
            || !(publication->proofs.size() || publication->surfaceChanges.size() || publication->removals.size())) {
            result.error = QStringLiteral("borrowed commit must retain its original complete input"); return {};
        }
    } else if (expectedState != KisPageTransactionState::Prepared
        || !transactionIt->second.delta || transactionIt->second.delta->empty()) {
        result.error = QStringLiteral("transaction is not prepared for commit"); return {};
    }

    if (d->nextEpoch == 0 || d->nextEpoch == std::numeric_limits<quint64>::max()
        || d->current.commitSequence() == std::numeric_limits<quint64>::max()) {
        result.error = QStringLiteral("image epoch identity space is exhausted");
        return {};
    }
    const KisImageEpochId epoch{d->nextEpoch++};
    const quint64 revision = transactionIt->second.revision;
    if constexpr (!borrowed) prepared = transactionIt->second.delta;
    current = d->current;
    const bool rebased = !(transaction.baseEpoch == current.epoch());
    if (borrowed || rebased) base = d->protectedRoot(transaction.baseEpoch.value);
    locker.unlock();
    const auto &inputs = [&]() -> decltype(auto) {
        if constexpr (borrowed) return (publication->proofs);
        else return (prepared->changes);
    }();
    const auto changes = EpochVersionRange{inputs};
    const auto treeChanges = EpochInputRange{inputs};
    const auto &surfaceChanges = [&]() -> decltype(auto) {
        if constexpr (borrowed) return (publication->surfaceChanges);
        else return (prepared->surfaceChanges);
    }();
    const auto &removedPages = [&]() -> decltype(auto) {
        if constexpr (borrowed) return (publication->removals);
        else return (prepared->removedPages);
    }();
    size_t pageInsertions;
    if constexpr (borrowed) {
        if (!validateEpochDelta(base, changes, surfaceChanges, removedPages, &pageInsertions, &result.error)) return {};
    } else pageInsertions = prepared->pageInsertions;
    if (rebased) {
        pageInsertions = 0;
        for (const KisPageVersion &change : changes) {
            KisPageVersion baseVersion;
            KisPageVersion currentVersion;
            const auto baseKind = base.resolveVersion(change.key, &baseVersion);
            const auto currentKind = current.resolveVersion(change.key, &currentVersion);
            const bool baseContains = baseKind != KisImageEpochRootSnapshot::ResolutionKind::Missing;
            const bool currentContains = currentKind != KisImageEpochRootSnapshot::ResolutionKind::Missing;
            if (baseContains != currentContains || (baseContains && !(baseVersion == currentVersion))) {
                result.status = KisImageEpochCommitStatus::Conflict;
                result.error = QStringLiteral("modified page changed after the transaction base epoch");
                return {};
            }
            // An explicit default sentinel can be removed without changing
            // its semantic version. Reuse the same conflict lookup's actual
            // membership; do not strengthen the original conflict policy.
            if (currentKind != KisImageEpochRootSnapshot::ResolutionKind::Explicit) ++pageInsertions;
        }
        for (const KisSurfaceEpochChange &change : surfaceChanges) {
            KisSurfaceEpochState currentState;
            const bool found = current.surfaceState(change.before.surface, &currentState);
            if (found && change.before.surface == derivedExtentSurface) {
                // Derived bounds are an output of the combined page tree. They
                // cannot make otherwise disjoint page transactions conflict.
                currentState.contentExtent = change.before.contentExtent;
                currentState.extentRevision = change.before.extentRevision;
            }
            if (!found || !(currentState == change.before)) {
                result.status = KisImageEpochCommitStatus::Conflict;
                result.error = QStringLiteral("surface metadata changed after the transaction base epoch");
                return {};
            }
        }
        for (const KisPageKey &key : removedPages) {
            KisPageVersion baseVersion;
            KisPageVersion currentVersion;
            if (!base.containsPage(key, &baseVersion) || !current.containsPage(key, &currentVersion)
                || !(baseVersion == currentVersion)) {
                result.status = KisImageEpochCommitStatus::Conflict;
                result.error = QStringLiteral("removed page changed after the transaction base epoch");
                return {};
            }
        }
    }

    root.m_epoch = epoch;
    root.m_previousEpoch = current.epoch();
    root.m_commitSequence = current.commitSequence() + 1;
    root.m_graphRevision = current.graphRevision();
    root.m_defaultPixelRevision = current.defaultPixelRevision();
    root.m_extentRevision = current.extentRevision();
    root.m_propertyRevision = current.propertyRevision();
    root.m_pageRoot = current.m_pageRoot;
    root.m_surfaces = current.m_surfaces;
    const qsizetype pageDeltaSize = qsizetype(changes.size() + removedPages.size());
    const qsizetype currentPageCount = current.pageCount();
    const bool densePageDelta = pageDeltaSize > 0 && pageDeltaSize >= (currentPageCount + 3) / 4;
    if (densePageDelta) {
        // Input validation already counted unique additions against base;
        // rebase obtained current membership from the same conflict lookups.
        // Build the final balanced shape directly from the ordered merge,
        // without a second owned directory or a counting walk of the tree.
        const size_t currentCount = size_t(currentPageCount);
        if (pageInsertions > size_t(std::numeric_limits<qsizetype>::max()) - currentCount
            || removedPages.size() > currentCount + pageInsertions) {
            result.error = QStringLiteral("prepared page count exceeds the supported range");
            return {};
        }
        const size_t finalCount = currentCount + pageInsertions - removedPages.size();
        if (finalCount) {
            PageDeltaCursor input(current.m_pageRoot, treeChanges.cbegin(), treeChanges.cend(), removedPages.begin(), removedPages.end());
            root.m_pageRoot = buildMergedPageRoot(input, finalCount, d->treeStorage);
            Q_ASSERT(!input.next().identity);
        } else {
            root.m_pageRoot.reset();
        }
    } else {
        // The accepted arrays are sorted and disjoint. Copy each affected
        // shared path once, including mixed insertion/removal/replacement;
        // untouched subtrees retain their original immutable storage.
        root.m_pageRoot = applyPageRootDelta(root.m_pageRoot,
            treeChanges.cbegin(), treeChanges.cend(),
            removedPages.begin(), removedPages.end(), d->treeStorage);
    }
    std::shared_ptr<KisImageEpochSurfaces> surfaces;
    const auto prepareSurfaces = [&] {
        if (surfaces) return;
        const auto storage = current.m_surfaces->get_allocator();
        surfaces = std::allocate_shared<KisImageEpochSurfaces>(storage,
            current.m_surfaces->begin(), current.m_surfaces->end(), storage);
        root.m_surfaces = surfaces;
    };
    if (surfaceChanges.size() != 0) prepareSurfaces();
    for (const KisSurfaceEpochChange &change : surfaceChanges) {
        const auto surface = std::find_if(surfaces->begin(), surfaces->end(), [&](const auto &value) {
            return value.surface == change.after.surface;
        });
        Q_ASSERT(surface != surfaces->end());
        *surface = change.after;
        root.m_defaultPixelRevision = qMax(root.m_defaultPixelRevision, change.after.defaultPixelRevision);
        root.m_extentRevision = qMax(root.m_extentRevision, surface->extentRevision);
    }
    if (derivedExtentSurface.isValid()) {
        KisSurfaceEpochState state, currentState;
        QRect extent;
        if (!current.surfaceState(derivedExtentSurface, &currentState) ||
            !root.surfaceState(derivedExtentSurface, &state) ||
            !root.contentExtentAfterPages(derivedExtentSurface, state.logicalPageExtent,
                                         nullptr, 0, nullptr, 0, &extent)) {
            result.error = QStringLiteral("derived committed extent exceeds QRect range");
            return {};
        }
        auto derived = state;
        derived.contentExtent = extent;
        derived.extentRevision = qMax(state.extentRevision, currentState.extentRevision);
        if (extent != currentState.contentExtent) {
            if (currentState.extentRevision == std::numeric_limits<quint64>::max()) {
                result.error = QStringLiteral("derived committed extent revision exhausted");
                return {};
            }
            derived.extentRevision = qMax(derived.extentRevision, currentState.extentRevision + 1);
        }
        if (!(derived == state)) {
            prepareSurfaces();
            auto surface = std::find_if(surfaces->begin(), surfaces->end(), [&](const auto &value) {
                return value.surface == derivedExtentSurface;
            });
            Q_ASSERT(surface != surfaces->end());
            *surface = derived;
            root.m_extentRevision = qMax(root.m_extentRevision, derived.extentRevision);
        }
    }
    // The original root and complete input were validated; replacement keeps
    // each existing surface identity, and publication identities were guarded.
    root.m_validated = true;

    surfaces.reset();
    preparedNode = prepareEpochNode(d->roots, root.epoch().value);
    PreparedCommit candidate;
    if (retainAfter) {
        const auto token = KisPageStoreDetail::allocateMonotonicId<KisImageEpochSnapshotToken>(&s_nextSnapshotToken);
        if (!token.isValid()) {
            result.error = QStringLiteral("image epoch snapshot identity space is exhausted");
            return {};
        }
        candidate.m_retainedAfter = describeRetainedRoot(root, token, false);
        candidate.m_retentionNode = prepareEpochNode(d->retainedSnapshots, token.value, root.epoch());
    }
    locker.relock();
    transactionIt = d->transactions.find(transaction.id.value);
    if (!(d->current.epoch() == current.epoch()) || transactionIt == d->transactions.end()
        || transactionIt->second.revision != revision || transactionIt->second.state != expectedState) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or transaction changed during candidate preparation");
        return {};
    }
    // Reserve the paid node before publication, but never expose a candidate
    // through root(), retention, transaction bases, or reachability queries.
    d->roots.insert(std::move(preparedNode));
    ++d->reservedRootCount;
    candidate.m_owner = d;
    candidate.m_root = root;
    candidate.m_transaction = transaction.id;
    candidate.m_revision = revision;
    candidate.m_expectedState = expectedState;
    candidate.m_mayDetachPrevious = d->canDetachPreviousRoot(transaction);
    return candidate;
}
catch (const std::bad_alloc &) {
    if (failure) failure->error = QStringLiteral("epoch root preparation storage admission failed");
    return {};
}
catch (const EpochValueUnavailable &) {
    if (failure) failure->error = QStringLiteral("candidate final Value is unavailable");
    return {};
}

KisImageEpochCommitResult KisImageEpochReferenceModel::installCommit(PreparedCommit &&candidate,
                                                                     void *context,
                                                                     InstallMetadataFunction installMetadata,
                                                                     KisRetainedImageEpochSnapshot *retainedAfter)
{
    // Consume even on rejection; cancellation must run after the mutex unlock.
    PreparedCommit consumed(std::move(candidate));
    KisImageEpochCommitResult result;
    if (retainedAfter) *retainedAfter = {};
    const auto d = std::atomic_load(&m_core);
    if (!consumed.isValid() || consumed.m_owner != d) {
        result.error = QStringLiteral("prepared epoch candidate has a different owner or was consumed");
        return result;
    }
    if (bool(retainedAfter) != consumed.m_retainedAfter.isValid()) {
        result.error = QStringLiteral("prepared epoch retention output does not match its admission");
        return result;
    }
    QMutexLocker locker(&d->mutex);
    const auto &root = consumed.m_root;
    auto transactionIt = d->transactions.find(consumed.m_transaction.value);
    if (!(d->current.epoch() == root.previousEpoch()) || transactionIt == d->transactions.end()
        || transactionIt->second.revision != consumed.m_revision
        || transactionIt->second.state != consumed.m_expectedState) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or transaction changed before candidate installation");
        return result;
    }
    if (consumed.m_requiresDetachedPrevious
        && !d->canDetachPreviousRoot(transactionIt->second.transaction)) {
        result.status = KisImageEpochCommitStatus::Rejected;
        result.error = QStringLiteral("previous root gained a consumer before installation");
        return result;
    }
    if (!installReservedRoot(consumed, context, installMetadata, &result))
        return result;

    if (retainedAfter) {
        Q_ASSERT(!consumed.m_retentionNode.empty());
        const auto inserted = d->retainedSnapshots.insert(std::move(consumed.m_retentionNode));
        Q_ASSERT(inserted.inserted);
        Q_UNUSED(inserted);
        ++d->roots.find(root.epoch().value)->second.retainedSnapshots;
        *retainedAfter = std::move(consumed.m_retainedAfter);
    }

    const KisPageTransaction transaction = transactionIt->second.transaction;
    transactionIt->second.state = KisPageTransactionState::Committed;
    d->finishTransaction(transaction);
    return result;
}

bool KisImageEpochReferenceModel::installReservedRoot(
    PreparedRootReservation &candidate, void *context, InstallMetadataFunction installMetadata,
    KisImageEpochCommitResult *result)
{
    const auto d = std::atomic_load(&m_core);
    auto rootIt = d->roots.find(candidate.m_root.epoch().value);
    Q_ASSERT(rootIt != d->roots.end() && !rootIt->second.root.epoch().isValid());
    if (installMetadata && !installMetadata(context, candidate.m_root.epoch())) {
        result->error = QStringLiteral("prepared metadata publication was rejected");
        return false;
    }
    rootIt->second.root = candidate.m_root;
    const quint64 previous = d->current.epoch().value;
    d->current = candidate.m_root;
    d->linkProtected(d->current.epoch().value);
    d->retireUnprotectedRoot(d->roots.find(previous));
    --d->reservedRootCount;
    candidate.m_owner.reset();
    result->status = KisImageEpochCommitStatus::Committed;
    result->root = candidate.m_root;
    return true;
}

KisImageEpochReferenceModel::PreparedRootReservation
KisImageEpochReferenceModel::prepareRestore(const KisRetainedImageEpochSnapshot &retained,
                                            KisImageEpochCommitResult *failure) try
{
    KisImageEpochCommitResult localFailure;
    KisImageEpochCommitResult &result = failure ? *failure : localFailure;
    result = {};
    const auto d = std::atomic_load(&m_core);
    if (!d) { result.error = QStringLiteral("image epoch model is not initialized"); return {}; }
    KisImageEpochRootSnapshot restored;
    EpochRegistry<EpochRootRecord>::node_type preparedNode;
    QMutexLocker locker(&d->mutex);
    const auto retainedIt = d->retainedSnapshots.find(retained.token.value);
    if (!d->operational() || !retained.isValid() || retainedIt == d->retainedSnapshots.end()
        || !(retainedIt->second == retained.snapshot.epoch)
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
    restored = d->protectedRoot(retained.snapshot.epoch.value);
    restored.m_epoch = KisImageEpochId{d->nextEpoch++};
    restored.m_previousEpoch = d->current.epoch();
    restored.m_commitSequence = d->current.commitSequence() + 1;
    locker.unlock();
    preparedNode = prepareEpochNode(d->roots, restored.epoch().value);
    locker.relock();
    const auto token = d->retainedSnapshots.find(retained.token.value);
    if (token == d->retainedSnapshots.end() || !(token->second == retained.snapshot.epoch)
        || !(d->current.epoch() == restored.previousEpoch()) || d->activeTransactions) {
        result.status = KisImageEpochCommitStatus::Conflict;
        result.error = QStringLiteral("epoch or retention changed during restore preparation");
        return {};
    }
    d->roots.insert(std::move(preparedNode));
    ++d->reservedRootCount;
    PreparedRootReservation candidate;
    candidate.m_owner = d;
    candidate.m_root = restored;
    return candidate;
}
catch (const std::bad_alloc &) {
    if (failure) failure->error = QStringLiteral("epoch restore record storage admission failed");
    return {};
}

KisImageEpochCommitResult KisImageEpochReferenceModel::installRestore(PreparedRootReservation &&candidate,
                                                                      void *context,
                                                                      InstallMetadataFunction installMetadata)
{
    PreparedRootReservation consumed(std::move(candidate));
    KisImageEpochCommitResult result;
    const auto d = std::atomic_load(&m_core);
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
    return result;
}

bool KisImageEpochReferenceModel::abort(const KisPageTransaction &transaction, QString *error)
{
    const auto d = std::atomic_load(&m_core);
    if (!d) { KisPageStoreDetail::setError(error, QStringLiteral("image epoch model is not initialized")); return false; }
    QMutexLocker locker(&d->mutex);
    auto transactionIt = d->transactions.find(transaction.id.value);
    if (!d->operational() || !transaction.isValid() || transactionIt == d->transactions.end()
        || !(transactionIt->second.transaction.baseEpoch == transaction.baseEpoch)
        || !transactionIt->second.isActive()) {
        KisPageStoreDetail::setError(error, QStringLiteral("transaction cannot be aborted"));
        return false;
    }
    transactionIt->second.state = KisPageTransactionState::Aborted;
    d->finishTransaction(transaction);
    KisPageStoreDetail::setError(error, {});
    return true;
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::captureCommittedRoot() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker locker(&d->mutex);
    return d->current;
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::describeRetainedRoot(
    const KisImageEpochRootSnapshot &root, KisImageEpochSnapshotToken token, bool completeManifest)
{
    KisRetainedImageEpochSnapshot retained;
    retained.token = token;
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
    return retained;
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::retainRootLocked(
    const KisImageEpochRootSnapshot &root,
    bool completeManifest, QMutexLocker<QMutex> &locker)
{
    const auto d = std::atomic_load(&m_core);
    const auto retainedRoot = root;
    const auto token = KisPageStoreDetail::allocateMonotonicId<KisImageEpochSnapshotToken>(&s_nextSnapshotToken);
    if (!token.isValid()) return {};
    locker.unlock();
    auto prepared = prepareEpochNode(d->retainedSnapshots, token.value, retainedRoot.epoch());
    auto retained = describeRetainedRoot(retainedRoot, token, completeManifest);
    locker.relock();
    if (!d->admitsRoot(retainedRoot.epoch().value)) {
        locker.unlock();
        return {};
    }
    d->retainedSnapshots.insert(std::move(prepared));
    ++d->roots.find(retainedRoot.epoch().value)->second.retainedSnapshots;
    return retained;
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureRetainedSnapshot() try
{
    return captureCurrentRetainedRootLocked(true);
}
catch (const std::bad_alloc &) { return {}; }

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureRetainedRoot() try
{
    return captureCurrentRetainedRootLocked(false);
}
catch (const std::bad_alloc &) { return {}; }

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::captureCurrentRetainedRootLocked(
    bool completeManifest)
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return {};
    return retainRootLocked(d->protectedRoot(d->current.epoch().value), completeManifest, locker);
}

KisRetainedImageEpochSnapshot KisImageEpochReferenceModel::retainSnapshot(KisImageEpochId epoch) try
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || !epoch.isValid() || !d->admitsRoot(epoch.value)) {
        return {};
    }
    return retainRootLocked(d->protectedRoot(epoch.value), false, locker);
}
catch (const std::bad_alloc &) { return {}; }

bool KisImageEpochReferenceModel::validateRetainedSnapshot(const KisRetainedImageEpochSnapshot &retained) const
{
    if (!retained.isValid())
        return false;
    const auto d = std::atomic_load(&m_core);
    if (!d) return false;
    QMutexLocker locker(&d->mutex);
    const auto retainedIt = d->retainedSnapshots.find(retained.token.value);
    if (!d->operational() || retainedIt == d->retainedSnapshots.end()
        || !(retainedIt->second == retained.snapshot.epoch)) {
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
    const auto d = std::atomic_load(&m_core);
    if (!d) { KisPageStoreDetail::setError(error, QStringLiteral("image epoch snapshot token is stale")); return false; }
    EpochRegistry<KisImageEpochId>::node_type released;
    QMutexLocker locker(&d->mutex);
    auto snapshotIt = d->retainedSnapshots.find(token.value);
    if (!d->operational() || !token.isValid() || snapshotIt == d->retainedSnapshots.end()) {
        KisPageStoreDetail::setError(error, QStringLiteral("image epoch snapshot token is stale"));
        return false;
    }
    const quint64 epoch = snapshotIt->second.value;
    released = d->retainedSnapshots.extract(snapshotIt);
    const auto root = d->roots.find(epoch);
    Q_ASSERT(root != d->roots.end() && root->second.retainedSnapshots > 0);
    if (--root->second.retainedSnapshots == 0) {
        const bool retired = d->retireUnprotectedRoot(root);
        if (rootBecameUnretained) *rootBecameUnretained = retired;
    }
    KisPageStoreDetail::setError(error, {});
    return true;
}

qsizetype KisImageEpochReferenceModel::retainedSnapshotCount() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return 0;
    QMutexLocker locker(&d->mutex);
    return d->retainedSnapshots.size();
}

qsizetype KisImageEpochReferenceModel::rootCount() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return 0;
    QMutexLocker locker(&d->mutex);
    return d->roots.size() - d->reservedRootCount;
}

qsizetype KisImageEpochReferenceModel::activeTransactionCount() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return 0;
    QMutexLocker locker(&d->mutex);
    return d->activeTransactions;
}

qsizetype KisImageEpochReferenceModel::collectFinishedTransactions(qsizetype budget)
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return 0;
    EpochRegistry<EpochTransactionRecord>::node_type released;
    QMutexLocker locker(&d->mutex);
    const qsizetype limit = budget < 0 ? d->finishedTransactionCount : qMin(budget, d->finishedTransactionCount);
    qsizetype removed = 0;
    while (removed < limit && d->finishedTransactionsHead) {
        const auto record = d->transactions.find(d->finishedTransactionsHead);
        Q_ASSERT(record != d->transactions.end() && !record->second.isActive());
        d->finishedTransactionsHead = record->second.nextFinished;
        released = d->transactions.extract(record);
        --d->finishedTransactionCount;
        if (!d->finishedTransactionsHead) d->finishedTransactionsTail = 0;
        ++removed;
        locker.unlock();
        released = {};
        locker.relock();
    }
    return removed;
}

qsizetype KisImageEpochReferenceModel::collectUnretainedRoots(qsizetype budget)
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return 0;
    EpochRegistry<EpochRootRecord>::node_type released;
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
        d->rootCandidatesHead = record->second.nextCandidate;
        if (!d->rootCandidatesHead)
            d->rootCandidatesTail = 0;
        record->second.nextCandidate = 0;
        // Logical admission ended at the enqueue event and cannot revive.
        // Independent structural snapshots can still retain the tree value.
        Q_ASSERT(!d->admitsRoot(epoch));
        released = d->roots.extract(record);
        ++removed;
        locker.unlock();
        released = {};
        locker.relock();
    }
    return removed;
}

bool KisImageEpochReferenceModel::hasCollectionWork() const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return false;
    QMutexLocker locker(&d->mutex);
    return d->finishedTransactionsHead != 0 || d->rootCandidatesHead != 0;
}

KisPageSnapshotArray<KisPageVersion> KisImageEpochReferenceModel::reachablePageVersions(const KisPageSnapshotArray<KisPageKey> &registeredKeys,
                                                                        quint64 *visitedRoots) const
{
    KisPageSnapshotArray<KisPageVersion> result;
    if (visitedRoots)
        *visitedRoots = 0;
    const auto d = std::atomic_load(&m_core);
    if (!d) return result;
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || registeredKeys.isEmpty())
        return result;
    auto visit = [&](quint64 epoch) {
        const auto root = d->roots.find(epoch);
        Q_ASSERT(root != d->roots.end() && root->second.root.epoch().isValid());
        if (visitedRoots)
            ++*visitedRoots;
        for (const KisPageKey &key : registeredKeys) {
            KisPageVersion version;
            if (root->second.root.resolve(key, &version))
                result.append(version);
        }
    };
    // The original protected list already visits each admitted root once;
    // counted ownership now lives in those same root records.
    for (quint64 epoch = d->protectedHead; epoch; epoch = d->roots.find(epoch)->second.nextProtected)
        visit(epoch);
    const auto less = [](const KisPageVersion &a, const KisPageVersion &b) {
        return std::tie(a.key.surface.value, a.key.page.row, a.key.page.column, a.generation.value, a.defaultPixelRevision)
            < std::tie(b.key.surface.value, b.key.page.row, b.key.page.column, b.generation.value, b.defaultPixelRevision);
    };
    std::sort(result.begin(), result.end(), less);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::root(KisImageEpochId epoch) const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker locker(&d->mutex);
    return d->operational() && d->admitsRoot(epoch.value) ? d->protectedRoot(epoch.value)
                                                        : KisImageEpochRootSnapshot();
}

KisImageEpochRootSnapshot KisImageEpochReferenceModel::retainedRoot(KisImageEpochSnapshotToken token,
                                                                 KisImageEpochId epoch) const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
    QMutexLocker locker(&d->mutex);
    if (!d->operational() || !d->ownsRetention(token, epoch))
        return {};
    return d->protectedRoot(epoch.value);
}

KisPageTransactionSnapshot KisImageEpochReferenceModel::transaction(KisPageTransactionId id) const
{
    KisPageTransactionSnapshot result;
    const auto d = std::atomic_load(&m_core);
    if (!d) return result;
    boost::intrusive_ptr<const EpochTransactionChanges> delta;
    QMutexLocker locker(&d->mutex);
    const auto found = d->transactions.find(id.value);
    if (found == d->transactions.cend()) return {};
    result.transaction = found->second.transaction;
    result.state = found->second.state;
    delta = found->second.delta;
    locker.unlock();
    if (delta) {
        const auto versions = EpochVersionRange{delta->changes};
        result.changes = KisPageSnapshotArray<KisPageVersion>(versions.begin(), versions.end());
        result.surfaceChanges = KisPageSnapshotArray<KisSurfaceEpochChange>(delta->surfaceChanges.begin(), delta->surfaceChanges.end());
        result.removedPages = KisPageSnapshotArray<KisPageKey>(delta->removedPages.begin(), delta->removedPages.end());
    }
    return result;
}

KisPageTransaction KisImageEpochReferenceModel::activeTransaction(KisPageTransactionId id) const
{
    const auto d = std::atomic_load(&m_core);
    if (!d) return {};
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

    const auto d = std::atomic_load(&m_core);
    if (!d) return fail();
    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return fail();

    switch (view.kind) {
    case KisPageReadViewKind::CurrentCommittedEpoch:
        return d->current.resolve(key, version);
    case KisPageReadViewKind::CommittedEpoch:
        if (!d->ownsRetention(view.retention, view.epoch))
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
                [](auto *value, const auto &key) { return pageKeyLess(value->version.key, key); });
            if (change != changes.end() && (*change)->version.key == key) {
                if (version) *version = (*change)->version;
                return true;
            }
        }
        return d->protectedRoot(transaction->transaction.baseEpoch.value).resolve(key, version);
    }
    case KisPageReadViewKind::ExactVersion: {
        if (!d->ownsRetention(view.retention, view.epoch))
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

    const auto d = std::atomic_load(&m_core);
    if (!d) return false;
    QMutexLocker locker(&d->mutex);
    if (!d->operational())
        return false;

    switch (view.kind) {
    case KisPageReadViewKind::CurrentCommittedEpoch:
        return d->current.surfaceState(surface, state);
    case KisPageReadViewKind::CommittedEpoch:
    case KisPageReadViewKind::ExactVersion:
        if (!d->ownsRetention(view.retention, view.epoch)) {
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
