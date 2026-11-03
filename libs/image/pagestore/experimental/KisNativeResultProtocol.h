/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_NATIVE_RESULT_PROTOCOL_H
#define KIS_NATIVE_RESULT_PROTOCOL_H

#include "KisPageBackingBudget_p.h"
#include "KisPageStoreTypes.h"
#include "tiles3/kis_tile.h"
#include "tiles3/kis_memento.h"
#include <memory>
#include <functional>

class KisTiledDataManager;
class KisCpuResidentBinding;
class KisCpuWriteBindingReservation;
class KisTileData;
class KisPageStoreIteratorReadScope;
class KisTiledDataManagerIteratorWriteScope;
class KisPixelWriteCursor;

// Isolated complete-result construction. No PageStore private/publication
// coordinator is an internal consumer of this protocol.
class KRITAIMAGE_EXPORT KisNativeResultProtocol final
{
    struct Impl;
    struct View;
    struct Body;
    struct Async;
public:
    struct Counts {
        quint64 fresh = 0, handoff = 0, results = 0, retainedEdges = 0;
        quint64 cancelled = 0, retried = 0, retired = 0, adapterInstalled = 0;
    };
    class IteratorSession {
    public:
        ~IteratorSession();
        bool finish();
        bool isActive() const;
        static void *operator new(size_t);
        static void operator delete(void *) noexcept;
    private:
        std::shared_ptr<Impl> owner;
        Body *body = nullptr;
        friend class KisNativeResultProtocol;
    };
    class ReadAccess {
    public:
        ReadAccess() = default;
        ~ReadAccess();
        ReadAccess(ReadAccess &&) noexcept;
        ReadAccess &operator=(ReadAccess &&) noexcept;
        ReadAccess(const ReadAccess &) = delete;
        ReadAccess &operator=(const ReadAccess &) = delete;
        const quint8 *data() const { return pixels; }
        quint32 rowStride() const { return stride; }
        KisPageVersion version() const { return selected; }
    private:
        std::shared_ptr<Impl> owner;
        std::shared_ptr<KisCpuResidentBinding> binding;
        KisTileData *constant = nullptr;
        const quint8 *pixels = nullptr;
        quint32 stride = 0;
        KisPageVersion selected;
        friend class KisNativeResultProtocol;
    };
    class Snapshot {
    public:
        Snapshot() = default;
        explicit operator bool() const { return bool(d); }
        bool read(quint8 *bytes, const QRect &pixels, qsizetype stride, QString *error = nullptr) const;
        ReadAccess readPage(qint32 column, qint32 row, QString *error = nullptr) const;
        KisPageVersion versionAt(qint32 column, qint32 row) const;
    private:
        std::shared_ptr<View> d;
        friend class KisNativeResultProtocol;
    };
    class Prepared {
    public:
        Prepared() = default;
        ~Prepared();
        Prepared(Prepared &&) noexcept;
        Prepared &operator=(Prepared &&) noexcept;
        Prepared(const Prepared &) = delete;
        Prepared &operator=(const Prepared &) = delete;
        explicit operator bool() const { return body != nullptr; }
    private:
        std::shared_ptr<Impl> owner;
        Body *body = nullptr;
        friend class KisNativeResultProtocol;
    };
    class ProducerCompletion {
    public:
        ProducerCompletion() = default;
        explicit operator bool() const { return bool(d); }
        bool finish(bool success = true);
        KisCompletionTicket ticket() const;
    private:
        std::shared_ptr<Async> d;
        friend class KisNativeResultProtocol;
    };
    struct Faults {
        // Contract witnesses only, never enabled by the ordinary price run.
        int rejectAfterTargets = -1;
        bool rejectAdapter = false;
        bool rejectAccept = false;
        int rejectAcceptAt = -1;
        bool rejectPhysical = false;
        std::function<void()> beforeAccept;
    };

    KisNativeResultProtocol(KisTiledDataManager &adapter,
        const KisPageAllocationDescriptor &descriptor,
        const KisPageBackingLimits &limits);
    KisNativeResultProtocol(KisTiledDataManager &adapter, const KisPageAllocationDescriptor &descriptor);
    ~KisNativeResultProtocol();
    KisNativeResultProtocol(const KisNativeResultProtocol &) = delete;
    KisNativeResultProtocol &operator=(const KisNativeResultProtocol &) = delete;

    Prepared prepareWrite(const quint8 *bytes, const QRect &pixels, qsizetype stride,
                           QString *error = nullptr, const Faults *faults = nullptr);
    // Raw/custom/asynchronous CPU producers own the very same original rows.
    // Accept is closed until every exposed physical pin has been returned.
    Prepared prepareProducer(const QRect &pixels, QString *error = nullptr);
    quint8 *producerPage(Prepared &, qint32 column, qint32 row, quint32 *stride = nullptr);
    bool finishProducer(Prepared &, bool success, QString *error = nullptr);
    ProducerCompletion deferProducer(Prepared &, QString *error = nullptr);
    bool accept(Prepared &, QString *error = nullptr, const Faults *faults = nullptr);
    void cancel(Prepared &) noexcept;
    bool write(const quint8 *bytes, const QRect &pixels, qsizetype stride,
               QString *error = nullptr, const Faults *faults = nullptr);
    Snapshot capture(QString *error = nullptr);
    Snapshot captureRange(const QRect &pixels, QString *error = nullptr);
    Snapshot capturePrepared(const Prepared &, QString *error = nullptr);
    bool alias(const Snapshot &, const QRect &sourcePixels, const QPoint &destination, QString *error = nullptr);
    bool remove(const QRect &pixels, QString *error = nullptr);
    bool setDefaultPixel(const QByteArray &, QString *error = nullptr);
    std::unique_ptr<KisNativeResultProtocol> cloneTo(KisTiledDataManager &adapter, QString *error = nullptr);
    bool restore(const Snapshot &, QString *error = nullptr);
    bool read(const Snapshot &, quint8 *bytes, const QRect &pixels, qsizetype stride,
              QString *error = nullptr) const;
    bool readCurrent(quint8 *bytes, const QRect &pixels, qsizetype stride,
                     QString *error = nullptr) const;
    bool configureLimits(const KisPageBackingLimits &, QString *error = nullptr);
    bool swapIdleForTest(QString *error = nullptr);
    bool close(QString *error = nullptr);
    // Actual DataManager/undo public boundary. These use the same Selection,
    // snapshots and all-D acceptance, never a private/publication conversion.
    static std::unique_ptr<KisNativeResultProtocol> createProduct(KisTiledDataManager &, quint32 bpp, const quint8 *pixel);
    KisMementoSP beginHistory(QString *error = nullptr);
    bool commitHistory(QString *error = nullptr);
    bool abortHistory(const KisMementoSP &, QString *error = nullptr);
    bool restoreHistory(const KisMementoSP &, bool before, QString *error = nullptr);
    bool hasHistory() const;
    void purgeHistory(const KisMementoSP &);
    KisTileSP oldTile(qint32 column, qint32 row, bool *present, QString *error = nullptr) const;
    bool clear(QString *error = nullptr);
    using PageProducer = std::function<bool(qint32, qint32, quint8 *, quint32, const QRect &)>;
    bool mutate(const QRect &, const PageProducer &, bool discardCovered = false,
                const QByteArray &sparseDefault = {}, QString *error = nullptr);
    bool fill(const QRect &, const QByteArray &, QString *error = nullptr);
    bool copyFrom(const Snapshot &, const QRect &, QString *error = nullptr);
    bool trim(const QRect &, QString *error = nullptr);
    Snapshot captureOldData(QString *error = nullptr);
    std::shared_ptr<const KisPageStoreIteratorReadScope> captureIteratorScope(bool writable,
        std::shared_ptr<const KisPageStoreIteratorReadScope> existing = {}, QString *error = nullptr);
    std::unique_ptr<KisTiledDataManagerIteratorWriteScope> beginIterator(QString *error = nullptr);
    KisTileSP writableTile(qint32 column, qint32 row, QString *error = nullptr);
    bool purge(const QRect &, QString *error = nullptr);
    bool beginHistoryMutation(const KisMementoSP &, QString *error = nullptr);
    bool checkpointHistoryMutation(const KisMementoSP &, QString *error = nullptr);
    bool pixelOperation(const QVector<QRect> &, const std::function<bool(KisPixelWriteCursor *)> &,
                        const KisMementoSP &history, QString *error = nullptr);
    Counts counts() const;
    KisPageBackingUsage usage() const;

private:
    std::shared_ptr<Impl> d;
    static KisCpuWriteBindingReservation claimNative(
        const std::shared_ptr<KisCpuResidentBinding> &, quint64 incarnation,
        const KisPageVersion &target);
    static bool validateNative(KisCpuResidentBinding *, const KisReplicaHandle &);
};
#endif
