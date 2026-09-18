/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_EVALUATION_GRAPH_LOWERER_H
#define KIS_EVALUATION_GRAPH_LOWERER_H

#include <QByteArray>
#include <QSet>
#include <QString>
#include <QVector>

#include "KisEvaluationGraphTypes.h"

/**
 * Immutable input captured from an authoring model. immutablePayload is owned
 * by the schema-specific lowerer and may not contain QObject/native pointers.
 */
struct KRITAIMAGE_EXPORT KisAuthoringGraphSnapshot
{
    QByteArray schemaId;
    quint32 schemaVersion = 0;
    quint64 documentId = 0;
    quint64 graphRevision = 0;
    QByteArray immutablePayload;
    QVector<KisEvaluationValueVersion> canonicalInputs;
    QVector<KisEvaluationValueId> mutationRoots;
    bool exact = false;

    bool isValid() const
    {
        if (schemaId.isEmpty() || schemaVersion == 0 || documentId == 0 ||
            graphRevision == 0 || immutablePayload.isEmpty() || !exact) {
            return false;
        }
        QSet<quint64> inputValues;
        QSet<quint64> surfaceInputValues;
        for (const KisEvaluationValueVersion &input : canonicalInputs) {
            if (!input.isValid() || inputValues.contains(input.value.value)) return false;
            inputValues.insert(input.value.value);
            if (input.type.isSurfaceBacked()) {
                surfaceInputValues.insert(input.value.value);
            }
        }
        QSet<quint64> roots;
        for (KisEvaluationValueId root : mutationRoots) {
            if (!root.isValid() || roots.contains(root.value) ||
                !surfaceInputValues.contains(root.value)) {
                return false;
            }
            roots.insert(root.value);
        }
        return true;
    }
};

/**
 * Provisional internal lowering boundary. This is a source-level seam for
 * Krita-owned frontends, not a frozen binary plugin ABI.
 */
class KRITAIMAGE_EXPORT KisEvaluationGraphLowerer
{
public:
    virtual ~KisEvaluationGraphLowerer();

    virtual bool supports(const KisAuthoringGraphSnapshot &source) const = 0;
    virtual KisEvaluationGraphSnapshot lower(
        const KisAuthoringGraphSnapshot &source,
        const KisEvaluationContext &context,
        QString *error) const = 0;
};

/**
 * Compatibility frontend for the existing KisImage/KisNode layer tree. BR4
 * will snapshot layer semantics and lower them into the same EvalGraphIR used
 * by a future flow editor; it is not the global evaluation planner.
 */
class KRITAIMAGE_EXPORT KisLayerStackLowerer final : public KisEvaluationGraphLowerer
{
public:
    static QByteArray sourceSchemaId();

    bool supports(const KisAuthoringGraphSnapshot &source) const override;
    KisEvaluationGraphSnapshot lower(
        const KisAuthoringGraphSnapshot &source,
        const KisEvaluationContext &context,
        QString *error) const override;
};

#endif // KIS_EVALUATION_GRAPH_LOWERER_H
