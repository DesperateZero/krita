/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_LOCKLESS_STACK_TEST_H
#define KIS_LOCKLESS_STACK_TEST_H

#include <simpletest.h>

class KisAbstractIntStack;

class KisLocklessStackTest : public QObject
{
    Q_OBJECT

private:
    void runStressTest(KisAbstractIntStack &stack);

private Q_SLOTS:
    void testOperations();
    void testPreparedRetirement();
    void testEmbeddedRetirement();
    void testConcurrentRetirement();
    void testInsertIfAbsentAcrossMigration();
    void testConcurrentInsertIfAbsent();
    void testTileHashReferenceLifetime();
    void testTileHashNotificationFailure();
    void testTileHashConcurrentClear();
    void testReadEraseDuringPartialMigration();
    void testMigrationOverflowCompletion();
    void testPrepareRedirectedEmptyCell();
    void testPartialMigrationTileHashDestruction();
    void testInvalidMapCapacity();
    void testCoordinatorRetryWakeup();
    void testPreparedKeySurvivesMigration();
    void testPreparedKeyLogicalOperations();
    void testPreparedKeyDuringPartialMigration();
    void testPreparedKeyCloseAndLifetime();
    void testPreparedKeyConcurrentRelease();
    void testPreparedKeySameKeyCompetition();
    void testPreparedKeyMapAddressReuse();
    void testTileHashPreparedKeyLifecycle();
    void testPreparedTileCandidatesLockFree();
    void testPreparedTileCandidatesChained();
    void stressTestLockless();
    void stressTestQStack();

    void stressTestClear();


    void stressTestBulkPop();
};

#endif /* KIS_LOCKLESS_STACK_TEST_H */
