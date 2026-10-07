/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TRANSACTION_DATA_H_
#define KIS_TRANSACTION_DATA_H_

#include <kundo2command.h>
#include "kis_types.h"
#include <kritaimage_export.h>
#include <functional>
#include <QVector>

class KisTransactionWrapperFactory;
class KisPixelWriteCursor;
class QRect;

/**
 * A tile based undo command.
 *
 * Ordinary KUndo2Command subclasses store parameters and apply the action in
 * the redo() command, however, Krita doesn't work like this. Undo replaces
 * the current tiles in a paint device with the old tiles, redo replaces them
 * again with the new tiles without actually executing the command that changed
 * the image data again.
 */
class KRITAIMAGE_EXPORT KisTransactionData : public KUndo2Command
{
public:
    KisTransactionData(const KUndo2MagicString& name, KisPaintDeviceSP device, bool resetSelectionOutlineCache, KisTransactionWrapperFactory *interstrokeDataFactory, KUndo2Command* parent, bool suppressUpdates);
    ~KisTransactionData() override;

public:
    void redo() override;
    void undo() override;

    virtual void endTransaction();
    // Internal result-bearing endpoint; keeps the legacy virtual ABI intact.
    // Failure leaves the transaction open and its interstroke end command unrun.
    bool tryEndTransaction(QString *error = nullptr);
    // A failed cancellation remains cancelling; only cancellation may be retried.
    bool tryAbortTransaction(QString *error = nullptr);
    bool isTransactionFinished() const;
    // Check admission before starting work; a rejected begin owns no pixels.
    bool hasMemento() const;

    // Internal explicit stroke-owner path. Ordinary transactions remain
    // operation-scoped. Opt in during quiescent initialization before launching
    // workers; this is not a concurrent conversion of existing legacy writers.
    // Admission captures target identity, layout, offset and effective wrap;
    // keep them stable until execution/checkpoint/commit. A changed context is
    // rejected before borrowing; cancellation still targets the saved manager.
    // After opt-in only these declared target operations may
    // write; source input must be immutable and coordinates are manager-local.
    // External/current immutable reads observe the last checkpoint. Scheduling
    // a full cut, job read dependencies and async producers remain the strategy's
    // responsibility; this does not enable arbitrary legacy paint callbacks.
    bool beginStrokeMutation(QString *error = nullptr);
    bool applyStrokePixelOperation(KisPaintDeviceSP target, const QVector<QRect> &rects,
        const std::function<bool(KisPixelWriteCursor *)> &operation, QString *error = nullptr);
    bool checkpointStrokeMutation(QString *error = nullptr);

protected:
    virtual void saveSelectionOutlineCache();
    virtual void restoreSelectionOutlineCache(bool undo);

private:
    void init(KisPaintDeviceSP device);
    void startUpdates();
    void possiblyNotifySelectionChanged();
    void possiblyResetOutlineCache();
    void possiblyFlattenSelection(KisPaintDeviceSP device);
    void doFlattenUndoRedo(bool undo);
    bool strokeMutationTargetIsCurrent(KisPaintDeviceSP target, QString *error) const;

private:
    class Private;
    Private * const m_d;
};

#endif /* KIS_TRANSACTION_DATA_H_ */
