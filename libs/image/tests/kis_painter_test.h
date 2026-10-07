/*
 *  SPDX-FileCopyrightText: 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PAINTER_TEST_H
#define KIS_PAINTER_TEST_H

#include <simpletest.h>

class KoColorSpace;

class KisPainterTest : public QObject
{
    Q_OBJECT

private:

    void allCsApplicator(void (KisPainterTest::* funcPtr)(const KoColorSpace*cs));
    void testSimpleBlt(const KoColorSpace * cs);
    void testPaintDeviceBltSelection(const KoColorSpace * cs);
    void testPaintDeviceBltSelectionIrregular(const KoColorSpace * cs);
    void testPaintDeviceBltSelectionInverted(const KoColorSpace * cs);

    void checkPerformance();


private Q_SLOTS:

    void testAlphaDarkenSpanInvariant_data();
    void testAlphaDarkenSpanInvariant();
    void testFixedCursor_data();
    void testFixedCursor();
    void testFixedCursorReuse_data();
    void testFixedCursorReuse();
    void testSharpnessMirrorOwner_data();
    void testSharpnessMirrorOwner();
    void testFixedCursorOwnerRejection();
    void testFixedCursorCancellation();
    void testFixedCursorSnapshots_data();
    void testFixedCursorSnapshots();

    void testCompositeOverTransparentLanes_data();
    void testCompositeOverTransparentLanes();
    void testMappedWritePartition_data();
    void testMappedWritePartition();
    void testMappedWritePartitionParallel_data();
    void testMappedWritePartitionParallel();
    void testMappedWritePartitionRejection();
    void testMultiDabSparseFootprint_data();
    void testMultiDabSparseFootprint();
    void testMultiDabSparseConcurrent_data();
    void testMultiDabSparseConcurrent();
    void testSparsePixelOperationBounds_data();
    void testSparsePixelOperationBounds();
    void testMultiDabOperation_data();
    void testMultiDabOperation();
    void testMultiDabReuse_data();
    void testMultiDabReuse();
    void testMultiDabConcurrent();
    void testMultiDabCancellation();
    void testMultiDabSelectionSnapshot();
    void testMultiDabOwnerRejection();

    void testSimpleBlt();
    void testSelectionBltSelectionIrregular(); // Irregular selection
    void testPaintDeviceBltSelectionInverted(); // Inverted selection
    void testPaintDeviceBltSelectionIrregular(); // Irregular selection
    void testPaintDeviceBltSelection(); // Square selection
    void testSelectionBltSelection(); // Square selection
    void testSimpleAlphaCopy();
    void testSelectionBitBltFixedSelection();
    void testSelectionBitBltEraseCompositeOp();

    void testBitBltOldData();

    void testMassiveBltFixedSingleTile();
    void testMassiveBltFixedMultiTile();

    void testMassiveBltFixedMultiTileWithOpacity();

    void testMassiveBltFixedMultiTileWithSelection();

    void testMassiveBltFixedCornerCases();


    void testOptimizedCopying();
    void testPageStoreBitBltReadBoundary_data();
    void testPageStoreBitBltReadBoundary();
    void testPageStoreBitBltLiveSource_data();
    void testPageStoreBitBltLiveSource();
    void testPageStoreBitBltSelfCopy_data();
    void testPageStoreBitBltSelfCopy();
    void testPageStoreBitBltWriteOperation_data();
    void testPageStoreBitBltWriteOperation();
    void testPageStorePixelOperationCoordinates();
};

#endif
