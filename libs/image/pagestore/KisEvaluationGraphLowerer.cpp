/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisEvaluationGraphLowerer.h"

KisEvaluationGraphLowerer::~KisEvaluationGraphLowerer() = default;

QByteArray KisLayerStackLowerer::sourceSchemaId()
{
    return QByteArrayLiteral("org.krita.layer-stack");
}

bool KisLayerStackLowerer::supports(const KisAuthoringGraphSnapshot &source) const
{
    return source.schemaId == sourceSchemaId() && source.schemaVersion == 1;
}

KisEvaluationGraphSnapshot KisLayerStackLowerer::lower(
    const KisAuthoringGraphSnapshot &source,
    const KisEvaluationContext &context,
    QString *error) const
{
    Q_UNUSED(source);
    Q_UNUSED(context);
    if (error) {
        *error = QStringLiteral("Layer-stack lowering is unavailable before BR4");
    }
    // Fail closed: never emit an approximate graph or silently omit a layer,
    // mask, group, filter, blend, animation, or color-space semantic.
    return {};
}
