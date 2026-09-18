/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisEvaluationGraphTypes.h"

#include <QHash>
#include <QQueue>
#include <QSet>

QString KisEvaluationGraphSnapshot::validationError() const
{
    if (!epoch.isValid() || documentId == 0 || graphRevision == 0) {
        return QStringLiteral("Evaluation graph identity or revision is missing");
    }
    if (!context.isValid()) {
        return QStringLiteral("Evaluation graph context is invalid");
    }
    if (!exact || nodes.isEmpty() || requestedOutputs.isEmpty()) {
        return QStringLiteral("Evaluation graph is incomplete or approximate");
    }

    QHash<quint64, QHash<quint32, KisEvaluationPortDesc>> inputPorts;
    QHash<quint64, QHash<quint32, KisEvaluationPortDesc>> outputPorts;
    QHash<quint64, int> indegrees;
    QHash<quint64, QVector<quint64>> successors;
    QSet<quint64> producedValues;

    for (const KisEvaluationNodeDesc &node : nodes) {
        if (!node.isValid() || inputPorts.contains(node.id.value)) {
            return QStringLiteral("Evaluation graph contains an invalid or duplicate node");
        }

        QHash<quint32, KisEvaluationPortDesc> nodeInputs;
        QHash<quint32, KisEvaluationPortDesc> nodeOutputs;
        for (const KisEvaluationPortDesc &port : node.inputs) {
            if (nodeInputs.contains(port.portId)) {
                return QStringLiteral("Evaluation node contains a duplicate input port");
            }
            nodeInputs.insert(port.portId, port);
        }
        for (const KisEvaluationPortDesc &port : node.outputs) {
            if (nodeOutputs.contains(port.portId) ||
                producedValues.contains(port.producedValue.value)) {
                return QStringLiteral("Evaluation graph contains a duplicate output port or value");
            }
            nodeOutputs.insert(port.portId, port);
            producedValues.insert(port.producedValue.value);
        }
        inputPorts.insert(node.id.value, nodeInputs);
        outputPorts.insert(node.id.value, nodeOutputs);
        indegrees.insert(node.id.value, 0);
    }

    QSet<QString> connectedInputs;
    for (const KisEvaluationEdgeDesc &edge : edges) {
        if (!edge.isValid() ||
            !outputPorts.contains(edge.source.node.value) ||
            !inputPorts.contains(edge.destination.node.value)) {
            return QStringLiteral("Evaluation graph contains an invalid or foreign edge");
        }
        const auto sourceIt = outputPorts[edge.source.node.value].constFind(edge.source.portId);
        const auto destinationIt = inputPorts[edge.destination.node.value].constFind(edge.destination.portId);
        if (sourceIt == outputPorts[edge.source.node.value].constEnd() ||
            destinationIt == inputPorts[edge.destination.node.value].constEnd() ||
            sourceIt->type != destinationIt->type ||
            sourceIt->surfaceFormatId != destinationIt->surfaceFormatId) {
            return QStringLiteral("Evaluation edge has a missing or type-mismatched port");
        }

        const QString destinationKey = QString::number(edge.destination.node.value) +
                                       QLatin1Char(':') + QString::number(edge.destination.portId);
        if (connectedInputs.contains(destinationKey)) {
            return QStringLiteral("Evaluation input has more than one producer");
        }
        connectedInputs.insert(destinationKey);
        successors[edge.source.node.value].append(edge.destination.node.value);
        ++indegrees[edge.destination.node.value];
    }

    for (const KisEvaluationNodeDesc &node : nodes) {
        for (const KisEvaluationPortDesc &port : node.inputs) {
            const QString inputKey = QString::number(node.id.value) +
                                     QLatin1Char(':') + QString::number(port.portId);
            if (!port.optional && !connectedInputs.contains(inputKey)) {
                return QStringLiteral("Evaluation graph leaves a required input unconnected");
            }
        }
    }

    for (const KisEvaluationPortRef &output : requestedOutputs) {
        if (!output.isValid() || !outputPorts.contains(output.node.value) ||
            !outputPorts[output.node.value].contains(output.portId)) {
            return QStringLiteral("Evaluation graph requests a missing output");
        }
    }
    for (KisEvaluationValueId root : mutationRoots) {
        if (!root.isValid() || !producedValues.contains(root.value)) {
            return QStringLiteral("Evaluation graph contains a missing mutation root value");
        }
    }
    for (quint64 generation : propertyGenerations) {
        if (generation == 0) {
            return QStringLiteral("Evaluation graph contains an invalid property generation");
        }
    }

    QQueue<quint64> ready;
    for (auto it = indegrees.constBegin(); it != indegrees.constEnd(); ++it) {
        if (it.value() == 0) ready.enqueue(it.key());
    }
    int visitedNodes = 0;
    while (!ready.isEmpty()) {
        const quint64 node = ready.dequeue();
        ++visitedNodes;
        for (quint64 successor : successors.value(node)) {
            const int nextIndegree = --indegrees[successor];
            if (nextIndegree == 0) ready.enqueue(successor);
        }
    }
    if (visitedNodes != nodes.size()) {
        return QStringLiteral("Evaluation graph contains a cycle; use an explicit state/delay node");
    }

    return {};
}
