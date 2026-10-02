/*
 * SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <QApplication>
#include <simpletest.h>
#include "matched_freehand_baseline.h"

class MatchedFreehandBaselineTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testMatchedCpuBaseline() { TestUtil::runMatchedFreehandBaseline(); }
};

// The auto brush has no external brush/texture/font resources. Use the normal
// plugin registry lazily, without testui's unrelated full font DB population.
SIMPLE_TEST_MAIN(MatchedFreehandBaselineTest)
#include "matched_freehand_baseline_test.moc"
