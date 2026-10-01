/*
 *  SPDX-FileCopyrightText: 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISDABRENDERINGQUEUETEST_H
#define KISDABRENDERINGQUEUETEST_H

#include <QObject>

class KisDabRenderingQueueTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void testBrushQueuedContext_data();
    void testBrushQueuedContext();
    void testSharpnessLine_data();
    void testSharpnessLine();
    void testBrushPagePartition_data();
    void testBrushPagePartition();
    void testCachedDabs();
    void testPostprocessedDabs();
    void testRunningJobs();

    void testExecutor();
};

#endif // KISDABRENDERINGQUEUETEST_H
