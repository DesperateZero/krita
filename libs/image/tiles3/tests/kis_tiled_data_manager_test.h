/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_TILED_DATA_MANAGER_TEST_H
#define KIS_TILED_DATA_MANAGER_TEST_H

#include <simpletest.h>

class KisTiledDataManager;

class KisTiledDataManagerTest : public QObject
{
    Q_OBJECT

private:
    bool checkHole(quint8* buffer, quint8 holeColor, QRect holeRect,
                   quint8 backgroundColor, QRect backgroundRect);

    bool checkTilesShared(KisTiledDataManager *srcDM,
                          KisTiledDataManager *dstDM,
                          bool takeOldSrc, bool takeOldDst,
                          QRect tilesRect);

    bool checkTilesNotShared(KisTiledDataManager *srcDM,
                             KisTiledDataManager *dstDM,
                             bool takeOldSrc, bool takeOldDst,
                             QRect tilesRect);

    void benchmarkCOWImpl();

private Q_SLOTS:
    void testExtentGrowthReusesNegativeCapacity();
    void testPageStoreHistoryDirtyExtent();
    void testUndoingNewTiles();
    void testPurgedAndEmptyTransactions();
    void testUnversionedBitBlt();
    void testVersionedBitBlt();
    void testBitBltOldData();
    void testBitBltRough();
    void testTransactions();
    void testPurgeHistory();
    void testUndoSetDefaultPixel();
    void testDefaultBudgetFailure_data();
    void testDefaultBudgetFailure();
    void testConstructorBudgetFailure();
    void testPageStoreSaveReopenRoundTrip();
    void testPageStoreSparsePurge();
    void testPageStoreVirtualDefaultReadStaysSparse();
    void testPageStoreCompatibilityBehavior();
    void testPageStorePackedReadCapturesOneView_data();
    void testPageStorePackedReadCapturesOneView();
    void testPageStorePackedReadSurvivesPublication_data();
    void testPageStorePackedReadSurvivesPublication();
    void testPageStoreCommitCleanupAllowsReader_data();
    void testPageStoreCommitCleanupAllowsReader();
    void testPageStoreOverlayExtent_data();
    void testPageStoreOverlayExtent();
    void testPageStoreOverlayExtentSibling_data();
    void testPageStoreOverlayExtentSibling();
    void testPageStoreOverlayExtentRangeFailure();
    void testPageStoreIteratorFixedView_data();
    void testPageStoreIteratorFixedView();
    void testPageStoreIteratorLiveWriteCompatibility_data();
    void testPageStoreIteratorLiveWriteCompatibility();
    void testPageStoreIteratorOtherThreadReadsSealedView();
    void testPageStoreReadCursorBounded();
    void testPageStoreWrappedIteratorFixedView_data();
    void testPageStoreWrappedIteratorFixedView();
    void testPageStoreRepeatIteratorFixedView_data();
    void testPageStoreRepeatIteratorFixedView();
    void testPageStoreIteratorNextPixelsOffsets();
    void testPageStorePlanarReadFixedView_data();
    void testPageStorePlanarReadFixedView();
    void testPageStorePlanarWriteMutation_data();
    void testPageStorePlanarWriteMutation();
    void testPageStoreHistoryMutation_data();
    void testPageStoreHistoryMutation();
    void testPageStoreHistoryMutationFailure_data();
    void testPageStoreHistoryMutationFailure();
    void testPageStoreHistoryMutationClosing_data();
    void testPageStoreHistoryMutationClosing();
    void testPageStoreHistoryMutationOwner_data();
    void testPageStoreHistoryMutationOwner();
    void testPageStoreMetadataRefresh_data();
    void testPageStoreMetadataRefresh();
    void testExtentStoragePreparation();
    void testExtentStorageConcurrent();
    void testPageStoreExtentStorage_data();
    void testPageStoreExtentStorage();
    void testPageStorePresenceStorage_data();
    void testPageStorePresenceStorage();
    void testPageStorePixelOperation_data();
    void testPageStorePixelOperation();
    void testPageStorePixelOperationPreflightBudgetPressure();
    void testPageStoreChangedStorage_data();
    void testPageStoreChangedStorage();
    void testPageStoreChangedStorageBudget();
    void testPageStorePackedChangedStorage_data();
    void testPageStorePackedChangedStorage();
    void testPageStorePixelOperationBorrowed();
    void testPageStorePixelOperationConcurrent_data();
    void testPageStorePixelOperationConcurrent();
    void testPageStoreAdapterPreparationRefusal_data();
    void testPageStoreAdapterPreparationRefusal();
    void testPageStoreRestoreLateAdmission();
    void testPageStoreAdapterAfterAbort_data();
    void testPageStoreAdapterAfterAbort();
    void testPageStoreAdapterBarrier_data();
    void testPageStoreAdapterBarrier();
    void testPageStoreAdapterDelivery_data();
    void testPageStoreAdapterDelivery();
    void testPageStoreAdapterDeliveryLifetime_data();
    void testPageStoreAdapterDeliveryLifetime();
    void testPageStoreAdapterDeliveryLegacyConflict_data();
    void testPageStoreAdapterDeliveryLegacyConflict();
    void testPageStoreClearRangeAdmission_data();
    void testPageStoreClearRangeAdmission();
    void testPageStoreCancelledPrivateClient_data();
    void testPageStoreCancelledPrivateClient();
    void testPageStorePlanarWriteCoordinateLimits();
    void testPageStorePlanarWriteNoOpInputs();
    void testPageStoreTrimMutation_data();
    void testPageStoreTrimMutation();
    void testPageStoreRegionWriteFailure_data();
    void testPageStoreRegionWriteFailure();
    void testVoidDrawingFailure_data();
    void testVoidDrawingFailure();
    void testIteratorRejectedWriteAdmission_data();
    void testIteratorRejectedWriteAdmission();
    void testPageStoreSemanticRemovalFailure_data();
    void testPageStoreSemanticRemovalFailure();
    void testPageStoreAliasRegionFailure_data();
    void testPageStoreAliasRegionFailure();
    void testPageStoreCopyFixedView_data();
    void testPageStoreCopyFixedView();
    void testPageStoreCopyLiveWriteCompatibility_data();
    void testPageStoreCopyLiveWriteCompatibility();
    void testPageStoreCompatibilityCopyFailure_data();
    void testPageStoreCompatibilityCopyFailure();
    void testPageStoreDefaultLifecycleDoesNotMaterialize_data();
    void testPageStoreDefaultLifecycleDoesNotMaterialize();
    void testPageStoreAbortUnderBudgetPressure_data();
    void testPageStoreAbortUnderBudgetPressure();
    void testPageStoreHistoryClearStagesAbsence_data();
    void testPageStoreHistoryClearStagesAbsence();
    void testPageStoreResidentReadReusesNativeGuard();
    void testPageStoreIdentityCacheLifetime_data();
    void testPageStoreIdentityCacheLifetime();
    void testPageStoreCopyBoundsSelection_data();
    void testPageStoreCopyBoundsSelection();
    void testPageStoreClonePublishedBounds_data();
    void testPageStoreClonePublishedBounds();
    void testPageStoreFreshReadSelection_data();
    void testPageStoreFreshReadSelection();
    void testPageStoreFreshReadWarmPath_data();
    void testPageStoreFreshReadWarmPath();
    void testPageStoreFreshReadOutlivesManager();
    void testPageStoreFreshReadDefaultRevision_data();
    void testPageStoreFreshReadDefaultRevision();
    void testPageStoreFreshReadFailure();
    void testPageStoreFreshReadDefaultPressure();
    void testPageStoreWriteIntentDoesNotAllocateUntilDataExposure();
    void testPageStoreNestedTileCapabilityLifetime_data();
    void testPageStoreNestedTileCapabilityLifetime();
    void testPageStoreClearBarrierCancelsLegacyWriter();
    void testIteratorClearBarrier_data();
    void testIteratorClearBarrier();
    void testIteratorClearRejectsForeignBatch();
    void testClearRejectsNativeBatch();
    void testClearRetainsBareLease_data();
    void testClearRetainsBareLease();
    void testPageStoreNativeIteratorEntryPoints_data();
    void testPageStoreNativeIteratorEntryPoints();
    void testPageStoreWriteBytesPinWorkingSet_data();
    void testPageStoreWriteBytesPinWorkingSet();
    void testPageStorePackedWriteNative_data();
    void testPageStorePackedWriteNative();
    void testPageStorePackedWritePreclaimExcludesLateWriter_data();
    void testPageStorePackedWritePreclaimExcludesLateWriter();
    void testPageStorePackedWriteBorrowedAndInvalid();
    void testPageStorePackedWriteBodyAllowsReader();
    void testPageStoreWholeTileFillUsesSynchronousAdoption();
    void testPageStoreBulkNoOpAndBitBltBatching();
    void testIteratorWriteScope_data();
    void testIteratorWriteScope();
    void testIteratorNestedWriteScope_data();
    void testIteratorNestedWriteScope();
    void testIteratorSwapInFailureDropsPointers_data();
    void testIteratorSwapInFailureDropsPointers();
    void testPageStoreFixedPageWork_data();
    void testPageStoreFixedPageWork();

    void benchmarkReadOnlyTileLazy();
    void benchmarkSharedPointers();

    void benchmarkCOWNoPooler();
    void benchmarkCOWWithPooler();

    void stressTest();

    void stressTestLazyCopying();

    void stressTestExtentsColumn();

    void benchmarkQRegion();
    void benchmarkKisRegion();
    void benchmarkOverlappedKisRegion();
};

#endif /* KIS_TILED_DATA_MANAGER_TEST_H */
