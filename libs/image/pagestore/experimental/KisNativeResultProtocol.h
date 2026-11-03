/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_NATIVE_RESULT_PROTOCOL_H
#define KIS_NATIVE_RESULT_PROTOCOL_H

#include "KisPageBackingBudget_p.h"
#include "KisPageStoreTypes.h"
#include <memory>
#include <functional>

class KisTiledDataManager;
class KisCpuResidentBinding;
class KisCpuWriteBindingReservation;

// Isolated complete-result construction. No PageStore private/publication
// coordinator is an internal consumer of this protocol.
class KisNativeResultProtocol final
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
    class Snapshot {
    public:
        Snapshot() = default;
        explicit operator bool() const { return bool(d); }
        bool read(quint8 *bytes, const QRect &pixels, qsizetype stride, QString *error = nullptr) const;
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
        const KisPageBackingLimits &limits = {});
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
    void retryRetirements();
    bool close(QString *error = nullptr);
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
