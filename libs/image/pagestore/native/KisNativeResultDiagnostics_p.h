/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_NATIVE_RESULT_DIAGNOSTICS_P_H
#define KIS_NATIVE_RESULT_DIAGNOSTICS_P_H
#include "KisNativeResultProtocol.h"
namespace KisNativeResultDiagnostics {
struct Counts {
        quint64 fresh = 0, handoff = 0, results = 0, retainedEdges = 0;
        quint64 cancelled = 0, retried = 0, retired = 0, adapterInstalled = 0;
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
struct State {
        quint64 pageSlots = 0, physicalResources = 0, resourceCapacity = 0;
        quint64 activeResults = 0, logicalViews = 0, domainMoves = 0, terminalRegistrations = 0;
        quint64 reservedEdges = 0, rowCapacity = 0;
        quint64 acceptedEpoch = 0, historyEdges = 0;
    };
struct Layout { size_t page, resource, body, row, retentionEdge, view; };

}
// No runtime owner, observer or virtual dispatch is introduced by test access.
class KisNativeResultTestAccess {
public:
    static KRITAIMAGE_EXPORT KisNativeResultProtocol &protocol(KisTiledDataManager &);
    using Faults = KisNativeResultDiagnostics::Faults;
    static auto counts(const KisNativeResultProtocol &p) { return p.counts(); }
    static auto state(const KisNativeResultProtocol &p) { return p.state(); }
    static auto layout() { return KisNativeResultProtocol::layout(); }
    static bool swapIdle(KisNativeResultProtocol &p, QString *why = nullptr) { return p.swapIdleForTest(why); }
    static auto prepareWrite(KisNativeResultProtocol &p, const quint8 *bytes, const QRect &rect,
                             qsizetype stride, QString *why, const Faults *faults) {
        return p.prepareWrite(bytes, rect, stride, why, faults);
    }
    static bool accept(KisNativeResultProtocol &p, KisNativeResultProtocol::Prepared &body,
                       QString *why, const Faults *faults) { return p.accept(body, why, faults); }
};
#endif
