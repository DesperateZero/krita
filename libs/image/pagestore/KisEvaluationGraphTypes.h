/*
 *  SPDX-FileCopyrightText: 2026 Krita contributors
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_EVALUATION_GRAPH_TYPES_H
#define KIS_EVALUATION_GRAPH_TYPES_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include "KisPageStoreTypes.h"

struct KRITAIMAGE_EXPORT KisEvaluationEpochId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisEvaluationEpochId lhs, KisEvaluationEpochId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisEvaluationNodeId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisEvaluationNodeId lhs, KisEvaluationNodeId rhs)
{
    return lhs.value == rhs.value;
}

struct KRITAIMAGE_EXPORT KisEvaluationValueId
{
    quint64 value = 0;

    bool isValid() const { return value != 0; }
};

inline bool operator==(KisEvaluationValueId lhs, KisEvaluationValueId rhs)
{
    return lhs.value == rhs.value;
}

enum class KisEvaluationStorageClass : quint8 {
    Unknown,
    Surface,
    InlineValue,
    Resource,
    External
};

/**
 * Open semantic value type. typeId/version may be extended by internal node
 * providers without adding another core enum member; storageClass only
 * selects the physical ownership boundary.
 */
struct KRITAIMAGE_EXPORT KisEvaluationValueType
{
    QByteArray typeId;
    quint64 semanticVersion = 0;
    KisEvaluationStorageClass storageClass = KisEvaluationStorageClass::Unknown;

    bool isValid() const
    {
        return !typeId.isEmpty() && semanticVersion != 0 &&
               storageClass != KisEvaluationStorageClass::Unknown;
    }

    bool isSurfaceBacked() const
    {
        return storageClass == KisEvaluationStorageClass::Surface;
    }
};

inline bool operator==(const KisEvaluationValueType &lhs,
                       const KisEvaluationValueType &rhs)
{
    return lhs.typeId == rhs.typeId &&
           lhs.semanticVersion == rhs.semanticVersion &&
           lhs.storageClass == rhs.storageClass;
}

inline bool operator!=(const KisEvaluationValueType &lhs,
                       const KisEvaluationValueType &rhs)
{
    return !(lhs == rhs);
}

/**
 * Exact identity of an evaluated value. Surface-backed values use PageStore;
 * other value types are owned by a separate value/resource store and carry a
 * stable content digest. PageStore must not become a generic object store.
 */
struct KRITAIMAGE_EXPORT KisEvaluationValueVersion
{
    KisEvaluationValueId value;
    quint64 generation = 0;
    KisEvaluationValueType type;
    KisSurfaceVersion surface;
    QByteArray contentDigest;

    bool isValid() const
    {
        if (!value.isValid() || generation == 0 || !type.isValid()) {
            return false;
        }
        return type.isSurfaceBacked()
            ? surface.isValid() && surface.generation.value == generation
            : !contentDigest.isEmpty();
    }
};

struct KRITAIMAGE_EXPORT KisEvaluationPortDesc
{
    quint32 portId = 0;
    QByteArray name;
    KisEvaluationValueType type;
    quint64 surfaceFormatId = 0;
    KisEvaluationValueId producedValue;
    bool optional = false;

    bool isValid() const
    {
        return portId != 0 && !name.isEmpty() && type.isValid() &&
               (type.isSurfaceBacked() ? surfaceFormatId != 0 : surfaceFormatId == 0);
    }
};

struct KRITAIMAGE_EXPORT KisEvaluationPortRef
{
    KisEvaluationNodeId node;
    quint32 portId = 0;

    bool isValid() const { return node.isValid() && portId != 0; }
};

enum class KisEvaluationRegionPolicy : quint8 {
    Pointwise,
    Halo,
    Transform,
    Global,
    External
};

enum class KisEvaluationPurity : quint8 {
    Pure,
    Stateful,
    SideEffecting
};

enum class KisEvaluationDeterminism : quint8 {
    Deterministic,
    Seeded,
    NonDeterministic
};

struct KRITAIMAGE_EXPORT KisEvaluationNodeDesc
{
    KisEvaluationNodeId id;
    QByteArray operationId;
    quint64 semanticVersion = 0;
    QVector<KisEvaluationPortDesc> inputs;
    QVector<KisEvaluationPortDesc> outputs;
    QByteArray immutableParameters;
    KisEvaluationRegionPolicy regionPolicy = KisEvaluationRegionPolicy::Pointwise;
    quint32 haloX = 0;
    quint32 haloY = 0;
    KisEvaluationPurity purity = KisEvaluationPurity::Pure;
    KisEvaluationDeterminism determinism = KisEvaluationDeterminism::Deterministic;

    bool isValid() const
    {
        if (!id.isValid() || operationId.isEmpty() || semanticVersion == 0 ||
            outputs.isEmpty()) {
            return false;
        }
        for (const KisEvaluationPortDesc &port : inputs) {
            if (!port.isValid() || port.producedValue.isValid()) return false;
        }
        for (const KisEvaluationPortDesc &port : outputs) {
            if (!port.isValid() || !port.producedValue.isValid()) return false;
        }
        return regionPolicy == KisEvaluationRegionPolicy::Halo || (haloX == 0 && haloY == 0);
    }
};

struct KRITAIMAGE_EXPORT KisEvaluationEdgeDesc
{
    KisEvaluationPortRef source;
    KisEvaluationPortRef destination;

    bool isValid() const { return source.isValid() && destination.isValid(); }
};

struct KRITAIMAGE_EXPORT KisEvaluationContext
{
    quint64 contextVersion = 0;
    qint64 animationTime = 0;
    quint32 levelOfDetail = 0;
    QByteArray workingSpaceId;
    QByteArray policyDigest;

    bool isValid() const
    {
        return contextVersion != 0 && !workingSpaceId.isEmpty() && !policyDigest.isEmpty();
    }
};

/**
 * Immutable backend-neutral semantic DAG. It is neither the saved authoring
 * graph nor a GPU submission graph. Layer stacks and future flow editors lower
 * into this representation before scheduling or backend selection.
 *
 * mutationRoots may be empty for pure generation, display, or export graphs.
 */
struct KRITAIMAGE_EXPORT KisEvaluationGraphSnapshot
{
    KisEvaluationEpochId epoch;
    quint64 documentId = 0;
    quint64 graphRevision = 0;
    KisEvaluationContext context;
    QVector<KisEvaluationNodeDesc> nodes;
    QVector<KisEvaluationEdgeDesc> edges;
    QVector<KisEvaluationPortRef> requestedOutputs;
    QVector<KisEvaluationValueId> mutationRoots;
    QVector<quint64> propertyGenerations;
    bool exact = false;

    QString validationError() const;

    bool isValid() const { return validationError().isEmpty(); }
};

#endif // KIS_EVALUATION_GRAPH_TYPES_H
