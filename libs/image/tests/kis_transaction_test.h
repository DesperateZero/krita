/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_TRANSACTION_TEST_H
#define KIS_TRANSACTION_TEST_H

#include <simpletest.h>

class KisTransactionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testStrokeMappingContext_data();
    void testStrokeMappingContext();
    void testOrdinaryTransactionMove();

    void testStrokeMutationOwner_data();
    void testStrokeMutationOwner();
    void testStrokeMutationDestruction();
    void testStrokeMutationFailure();
    void testStrokeMutationContext_data();
    void testStrokeMutationContext();
    void testUndo();
    void testRedo();
    void testDeviceMove();
    void testRejectedCommitIsRetryable_data();
    void testRejectedCommitIsRetryable();
    void testPainterRetainsRejectedTransaction();
    void testDirectAbort_data();
    void testDirectAbort();
    void testAbortWithLiveWriter();
    void testAbortRejectedBegin();
    void testAbortInterstrokeData_data();
    void testAbortInterstrokeData();
    void testAbortWithUnswitchedFrame();
    void testAbortSelectionCache();

    void testUndoWithUnswitchedFrames();

    void testTransactionWrapperFactory();

    void testInterstrokeData();

    void testInterstrokeDataWithUnswitchedFrames();
};

#endif
