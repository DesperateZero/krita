/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TRANSACTION_H_
#define KIS_TRANSACTION_H_

#include <kundo2command.h>

#include "kis_types.h"

#include "kis_transaction_data.h"
#include "kis_paint_device.h"
#include "kis_pixel_selection.h"
#include "kis_image_config.h"

#include "kis_undo_adapter.h"
#include "kis_post_execution_undo_adapter.h"

class KisTransactionWrapperFactory;

class KisTransaction
{
public:
    enum Flag {
        None = 0x0,
        SuppressUpdates = 0x1
    };
    Q_DECLARE_FLAGS(Flags, Flag)

public:
    KisTransaction(const KUndo2MagicString& name, KisPaintDeviceSP device, KUndo2Command* parent = 0,int timedID = -1, KisTransactionWrapperFactory *interstrokeDataFactory = 0, Flags flags = None) {
        m_transactionData = new KisTransactionData(name, device, true, interstrokeDataFactory, parent, flags & SuppressUpdates);
        m_transactionData->setTimedID(timedID);
    }

    KisTransaction(KisPaintDeviceSP device, KUndo2Command* parent = 0, int timedID = -1, KisTransactionWrapperFactory *interstrokeDataFactory = 0, Flags flags = None)
        : KisTransaction(KUndo2MagicString(), device, parent, timedID, interstrokeDataFactory, flags)
    {
    }

    KisTransaction(KisTransaction &&rhs)
        : m_transactionData(rhs.m_transactionData)
    {
        rhs.m_transactionData = nullptr;
    }

    KisTransaction& operator=(KisTransaction &&rhs)
    {
        delete m_transactionData;
        m_transactionData = rhs.m_transactionData;
        rhs.m_transactionData = nullptr;
        return *this;
    }

    virtual ~KisTransaction() {
        delete m_transactionData;
    }

    KUndo2Command* undoCommand() {
        return m_transactionData;
    }

    bool hasMemento() const {
        return m_transactionData && m_transactionData->hasMemento();
    }

    bool beginStrokeMutation(QString *error = nullptr) {
        return m_transactionData && m_transactionData->beginStrokeMutation(error);
    }
    bool applyStrokePixelOperation(KisPaintDeviceSP target, const QVector<QRect> &rects,
        const std::function<bool(KisPixelWriteCursor *)> &operation, QString *error = nullptr) {
        return m_transactionData && m_transactionData->applyStrokePixelOperation(target, rects, operation, error);
    }
    bool checkpointStrokeMutation(QString *error = nullptr) {
        return m_transactionData && m_transactionData->checkpointStrokeMutation(error);
    }

    bool tryEnd(QString *error = nullptr) {
        KIS_ASSERT(m_transactionData);
        if (!m_transactionData->tryEndTransaction(error)) return false;
        delete m_transactionData;
        m_transactionData = nullptr;
        return true;
    }

    void commit(KisUndoAdapter* undoAdapter) {
        QString error;
        if (!tryCommit(undoAdapter, &error)) {
            qWarning() << "Transaction undo registration failed:" << error;
        }
    }

    bool tryCommit(KisUndoAdapter* undoAdapter, QString *error = nullptr) {
        KIS_ASSERT_X(m_transactionData,
                     "KisTransaction::commit()",
                     "the transaction has been tried to be committed twice");

        if (!m_transactionData->tryEndTransaction(error)) return false;
        undoAdapter->addCommand(m_transactionData);
        m_transactionData = 0;
        return true;
    }

    void commit(KisPostExecutionUndoAdapter* undoAdapter) {
        QString error;
        if (!tryCommit(undoAdapter, &error)) {
            qWarning() << "Transaction undo registration failed:" << error;
        }
    }

    bool tryCommit(KisPostExecutionUndoAdapter* undoAdapter, QString *error = nullptr) {
        KIS_ASSERT_X(m_transactionData,
                     "KisTransaction::commit()",
                     "the transaction has been tried to be committed twice");

        if (!m_transactionData->tryEndTransaction(error)) return false;
        m_transactionData->redo();
        undoAdapter->addCommand(KUndo2CommandSP(m_transactionData));
        m_transactionData = 0;
        return true;
    }

    // Unlike endAndTake's legacy interface, failure is separate from the
    // command output and retains ownership for retry or explicit cancellation.
    bool tryEndAndTake(KUndo2Command *&command, QString *error = nullptr) {
        command = nullptr;
        KIS_ASSERT_X(m_transactionData, "KisTransaction::tryEndAndTake()",
                     "the transaction has already ended");
        if (!m_transactionData->tryEndTransaction(error)) return false;
        command = m_transactionData;
        m_transactionData = nullptr;
        return true;
    }

    KUndo2Command* endAndTake() {
        KIS_ASSERT_X(m_transactionData,
                     "KisTransaction::endAndTake()",
                     "the transaction has been tried to be committed twice");

        m_transactionData->endTransaction();
        KisTransactionData *transactionData = m_transactionData;
        m_transactionData = 0;

        return transactionData;
    }

    void end() {
        KIS_ASSERT_X(m_transactionData, "KisTransaction::end()", "nothing to end!");
        /**
         * We will not call endTransaction for m_transactionData,
         * we'll just kill it, and it'll report about it's death to
         * the memento manager, so no commit will be made
         */
        delete m_transactionData;
        m_transactionData = 0;
    }

    void revert() {
        QString error;
        if (!tryRevert(&error)) {
            qWarning() << "Transaction cancellation failed:" << error;
        }
    }

    bool tryRevert(QString *error = nullptr) {
        KIS_ASSERT_X(m_transactionData,
                     "KisTransaction::reverted()",
                     "the transaction is tried to be reverted()"
                     "after it has already been added to undo adapter");

        if (m_transactionData->isTransactionFinished()) {
            // Compatibility for callers that explicitly ended the data first.
            m_transactionData->undo();
        } else if (!m_transactionData->tryAbortTransaction(error)) {
            return false;
        }
        delete m_transactionData;
        m_transactionData = 0;
        return true;
    }

    KUndo2MagicString text() const {
        KIS_ASSERT_X(m_transactionData,
                     "KisTransaction::text()",
                     "the name has been requested after the transaction"
                     "has already been ended");
        return m_transactionData->text();
    }

protected:
    Q_DISABLE_COPY(KisTransaction);

    KisTransaction() : m_transactionData(0) {}
    KisTransactionData* m_transactionData;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(KisTransaction::Flags)

class KisSelectionTransaction : public KisTransaction
{
public:
    KisSelectionTransaction(KisPixelSelectionSP pixelSelection, KUndo2Command* parent = 0)
    {
        m_transactionData = new KisTransactionData(KUndo2MagicString(), pixelSelection, false, 0, parent, false);
    }

    KisSelectionTransaction(const KUndo2MagicString& name, KisPixelSelectionSP pixelSelection, KUndo2Command* parent = 0)
    {
        m_transactionData = new KisTransactionData(name, pixelSelection, false, 0, parent, false);
    }
};

#endif /* KIS_TRANSACTION_H_ */
