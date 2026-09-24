#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Krita contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""BR1 PageStore containment inventory and architecture ratchet.

This checker intentionally does not decide whether an existing compatibility
path is removable.  It freezes the dirty BR1 source inventory, validates the
machine-readable intrusion/ownership manifests, and prevents the current
PageStore intrusion and duplicate-owner surface from growing while R1-R4
replace it.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any, Iterable


SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[2]
AUDIT_ROOT = SCRIPT_PATH.parent / "pagestore-audit"
DEFAULT_MANIFEST = AUDIT_ROOT / "intrusion-manifest.json"
DEFAULT_OWNERSHIP = AUDIT_ROOT / "state-ownership.json"
DEFAULT_BASELINE = AUDIT_ROOT / "baseline-source.json"
DEFAULT_CENSUS = AUDIT_ROOT / "runtime-census.json"
DEFAULT_CALLGRAPH = AUDIT_ROOT / "reverse-callgraph.json"

SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc", ".c", ".cpp", ".cc", ".cxx", ".m", ".mm"}
BUILD_SOURCE_NAMES = {"CMakeLists.txt"}
BUILD_SOURCE_SUFFIXES = {".cmake"}
INVENTORY_ROOTS = ("libs/image", "libs/vulkanbackend")
EXCLUDED_PARTS = {".git", "_build-br0-cpu", "_build-cpu", "build"}

PAGESTORE_IDENTIFIER_RE = re.compile(
    r"\b(?:KisPageStore\w*|KisCapturedReadView|KisCpuWriteGuard|"
    r"KisCpuMutationSession|KisPageMutationSession|KisTilePageStore\w*|"
    r"KisTiledDataManagerPageStore\w*)\b"
)
TYPE_RE = re.compile(
    r"^\s*(?:class|struct|enum(?:\s+class)?)\s+(?:KRITA\w+_EXPORT\s+)?([A-Za-z_]\w*)",
    re.MULTILINE,
)
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
CONTAINER_RE = re.compile(
    r"\b(?:QHash|QSet|QMap|QVector|QList|QSharedPointer|QScopedPointer|"
    r"std::(?:vector|list|map|unordered_map|unordered_set|shared_ptr|unique_ptr))\s*<"
)
SYNC_RE = re.compile(r"\b(?:QMutex|QReadWriteLock|QWaitCondition|std::mutex|std::condition_variable)\b")
RETIRED_WRITE_PROTOCOL_RE = re.compile(
    r"\b(?:seedNextWrite|discardWriteSeed|pendingWriteSeeds|adoptSynchronousHost\w*|"
    r"KisPageStoreManagedRange|KisPageStoreLegacyPage|tileDataForLeasedReplica|"
    r"KisTiledDataManagerPageStoreShadow|KisTiledPageStoreShadowConfig|"
    r"KisTiledPageShadowSnapshot|KisTiledSurfaceShadowSnapshot|"
    r"KisMaterializationKey|KisMaterializationReservation|KisPageCompletionEvent|"
    r"reserveMaterialization|bindMaterialization|rejectPlannedMaterialization|"
    r"queueMaterializationCompletion|activeMaterializationCount|"
    r"KisMutationSourceSet|planEntry|DiscardSupersededPreparedVersions|"
    r"discardSupersededPreparedVersion|discardReadSuperseded|m_discardSuperseded)\b"
)
FIELD_RE = re.compile(
    r"^\s*(?:(?:mutable|const|static|std::atomic_bool)\s+)*"
    r"(?:[A-Za-z_]\w*(?:::\w+)*(?:\s*<[^;{}]+>)?(?:\s*[*&])?)\s+"
    r"(m_[A-Za-z_]\w*)\s*(?:[;={])",
    re.MULTILINE,
)
BACKEND_DUPLICATE_STATE_RE = re.compile(
    r"\b(?:QHash|QSet)\s*<[^;]+>\s*"
    r"(iteratorWriteBoundaries|anonymousLeases|activeWritablePages|"
    r"operationWritablePages|cpuMutationBatches|cpuMutationPages)\s*;"
)
DEFAULT_STORAGE_PRIVATE_STATE_RE = re.compile(
    r"\b(?:defaultBufferMutex|defaultBuffers|defaultBufferOrder|"
    r"defaultBufferBytes|defaultStorageStatistics|defaultPreparations|"
    r"defaultPreparationFinished)\b"
)
RETIREMENT_QUEUE_PRIVATE_STATE_RE = re.compile(
    r"^\s*(?:mutable\s+)?(?:QMutex|QWaitCondition|bool|qsizetype|quint64|"
    r"std::deque\s*<[^\n;]+>)\s+"
    r"(retirementMutex|pendingRetiredReplicas|readyRetiredReplicas|"
    r"retirementIdle|retirementJobScheduled|retirementClosing|"
    r"activeRetirementReplicas|backgroundRetirementPasses|"
    r"maximumReplicasPerRetirementPass|pendingRetiredBytes)\b",
    re.MULTILINE,
)
HISTORY_COLLECTOR_PRIVATE_STATE_RE = re.compile(
    r"^\s*(?:mutable\s+)?(?:QHash|QSet|QVector|QWaitCondition|bool|qsizetype|quint64|"
    r"std::deque)\b[^;\n]*\b"
    r"(deferredHistoricalGc|pendingHistoricalGc|readyHistoricalGc|"
    r"queuedHistoricalGc|historyScans|historyJobScheduled|"
    r"historyRescanRequested|cachedHistoryReachableVersions)\b[^;\n]*;",
    re.MULTILINE,
)
READ_COORDINATOR_PRIVATE_STATE_RE = re.compile(
    r"^\s*(?:mutable\s+)?(?:QHash|QVector|qsizetype|quint64)\b[^;\n]*\b"
    r"(readRequests|activeReads|pendingLastUses|readRequestsCreated|"
    r"capturedViewsCreated|capturedViewReleases)\b[^;\n]*;",
    re.MULTILINE,
)
PUBLICATION_COORDINATOR_PRIVATE_STATE_RE = re.compile(
    r"^\s*(?:mutable\s+)?(?:QHash|QSet|QVector|quint64|"
    r"KisPageStorePublicationStatistics)\b[^;\n]*\b"
    r"(preparingCommits|preparedProofs|synchronousHostCompletions|"
    r"preparedSurfaceChanges|stagedPageRemovals|defaultRevisionHighWater|"
    r"descriptors|descriptorRevision|publicationStats|"
    r"committedTransactions|fusedIdleHistoryTransactions)\b[^;\n]*;",
    re.MULTILINE,
)
TEST_METHOD_RE = re.compile(
    r"\b(?:void|bool)\s+([A-Za-z_]\w*)::\s*([A-Za-z_]\w*)\s*\(",
    re.MULTILINE,
)

VALID_CATEGORIES = {
    "canonical",
    "misplaced",
    "duplicate",
    "compatibility-intrusion",
    "test-only",
}
VALID_STATUSES = {
    "inventoried",
    "delegated",
    "callers-migrated",
    "quarantined",
    "removable",
    "tombstone",
}
VALID_OWNERSHIP_STATUSES = {"canonical", "split", "misplaced"}
VALID_CENSUS_STATUSES = {
    "instrumented", "existing", "static-only", "static-proven", "deferred"
}
CENSUS_DIMENSIONS = ("operation", "page", "live", "peak", "terminal", "fallback")


class AuditFailure(Exception):
    pass


def display_path(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPOSITORY_ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def run_git(arguments: list[str]) -> str:
    result = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), *arguments],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def code_only(text: str) -> str:
    """Remove comments and literals for conservative source-token scans."""
    token = re.compile(
        r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
        re.DOTALL,
    )
    return token.sub(lambda match: "\n" * match.group(0).count("\n"), text)


def tile_pagestore_fields(text: str) -> set[str]:
    return {
        field for field in FIELD_RE.findall(code_only(text))
        if field.startswith("m_pageStore")
    }


def load_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as source:
            value = json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise AuditFailure(f"cannot read {display_path(path)}: {error}") from error
    if not isinstance(value, dict):
        raise AuditFailure(f"{display_path(path)} must contain a JSON object")
    return value


def is_inventory_path(path: str) -> bool:
    candidate = Path(path)
    return (
        (candidate.suffix in SOURCE_SUFFIXES
         or candidate.name in BUILD_SOURCE_NAMES
         or candidate.suffix in BUILD_SOURCE_SUFFIXES)
        and any(path == root or path.startswith(root + "/") for root in INVENTORY_ROOTS)
        and not any(part in EXCLUDED_PARTS for part in candidate.parts)
    )


def is_production_source_path(path: str) -> bool:
    return (
        is_inventory_path(path)
        and Path(path).suffix in SOURCE_SUFFIXES
        and "/tests/" not in f"/{path}/"
    )


def is_test_path(path: str) -> bool:
    return "/tests/" in f"/{path}/"


def production_source_paths() -> list[str]:
    paths: list[str] = []
    for root in INVENTORY_ROOTS:
        root_path = REPOSITORY_ROOT / root
        if not root_path.exists():
            continue
        for path in root_path.rglob("*"):
            if path.is_file() and is_production_source_path(display_path(path)):
                paths.append(display_path(path))
    return sorted(set(paths))


def test_method_names() -> set[str]:
    methods: set[str] = set()
    for root in INVENTORY_ROOTS:
        root_path = REPOSITORY_ROOT / root
        if not root_path.exists():
            continue
        for path in root_path.rglob("*"):
            path_name = display_path(path)
            if (path.is_file() and path.suffix in SOURCE_SUFFIXES
                    and "/tests/" in f"/{path_name}/"):
                text = path.read_text(encoding="utf-8", errors="replace")
                methods.update(
                    f"{owner}::{method}"
                    for owner, method in TEST_METHOD_RE.findall(code_only(text))
                )
    return methods


def validate_test_references(
    references: Any, context: str, known_methods: set[str], errors: list[str]
) -> None:
    if not isinstance(references, list):
        return
    for reference in references:
        if not isinstance(reference, str) or "::" not in reference:
            errors.append(f"{context}: invalid test reference {reference!r}")
            continue
        if reference not in known_methods:
            errors.append(f"{context}: test method does not exist: {reference}")


def pagestore_external_usage() -> dict[str, dict[str, Any]]:
    usage: dict[str, dict[str, Any]] = {}
    for path_name in production_source_paths():
        if path_name.startswith("libs/image/pagestore/"):
            continue
        path = REPOSITORY_ROOT / path_name
        text = path.read_text(encoding="utf-8", errors="replace")
        identifiers = PAGESTORE_IDENTIFIER_RE.findall(code_only(text))
        if not identifiers:
            continue
        direct_includes = [
            include for include in INCLUDE_RE.findall(text)
            if "PageStore" in include or "pagestore/" in include
        ]
        usage[path_name] = {
            "identifier_occurrences": len(identifiers),
            "direct_includes": len(direct_includes),
            "symbols": sorted(set(identifiers)),
        }
    return usage


def changed_source_sets(baseline_commit: str) -> tuple[list[str], list[str]]:
    tracked = {
        line.strip()
        for line in run_git(["diff", "--name-only", baseline_commit, "--"]).splitlines()
        if line.strip() and is_inventory_path(line.strip())
    }
    untracked = {
        line.strip()
        for line in run_git(["ls-files", "--others", "--exclude-standard"]).splitlines()
        if line.strip() and is_inventory_path(line.strip())
    }
    return sorted(tracked), sorted(untracked)


def inspect_source(path_name: str) -> dict[str, Any]:
    path = REPOSITORY_ROOT / path_name
    text = path.read_text(encoding="utf-8", errors="replace")
    code = code_only(text)
    return {
        "sha256": sha256(path),
        "lines": len(text.splitlines()),
        "public_types": sorted(set(TYPE_RE.findall(code))),
        "member_fields": sorted(set(FIELD_RE.findall(code))),
        "container_declarations": len(CONTAINER_RE.findall(code)),
        "synchronization_declarations": len(SYNC_RE.findall(code)),
        "include_edges": sorted(set(INCLUDE_RE.findall(text))),
        "pagestore_identifiers": sorted(set(PAGESTORE_IDENTIFIER_RE.findall(code))),
    }


def optional_file_record(path: Path) -> dict[str, Any] | None:
    if not path.is_file():
        return None
    return {"path": display_path(path), "sha256": sha256(path)}


def build_baseline(manifest: dict[str, Any], build_dir: Path) -> dict[str, Any]:
    baseline_commit = manifest.get("baseline_commit")
    if not isinstance(baseline_commit, str) or not baseline_commit:
        raise AuditFailure("intrusion manifest has no baseline_commit")
    try:
        resolved = run_git(["rev-parse", baseline_commit]).strip()
    except subprocess.CalledProcessError as error:
        raise AuditFailure(f"cannot resolve baseline commit {baseline_commit}") from error

    tracked, untracked = changed_source_sets(baseline_commit)
    dirty_union = sorted(set(tracked) | set(untracked))
    records = {
        path_name: inspect_source(path_name)
        for path_name in dirty_union
        if (REPOSITORY_ROOT / path_name).is_file()
    }
    return {
        "schema": 1,
        "milestone": "BR1-R3-M5B",
        "baseline_commit": resolved,
        "working_tree_head": run_git(["rev-parse", "HEAD"]).strip(),
        "compile_commands": optional_file_record(build_dir / "compile_commands.json"),
        "cmake_cache": optional_file_record(build_dir / "CMakeCache.txt"),
        "tracked_diff_sources": tracked,
        "untracked_sources": untracked,
        "dirty_source_union": records,
        "ratchet": {
            "external_pagestore_usage": pagestore_external_usage(),
            "tile_pagestore_fields": sorted(tile_pagestore_fields(
                (REPOSITORY_ROOT / "libs/image/tiles3/kis_tile.h").read_text(
                    encoding="utf-8", errors="replace"
                )
            )),
            "backend_duplicate_state": sorted(set(BACKEND_DUPLICATE_STATE_RE.findall(
                (REPOSITORY_ROOT / "libs/image/pagestore/KisTiledDataManagerPageStoreBackend.cpp").read_text(
                    encoding="utf-8", errors="replace"
                )
            ))),
        },
    }


def require_string(entry: dict[str, Any], field: str, context: str, errors: list[str]) -> None:
    if not isinstance(entry.get(field), str) or not entry[field].strip():
        errors.append(f"{context}: {field} must be a non-empty string")


def require_string_list(entry: dict[str, Any], field: str, context: str, errors: list[str]) -> None:
    value = entry.get(field)
    if not isinstance(value, list) or not value or any(not isinstance(item, str) or not item for item in value):
        errors.append(f"{context}: {field} must be a non-empty string list")


def validate_manifest(manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    entries = manifest.get("entries")
    if manifest.get("schema") != 1:
        errors.append("intrusion manifest: schema must be 1")
    if manifest.get("milestone") != "BR1-R3-M5B":
        errors.append("intrusion manifest: milestone must be BR1-R3-M5B")
    if manifest.get("containment_status") != "r0-complete-no-d0":
        errors.append("intrusion manifest: R0 containment status is not closed")
    if manifest.get("restructuring_status") != "r1-complete":
        errors.append("intrusion manifest: R1 service extraction is not closed")
    if manifest.get("metadata_status") != "r2-m4-allocation-free-publication":
        errors.append("intrusion manifest: R2/M4 publication status is stale")
    if manifest.get("write_status") != "r3-m5b-complete":
        errors.append("intrusion manifest: R3/M5B write status is stale")
    reviewed_ceilings = manifest.get("reviewed_ratchet_ceiling")
    if not isinstance(reviewed_ceilings, dict):
        errors.append("intrusion manifest: reviewed_ratchet_ceiling must be an object")
    else:
        for path_name, ceiling in sorted(reviewed_ceilings.items()):
            context = f"reviewed intrusion ceiling {path_name}"
            if not isinstance(path_name, str) or Path(path_name).is_absolute() or ".." in Path(path_name).parts:
                errors.append(f"{context}: path must be repository-relative")
                continue
            if not isinstance(ceiling, dict):
                errors.append(f"{context}: ceiling must be an object")
                continue
            for field in ("identifier_occurrences", "direct_includes"):
                if not isinstance(ceiling.get(field), int) or ceiling[field] < 0:
                    errors.append(f"{context}: {field} must be a non-negative integer")
            require_string(ceiling, "reason", context, errors)
    if not isinstance(entries, list) or not entries:
        return errors + ["intrusion manifest: entries must be a non-empty list"]

    seen_ids: set[str] = set()
    known_tests = test_method_names()
    required_strings = (
        "id", "path", "category", "current-owner", "target-owner",
        "replacement-type", "runtime-counter", "remove-after", "status",
    )
    required_lists = ("symbols", "facts-owned", "static-callers", "failure-tests")
    for index, raw_entry in enumerate(entries):
        context = f"intrusion manifest entry {index}"
        if not isinstance(raw_entry, dict):
            errors.append(f"{context}: must be an object")
            continue
        entry = raw_entry
        for field in required_strings:
            require_string(entry, field, context, errors)
        for field in required_lists:
            require_string_list(entry, field, context, errors)
        entry_id = entry.get("id")
        if isinstance(entry_id, str):
            if entry_id in seen_ids:
                errors.append(f"{context}: duplicate id {entry_id}")
            seen_ids.add(entry_id)
            context = entry_id
        if entry.get("category") not in VALID_CATEGORIES:
            errors.append(f"{context}: invalid category {entry.get('category')!r}")
        if entry.get("status") not in VALID_STATUSES:
            errors.append(f"{context}: invalid status {entry.get('status')!r}")
        validate_test_references(entry.get("failure-tests"), context, known_tests, errors)

        path_name = entry.get("path")
        if not isinstance(path_name, str):
            continue
        if Path(path_name).is_absolute() or ".." in Path(path_name).parts:
            errors.append(f"{context}: path must be repository-relative")
            continue
        path = REPOSITORY_ROOT / path_name
        tombstone = entry.get("status") == "tombstone"
        if not path.is_file():
            if not tombstone:
                errors.append(f"{context}: path does not exist: {path_name}")
            continue
        if tombstone:
            errors.append(f"{context}: tombstone path still exists: {path_name}")
            continue
        text = code_only(path.read_text(encoding="utf-8", errors="replace"))
        for symbol in entry.get("symbols", []):
            if isinstance(symbol, str) and not re.search(rf"\b{re.escape(symbol)}\b", text):
                errors.append(f"{context}: symbol is missing from {path_name}: {symbol}")
    return errors


def validate_ownership(ownership: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    facts = ownership.get("facts")
    if ownership.get("schema") != 1:
        errors.append("state ownership: schema must be 1")
    if ownership.get("milestone") != "BR1-R3-M5B":
        errors.append("state ownership: milestone must be BR1-R3-M5B")
    if not isinstance(facts, list) or not facts:
        return errors + ["state ownership: facts must be a non-empty list"]
    seen_ids: set[str] = set()
    for index, raw_fact in enumerate(facts):
        context = f"state ownership fact {index}"
        if not isinstance(raw_fact, dict):
            errors.append(f"{context}: must be an object")
            continue
        fact = raw_fact
        for field in ("id", "fact", "target-authoritative-owner", "status", "lock-or-claim"):
            require_string(fact, field, context, errors)
        for field in ("current-writers", "read-only-observers", "creation-paths", "terminal-paths"):
            require_string_list(fact, field, context, errors)
        fact_id = fact.get("id")
        if isinstance(fact_id, str):
            if fact_id in seen_ids:
                errors.append(f"{context}: duplicate id {fact_id}")
            seen_ids.add(fact_id)
            context = fact_id
        if fact.get("status") not in VALID_OWNERSHIP_STATUSES:
            errors.append(f"{context}: invalid status {fact.get('status')!r}")
        writers = fact.get("current-writers", [])
        if fact.get("status") == "canonical" and isinstance(writers, list) and len(writers) != 1:
            errors.append(f"{context}: canonical fact must have exactly one current writer")
    return errors


def validate_runtime_census(census: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    coverage = census.get("coverage")
    if census.get("schema") != 1:
        errors.append("runtime census: schema must be 1")
    if census.get("milestone") != "BR1-R3-M5B":
        errors.append("runtime census: milestone must be BR1-R3-M5B")
    if not isinstance(coverage, list) or not coverage:
        return errors + ["runtime census: coverage must be a non-empty list"]

    manifest_ids = {
        entry.get("id")
        for entry in manifest.get("entries", [])
        if isinstance(entry, dict) and isinstance(entry.get("id"), str)
    }
    covered_manifest_ids: set[str] = set()
    seen_ids: set[str] = set()
    known_tests = test_method_names()
    for index, raw_entry in enumerate(coverage):
        context = f"runtime census entry {index}"
        if not isinstance(raw_entry, dict):
            errors.append(f"{context}: must be an object")
            continue
        entry = raw_entry
        for field in ("id", "status", "metrics-source", "evidence"):
            require_string(entry, field, context, errors)
        require_string_list(entry, "manifest-entries", context, errors)

        entry_id = entry.get("id")
        if isinstance(entry_id, str):
            if entry_id in seen_ids:
                errors.append(f"{context}: duplicate id {entry_id}")
            seen_ids.add(entry_id)
            context = entry_id
        status = entry.get("status")
        if status not in VALID_CENSUS_STATUSES:
            errors.append(f"{context}: invalid status {status!r}")

        references = entry.get("manifest-entries", [])
        if isinstance(references, list):
            for manifest_id in references:
                if manifest_id not in manifest_ids:
                    errors.append(f"{context}: unknown manifest entry {manifest_id}")
                elif manifest_id in covered_manifest_ids:
                    errors.append(f"{context}: manifest entry covered more than once: {manifest_id}")
                else:
                    covered_manifest_ids.add(manifest_id)

        dimensions = entry.get("dimensions")
        if not isinstance(dimensions, dict):
            errors.append(f"{context}: dimensions must be an object")
            dimensions = {}
        for dimension in CENSUS_DIMENSIONS:
            values = dimensions.get(dimension)
            if not isinstance(values, list) or any(
                not isinstance(value, str) or not value for value in values
            ):
                errors.append(f"{context}: dimensions.{dimension} must be a string list")

        tests = entry.get("representative-tests")
        if status in {"instrumented", "existing"}:
            require_string_list(entry, "representative-tests", context, errors)
            counter_count = sum(
                len(dimensions.get(dimension, []))
                for dimension in CENSUS_DIMENSIONS
                if isinstance(dimensions.get(dimension), list)
            )
            if counter_count == 0:
                errors.append(f"{context}: runtime-covered entry has no counters")
        elif not isinstance(tests, list) or any(not isinstance(test, str) or not test for test in tests):
            errors.append(f"{context}: representative-tests must be a string list")
        validate_test_references(tests, context, known_tests, errors)

    missing = sorted(manifest_ids - covered_manifest_ids)
    for manifest_id in missing:
        errors.append(f"runtime census does not cover manifest entry: {manifest_id}")
    return errors


def validate_callgraph(
    callgraph: dict[str, Any],
    manifest: dict[str, Any],
    ownership: dict[str, Any],
    census: dict[str, Any],
    baseline: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    # Use the generator's one implementation; never accept edited root paths
    # or a test-only cycle promoted to production by a stale review artifact.
    spec = importlib.util.spec_from_file_location(
        "pagestore_callgraph", SCRIPT_PATH.with_name("generate-pagestore-callgraph.py"))
    generator = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = generator
    spec.loader.exec_module(generator)
    try:
        review = generator.reachability_review(callgraph.get("apis", []))
        if callgraph.get("reachability-review") != review:
            errors.append("reverse call graph: missing or inconsistent reachability review; regenerate")
    except (KeyError, TypeError, ValueError, AttributeError):
        errors.append("reverse call graph: malformed reachability inputs")
    fingerprints = callgraph.get("source-fingerprints", {})
    current_paths = {
        display_path(path) for root in INVENTORY_ROOTS
        for path in (REPOSITORY_ROOT / root).rglob("*")
        if path.is_file() and is_inventory_path(display_path(path))
    }
    if not isinstance(fingerprints, dict) or set(fingerprints) != current_paths:
        errors.append("reverse call graph: source inventory changed or missing; regenerate graph")
    if isinstance(fingerprints, dict):
        for path_name in sorted(current_paths & fingerprints.keys()):
            if sha256(REPOSITORY_ROOT / path_name) != fingerprints[path_name]:
                errors.append(f"reverse call graph: stale source {path_name}; regenerate graph")
    inputs = callgraph.get("audit-input-fingerprints")
    if not isinstance(inputs, dict) or not inputs:
        errors.append("reverse call graph: audit input fingerprints are missing")
    else:
        for path_name, digest in inputs.items():
            path = REPOSITORY_ROOT / path_name
            if not path.is_file() or sha256(path) != digest:
                errors.append(f"reverse call graph: stale input {path_name}; regenerate graph")
    if callgraph.get("schema") != 1:
        errors.append("reverse call graph: schema must be 1")
    if callgraph.get("milestone") != "BR1-R3-M5B":
        errors.append("reverse call graph: milestone must be BR1-R3-M5B")

    compile_commands = callgraph.get("compile-commands")
    frozen_compile_commands = baseline.get("compile_commands")
    if not isinstance(compile_commands, dict):
        errors.append("reverse call graph: compile-commands must be an object")
    elif not isinstance(frozen_compile_commands, dict):
        errors.append("reverse call graph: baseline has no compile_commands provenance")
    else:
        for field in ("path", "sha256"):
            if compile_commands.get(field) != frozen_compile_commands.get(field):
                errors.append(
                    f"reverse call graph: compile-commands {field} does not match baseline"
                )

    units = callgraph.get("indexed-translation-units")
    if not isinstance(units, list) or not units:
        errors.append("reverse call graph: indexed-translation-units must be non-empty")
    else:
        seen_units: set[str] = set()
        for index, unit in enumerate(units):
            context = f"reverse call graph translation unit {index}"
            if not isinstance(unit, dict):
                errors.append(f"{context}: must be an object")
                continue
            path_name = unit.get("path")
            if not isinstance(path_name, str) or not is_inventory_path(path_name):
                errors.append(f"{context}: invalid inventory path {path_name!r}")
            elif path_name in seen_units:
                errors.append(f"{context}: duplicate path {path_name}")
            else:
                seen_units.add(path_name)
            if unit.get("scope") not in {"production", "test"}:
                errors.append(f"{context}: invalid scope {unit.get('scope')!r}")
            require_string(unit, "reason", context, errors)

    apis = callgraph.get("apis")
    api_usrs: set[str] = set()
    calculated_calls = 0
    calculated_callbacks = 0
    if not isinstance(apis, list) or not apis:
        errors.append("reverse call graph: apis must be a non-empty list")
        apis = []
    for index, api in enumerate(apis):
        context = f"reverse call graph API {index}"
        if not isinstance(api, dict):
            errors.append(f"{context}: must be an object")
            continue
        for field in ("usr", "name", "qualified-name", "kind"):
            require_string(api, field, context, errors)
        usr = api.get("usr")
        if isinstance(usr, str):
            if usr in api_usrs:
                errors.append(f"{context}: duplicate USR {usr}")
            api_usrs.add(usr)
        declaration_count = 0
        for field in ("declarations", "definitions"):
            locations = api.get(field)
            if not isinstance(locations, list):
                errors.append(f"{context}: {field} must be a list")
                continue
            declaration_count += len(locations)
            for value in locations:
                errors.extend(validate_callgraph_location(value, f"{context}.{field}"))
        if declaration_count == 0:
            errors.append(f"{context}: API has neither a declaration nor definition")

        for field in ("production-callers", "test-callers"):
            callers = api.get(field)
            if not isinstance(callers, list):
                errors.append(f"{context}: {field} must be a list")
                continue
            calculated_calls += len(callers)
            for caller in callers:
                errors.extend(validate_callgraph_location(caller, f"{context}.{field}"))
                if isinstance(caller, dict):
                    require_string(caller, "caller", f"{context}.{field}", errors)
                    path_name = caller.get("path", "")
                    if field == "test-callers" and isinstance(path_name, str) and not is_test_path(path_name):
                        errors.append(f"{context}: test caller is outside a test path: {path_name}")
                    if field == "production-callers" and isinstance(path_name, str) and is_test_path(path_name):
                        errors.append(f"{context}: production caller is under a test path: {path_name}")

        callbacks = api.get("callback-registrations")
        if not isinstance(callbacks, list):
            errors.append(f"{context}: callback-registrations must be a list")
        else:
            calculated_callbacks += len(callbacks)
            for callback in callbacks:
                errors.extend(validate_callgraph_location(callback, f"{context}.callback"))
                containers = callback.get("containers") if isinstance(callback, dict) else None
                if not isinstance(containers, list) or not containers:
                    errors.append(f"{context}: callback registration has no resolved container")

    if callgraph.get("api-count") != len(apis):
        errors.append("reverse call graph: api-count does not match apis")
    if callgraph.get("call-edge-count") != calculated_calls:
        errors.append("reverse call graph: call-edge-count does not match API edges")
    if callgraph.get("callback-registration-count") != calculated_callbacks:
        errors.append(
            "reverse call graph: callback-registration-count does not match API registrations"
        )
    unresolved = callgraph.get("unresolved-edges")
    if not isinstance(unresolved, list):
        errors.append("reverse call graph: unresolved-edges must be a list")
    elif unresolved:
        errors.append(
            f"reverse call graph: {len(unresolved)} unresolved call/callback edges remain"
        )

    manifest_ids = {
        entry.get("id")
        for entry in manifest.get("entries", [])
        if isinstance(entry, dict) and isinstance(entry.get("id"), str)
    }
    deferred_manifest_ids = {
        manifest_id
        for item in census.get("coverage", [])
        if isinstance(item, dict) and item.get("status") == "deferred"
        for manifest_id in item.get("manifest-entries", [])
        if isinstance(manifest_id, str)
    }
    bindings = callgraph.get("manifest-bindings")
    bound_ids: set[str] = set()
    if not isinstance(bindings, list):
        errors.append("reverse call graph: manifest-bindings must be a list")
    else:
        for index, binding in enumerate(bindings):
            context = f"reverse call graph manifest binding {index}"
            if not isinstance(binding, dict):
                errors.append(f"{context}: must be an object")
                continue
            binding_id = binding.get("id")
            if binding_id not in manifest_ids:
                errors.append(f"{context}: unknown manifest entry {binding_id!r}")
                continue
            if binding_id in bound_ids:
                errors.append(f"{context}: duplicate manifest entry {binding_id}")
            bound_ids.add(binding_id)
            indexed = binding.get("source-record-indexed")
            coverage = binding.get("coverage")
            if indexed is True and coverage != "indexed":
                errors.append(f"{binding_id}: indexed source has invalid coverage {coverage!r}")
            if indexed is not True:
                if binding_id not in deferred_manifest_ids:
                    errors.append(f"{binding_id}: non-deferred manifest source was not indexed")
                if coverage != "deferred-outside-cpu-compile-database":
                    errors.append(f"{binding_id}: invalid deferred graph coverage {coverage!r}")
            declared = binding.get("declared-api-usrs")
            if not isinstance(declared, list) or any(usr not in api_usrs for usr in declared):
                errors.append(f"{binding_id}: declared-api-usrs contains an unknown USR")
    for manifest_id in sorted(manifest_ids - bound_ids):
        errors.append(f"reverse call graph does not bind manifest entry: {manifest_id}")

    physical = callgraph.get("physical-exclusive-gate")
    if not isinstance(physical, dict):
        errors.append("reverse call graph: physical-exclusive-gate must be an object")
    else:
        physical_apis = [
            api for api in apis
            if isinstance(api, dict) and api.get("usr") in physical.get("api-usrs", [])
        ]
        production_count = sum(len(api.get("production-callers", [])) for api in physical_apis)
        test_count = sum(len(api.get("test-callers", [])) for api in physical_apis)
        if not physical_apis:
            errors.append("reverse call graph: physical gate API is missing")
        if physical.get("production-call-site-count") != production_count:
            errors.append("reverse call graph: physical production caller count is inconsistent")
        if physical.get("test-call-site-count") != test_count:
            errors.append("reverse call graph: physical test caller count is inconsistent")
        expected_proof = "production-callers-bound" if production_count else "production-caller-missing"
        if physical.get("static-proof") != expected_proof:
            errors.append("physical reservation caller disposition is stale")
        if production_count == 0:
            errors.append("fresh native write reservation has no indexed production caller")
        if physical.get("d0-eligible") is not False:
            errors.append("physical gate must not be D0 eligible while ownership is split")
        if physical.get("ownership-fact") != "BR1-OWN-PHYSICAL-ALLOCATION":
            errors.append("physical gate is not bound to its ownership fact")
        ownership_fact = next(
            (
                fact for fact in ownership.get("facts", [])
                if isinstance(fact, dict) and fact.get("id") == physical.get("ownership-fact")
            ),
            None,
        )
        if not isinstance(ownership_fact, dict):
            errors.append("physical gate ownership fact is missing")
        elif physical.get("ownership-status") != ownership_fact.get("status"):
            errors.append("physical gate ownership status is stale")
        physical_manifest = next(
            (
                entry for entry in manifest.get("entries", [])
                if isinstance(entry, dict) and entry.get("id") == "BR1-PS-PHYSICAL-GATE-015"
            ),
            None,
        )
        if not isinstance(physical_manifest, dict) or physical_manifest.get("status") != "delegated":
            errors.append("physical reservation manifest disposition is stale")
        physical_runtime = next(
            (
                item for item in census.get("coverage", [])
                if isinstance(item, dict) and item.get("id") == "physical-exclusive-gate"
            ),
            None,
        )
        if not isinstance(physical_runtime, dict) or physical_runtime.get("status") != "existing":
            errors.append("physical reservation runtime disposition must be existing")
    return errors


def validate_callgraph_location(value: Any, context: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(value, dict):
        return [f"{context}: location must be an object"]
    path_name = value.get("path")
    line = value.get("line")
    column = value.get("column")
    if not isinstance(path_name, str) or not is_inventory_path(path_name):
        errors.append(f"{context}: invalid inventory path {path_name!r}")
    else:
        path = REPOSITORY_ROOT / path_name
        if not path.is_file():
            errors.append(f"{context}: source path does not exist: {path_name}")
        elif not isinstance(line, int) or line < 1 or line > source_line_count(path):
            errors.append(f"{context}: invalid line {line!r} for {path_name}")
    if not isinstance(column, int) or column < 1:
        errors.append(f"{context}: invalid column {column!r}")
    return errors


@functools.lru_cache(maxsize=None)
def source_line_count(path: Path) -> int:
    return len(path.read_text(encoding="utf-8", errors="replace").splitlines())


def validate_ratchet(baseline: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    ratchet = baseline.get("ratchet")
    if not isinstance(ratchet, dict):
        return ["baseline source inventory has no ratchet object"]

    frozen_usage = ratchet.get("external_pagestore_usage")
    if not isinstance(frozen_usage, dict):
        errors.append("baseline ratchet has no external_pagestore_usage")
    else:
        current_usage = pagestore_external_usage()
        new_files = sorted(set(current_usage) - set(frozen_usage))
        for path_name in new_files:
            errors.append(f"new PageStore intrusion file is not allowlisted: {path_name}")
        for path_name, current in sorted(current_usage.items()):
            frozen = frozen_usage.get(path_name)
            if not isinstance(frozen, dict):
                continue
            for field in ("identifier_occurrences", "direct_includes"):
                current_count = current.get(field, 0)
                frozen_count = frozen.get(field, 0)
                reviewed = manifest.get("reviewed_ratchet_ceiling", {}).get(path_name, {})
                reviewed_count = reviewed.get(field) if isinstance(reviewed, dict) else None
                allowed_count = max(frozen_count, reviewed_count) \
                    if isinstance(frozen_count, int) and isinstance(reviewed_count, int) \
                    else frozen_count
                if not isinstance(allowed_count, int) or current_count > allowed_count:
                    errors.append(
                        f"PageStore intrusion grew in {path_name}: {field} "
                        f"{current_count} > {allowed_count}"
                    )

    tile_path = REPOSITORY_ROOT / "libs/image/tiles3/kis_tile.h"
    current_tile_fields = tile_pagestore_fields(
        tile_path.read_text(encoding="utf-8", errors="replace")
    )
    frozen_tile_fields = set(ratchet.get("tile_pagestore_fields", []))
    for field in sorted(current_tile_fields - frozen_tile_fields):
        errors.append(f"new KisTile PageStore compatibility field: {field}")

    backend_path = REPOSITORY_ROOT / "libs/image/pagestore/KisTiledDataManagerPageStoreBackend.cpp"
    current_backend_state = set(BACKEND_DUPLICATE_STATE_RE.findall(
        backend_path.read_text(encoding="utf-8", errors="replace")
    ))
    frozen_backend_state = set(ratchet.get("backend_duplicate_state", []))
    for symbol in sorted(current_backend_state - frozen_backend_state):
        errors.append(f"new backend duplicate writer/admission state: {symbol}")

    manifest_symbols = {
        symbol
        for entry in manifest.get("entries", [])
        if isinstance(entry, dict)
        for symbol in entry.get("symbols", [])
        if isinstance(symbol, str)
    }
    for symbol in sorted(current_tile_fields | current_backend_state):
        if symbol not in manifest_symbols:
            errors.append(f"ratcheted compatibility symbol is absent from manifest: {symbol}")

    allowed_physical_gate_files = {
        "libs/image/pagestore/KisCpuResidentBinding.cpp",
        "libs/image/pagestore/KisCpuResidentBinding_p.h",
    }
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(encoding="utf-8", errors="replace"))
        for symbol in sorted(set(RETIRED_WRITE_PROTOCOL_RE.findall(text))):
            errors.append(f"retired write protocol returned in {path_name}: {symbol}")
        if (
            path_name not in allowed_physical_gate_files
            and re.search(r"\btryAcquireExclusiveWrite\s*\(", text)
        ):
            errors.append(
                f"direct physical exclusivity authorization escaped quarantine: {path_name}"
            )
        if (
            path_name.startswith("libs/image/pagestore/")
            and path_name not in allowed_physical_gate_files
            and re.search(r"\b(?:m_usersCount|usersCount|swapLock)\b", text)
        ):
            errors.append(f"TileData count/swap-lock authorization escaped provider gate: {path_name}")

    default_storage_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageDefaultStorage_p.h"
    )
    default_storage_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageDefaultStorage.cpp"
    )
    page_store_source = REPOSITORY_ROOT / "libs/image/pagestore/KisPageStore.cpp"
    for path in (default_storage_header, default_storage_source):
        if not path.is_file():
            errors.append(f"R1 DefaultStorage service is missing: {display_path(path)}")
    if default_storage_header.is_file():
        header_text = code_only(default_storage_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        required_contracts = (
            "class KRITAIMAGE_EXPORT KisPageDefaultStorage final",
            "ReadCacheEntryBudget = 64",
            "ReadCacheByteBudget = 8 * 1024 * 1024",
            "PreparationEntryBudget = 64",
        )
        for contract in required_contracts:
            if contract not in header_text:
                errors.append(f"R1 DefaultStorage contract is missing: {contract}")
        if "QObject" in header_text:
            errors.append("R1 DefaultStorage must not inherit or own QObject behavior")
    if page_store_source.is_file():
        store_text = code_only(page_store_source.read_text(
            encoding="utf-8", errors="replace"
        ))
        leaked_state = sorted(set(DEFAULT_STORAGE_PRIVATE_STATE_RE.findall(store_text)))
        for symbol in leaked_state:
            errors.append(f"default storage state returned to KisPageStore::Private: {symbol}")
        if not re.search(r"\bKisPageDefaultStorage\s+defaultStorage\s*;", store_text):
            errors.append("KisPageStore::Private must embed KisPageDefaultStorage by value")
    owning_default_service = re.compile(
        r"(?:new\s+KisPageDefaultStorage\b|"
        r"Q(?:Shared|Scoped)Pointer\s*<\s*KisPageDefaultStorage\s*>|"
        r"std::(?:shared_ptr|unique_ptr)\s*<\s*KisPageDefaultStorage\s*>)"
    )
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(
            encoding="utf-8", errors="replace"
        ))
        if owning_default_service.search(text):
            errors.append(f"KisPageDefaultStorage must not become a heap service: {path_name}")

    retirement_queue_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageRetirementQueue_p.h"
    )
    retirement_queue_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageRetirementQueue.cpp"
    )
    for path in (retirement_queue_header, retirement_queue_source):
        if not path.is_file():
            errors.append(f"R1 RetirementQueue service is missing: {display_path(path)}")
    if retirement_queue_header.is_file():
        header_text = code_only(retirement_queue_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        required_contracts = (
            "class KisPageRetirementQueue final",
            "WorkerBatchBudget = 32",
            "void beginClose()",
            "QVector<KisPageRetirementRecord> takeForClose()",
        )
        for contract in required_contracts:
            if contract not in header_text:
                errors.append(f"R1 RetirementQueue contract is missing: {contract}")
        if "QObject" in header_text:
            errors.append("R1 RetirementQueue must not inherit or own QObject behavior")
    if page_store_source.is_file():
        store_text = code_only(page_store_source.read_text(
            encoding="utf-8", errors="replace"
        ))
        leaked_state = sorted(set(RETIREMENT_QUEUE_PRIVATE_STATE_RE.findall(store_text)))
        for symbol in leaked_state:
            errors.append(f"retirement queue state returned to KisPageStore::Private: {symbol}")
        if not re.search(r"\bKisPageRetirementQueue\s+retirementQueue\s*;", store_text):
            errors.append("KisPageStore::Private must embed KisPageRetirementQueue by value")
    owning_retirement_service = re.compile(
        r"(?:new\s+KisPageRetirementQueue\b|"
        r"Q(?:Shared|Scoped)Pointer\s*<\s*KisPageRetirementQueue\s*>|"
        r"std::(?:shared_ptr|unique_ptr)\s*<\s*KisPageRetirementQueue\s*>)"
    )
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(
            encoding="utf-8", errors="replace"
        ))
        if owning_retirement_service.search(text):
            errors.append(f"KisPageRetirementQueue must not become a heap service: {path_name}")

    history_collector_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageHistoryCollector_p.h"
    )
    history_collector_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageHistoryCollector.cpp"
    )
    for path in (history_collector_header, history_collector_source):
        if not path.is_file():
            errors.append(f"R1 HistoryCollector service is missing: {display_path(path)}")
    if history_collector_header.is_file():
        header_text = code_only(history_collector_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        required_contracts = (
            "class KisPageHistoryCollector final",
            "PageAdmissionBudget = 16",
            "VersionScanBudget = 32",
            "collectUnreachableLocked",
        )
        for contract in required_contracts:
            if contract not in header_text:
                errors.append(f"R1 HistoryCollector contract is missing: {contract}")
        if "QObject" in header_text:
            errors.append("R1 HistoryCollector must not inherit or own QObject behavior")
    if page_store_source.is_file():
        leaked_state = sorted(set(HISTORY_COLLECTOR_PRIVATE_STATE_RE.findall(store_text)))
        for symbol in leaked_state:
            errors.append(f"history collector state returned to KisPageStore::Private: {symbol}")
        if not re.search(r"\bKisPageHistoryCollector\s+historyCollector\s*;", store_text):
            errors.append("KisPageStore::Private must embed KisPageHistoryCollector by value")
    owning_history_service = re.compile(
        r"(?:new\s+KisPageHistoryCollector\b|"
        r"Q(?:Shared|Scoped)Pointer\s*<\s*KisPageHistoryCollector\s*>|"
        r"std::(?:shared_ptr|unique_ptr)\s*<\s*KisPageHistoryCollector\s*>)"
    )
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(
            encoding="utf-8", errors="replace"
        ))
        if owning_history_service.search(text):
            errors.append(f"KisPageHistoryCollector must not become a heap service: {path_name}")

    read_coordinator_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageReadCoordinator_p.h"
    )
    read_coordinator_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageReadCoordinator.cpp"
    )
    for path in (read_coordinator_header, read_coordinator_source):
        if not path.is_file():
            errors.append(f"R1 ReadCoordinator service is missing: {display_path(path)}")
    if read_coordinator_header.is_file():
        header_text = code_only(read_coordinator_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        required_contracts = (
            "class KisPageReadCoordinator final",
            "registerRequestLocked",
            "resolveLocked",
            "releaseSnapshot",
        )
        for contract in required_contracts:
            if contract not in header_text:
                errors.append(f"R1 ReadCoordinator contract is missing: {contract}")
        if "QObject" in header_text:
            errors.append("R1 ReadCoordinator must not inherit or own QObject behavior")
    if page_store_source.is_file():
        leaked_state = sorted(set(READ_COORDINATOR_PRIVATE_STATE_RE.findall(store_text)))
        for symbol in leaked_state:
            errors.append(f"read coordinator state returned to KisPageStore::Private: {symbol}")
        if not re.search(r"\bKisPageReadCoordinator\s+readCoordinator\s*;", store_text):
            errors.append("KisPageStore::Private must embed KisPageReadCoordinator by value")
    owning_read_service = re.compile(
        r"(?:new\s+KisPageReadCoordinator\b|"
        r"Q(?:Shared|Scoped)Pointer\s*<\s*KisPageReadCoordinator\s*>|"
        r"std::(?:shared_ptr|unique_ptr)\s*<\s*KisPageReadCoordinator\s*>)"
    )
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(
            encoding="utf-8", errors="replace"
        ))
        if owning_read_service.search(text):
            errors.append(f"KisPageReadCoordinator must not become a heap service: {path_name}")

    publication_coordinator_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPagePublicationCoordinator_p.h"
    )
    publication_coordinator_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPagePublicationCoordinator.cpp"
    )
    for path in (publication_coordinator_header, publication_coordinator_source):
        if not path.is_file():
            errors.append(f"R1 PublicationCoordinator service is missing: {display_path(path)}")
    if publication_coordinator_header.is_file():
        header_text = code_only(publication_coordinator_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        required_contracts = (
            "class KisPagePublicationCoordinator final",
            "class KisPreparedMutationCommit final",
            "std::unique_ptr<Data> data",
            "struct PreparedTransactionState",
            "QHash<quint64, PreparedTransactionState> m_preparedTransactions",
            "stageSurfaceMetadataLocked",
            "commitLocked",
            "restoreRetainedEpochLocked",
            "abortLocked",
        )
        for contract in required_contracts:
            if contract not in header_text:
                errors.append(f"R1 PublicationCoordinator contract is missing: {contract}")
        if "QObject" in header_text:
            errors.append("R1 PublicationCoordinator must not inherit or own QObject behavior")
        for forbidden in (
            "m_preparedProofs",
            "m_synchronousHostCompletions",
            "m_preparedSurfaceChanges",
            "m_stagedPageRemovals",
        ):
            if forbidden in header_text:
                errors.append(
                    f"R2/M4 retained parallel publication side map: {forbidden}"
                )
    if publication_coordinator_source.is_file():
        publication_text = code_only(publication_coordinator_source.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "KisPageMetadataCoordinator::PreparedPublication metadata",
            "KisImageEpochReferenceModel::PreparedCommit epoch",
            "KisImageEpochReferenceModel::PreparedRootReservation restoreEpoch",
            "KisPagePublicationCoordinator::KisPreparedMutationCommit::isValid() const",
            "KisPagePublicationCoordinator::KisPreparedMutationCommit::cancel() noexcept",
            "preparedCommit.tryInstall",
        ):
            if contract not in publication_text:
                errors.append(
                    f"R2/M4 prepared mutation commit contract is missing: {contract}"
                )
    if page_store_source.is_file():
        leaked_state = sorted(set(
            PUBLICATION_COORDINATOR_PRIVATE_STATE_RE.findall(store_text)
        ))
        for symbol in leaked_state:
            errors.append(
                f"publication coordinator state returned to KisPageStore::Private: {symbol}"
            )
        if not re.search(
            r"\bKisPagePublicationCoordinator\s+publicationCoordinator\s*;",
            store_text,
        ):
            errors.append(
                "KisPageStore::Private must embed KisPagePublicationCoordinator by value"
            )
    owning_publication_service = re.compile(
        r"(?:new\s+KisPagePublicationCoordinator\b|"
        r"Q(?:Shared|Scoped)Pointer\s*<\s*KisPagePublicationCoordinator\s*>|"
        r"std::(?:shared_ptr|unique_ptr)\s*<\s*KisPagePublicationCoordinator\s*>)"
    )
    for path_name in production_source_paths():
        text = code_only((REPOSITORY_ROOT / path_name).read_text(
            encoding="utf-8", errors="replace"
        ))
        if owning_publication_service.search(text):
            errors.append(
                f"KisPagePublicationCoordinator must not become a heap service: {path_name}"
            )

    metadata_arena_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageMetadataArena_p.h"
    )
    metadata_records_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageMetadataCoordinator_p.h"
    )
    metadata_index_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageMetadataIndex_p.h"
    )
    metadata_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageMetadataCoordinator.cpp"
    )
    for path in (metadata_arena_header, metadata_records_header,
                 metadata_index_header, metadata_source):
        if not path.is_file():
            errors.append(f"R2/M2 metadata arena component is missing: {display_path(path)}")
    if metadata_arena_header.is_file():
        arena_text = code_only(metadata_arena_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "struct KisGenerationalSlotId",
            "using KisVersionSlotId",
            "using KisReplicaSlotId",
            "using KisMetadataOverflowSlotId",
            "sizeof(KisVersionSlotId) == 8",
            "class KisShardSlotArena",
            "using Reservation",
            "class PreparedBlock",
            "class ReleasedBlocks",
            "reserveSlots",
            "emplaceReserved",
            "outstandingReservations",
            "canAttachBlocks",
            "DirectoryState::Quarantined",
            "takeEmptyBlocks",
            "attachedBlocks",
            "releasedBytes",
            "highWaterSlots",
        ):
            if contract not in arena_text:
                errors.append(f"R2/M2 typed arena contract is missing: {contract}")
    if metadata_records_header.is_file():
        records_text = code_only(metadata_records_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "struct KisVersionRecord",
            "struct KisReplicaRecord",
            "struct KisMetadataOverflowNode",
            "sizeof(KisVersionRecord) <= 128",
            "sizeof(KisReplicaRecord) <= 128",
            "sizeof(KisMetadataOverflowNode) <= 64",
        ):
            if contract not in records_text:
                errors.append(f"R2/M2 compact record contract is missing: {contract}")
        if re.search(r"QVector\s*<\s*Kis(?:PageVersion|Replica)StateSnapshot\s*>", records_text):
            errors.append("R2/M2 compact records must not embed public snapshot vectors")
    if metadata_index_header.is_file():
        index_text = code_only(metadata_index_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "class KisShardSlotIndex",
            "using Reservation",
            "reserveInsertions",
            "insertReserved",
            "eraseExact",
            "outstandingReservations",
        ):
            if contract not in index_text:
                errors.append(f"R2/M3 shard slot index contract is missing: {contract}")
        if re.search(r"\bm_entries\s*\.\s*insert\s*\(", index_text) is None:
            errors.append("R2/M3 slot index has no private reserved insertion")
    if metadata_source.is_file():
        metadata_raw = metadata_source.read_text(
            encoding="utf-8", errors="replace"
        )
        metadata_text = code_only(metadata_raw)
        if '#include "KisPageMetadataCoordinator_p.h"' not in metadata_raw:
            errors.append(
                "R2/M2 metadata coordinator does not include its compact record schema"
            )
        for contract in (
            "using VersionArena = KisShardSlotArena<KisVersionRecord, 16 * 1024>",
            "using ReplicaArena = KisShardSlotArena<KisReplicaRecord, 32 * 1024>",
            "using OverflowArena = KisShardSlotArena<KisMetadataOverflowNode, 16 * 1024>",
            "struct MetadataArenas",
            "struct MetadataArenaReservations",
            "MetadataArenas arenas",
            "struct ShardRecordStore",
            "struct ReservationSet",
            "KisShardSlotIndex<KisPageVersion, KisVersionSlotId>",
            "KisShardSlotIndex<PhysicalSlot, KisReplicaSlotId>",
            "KisVersionSlotId firstVersion",
            "KisVersionSlotId firstHistory",
            "KisVersionSlotId firstMutable",
            "struct MetadataPageActivity",
            "preparedPublications",
            "reservePublicationActivity",
            "consumePublicationActivity",
            "static_assert(sizeof(MetadataPage) <= 96)",
            "static_assert(sizeof(MetadataPageActivity) <= 96)",
            "QHash<KisPageKey, MetadataPageActivity> activities",
            "assignHeader",
            "installReservedHeader",
            "projectReplica",
            "metadataPageActivityBytes",
            "KisPageMetadataCoordinator::HistorySlice",
            "linkVersion",
            "linkHistory",
            "linkMutable",
            "arenas->versions.emplace",
            "arenas->replicas.emplace",
            "arenas->overflow.emplace",
            "takeEmptyBlocks",
            "struct MetadataArenaGrowth",
            "growMetadataArenasOutsideLock",
            "metadataArenaBlockCandidatesPrepared",
            "replaceAllReserved",
            "installPreparedAdditionsReserved",
        ):
            if contract not in metadata_text:
                errors.append(f"R2/M2 shard arena authority is missing: {contract}")
        if "VersionRecordNode" in metadata_text:
            errors.append("R2/M2 retained the transitional VersionRecordNode owner")
        for forbidden in (
            "struct VersionRecords",
            "shared_ptr<VersionRecords>",
            "std::list<",
            "std::map<",
            "preparedByTransaction",
            "mutableVersions",
            "KisPageStateSnapshot header;",
        ):
            if forbidden in metadata_text:
                errors.append(
                    f"R2/M3 retained a forbidden per-page snapshot/container: {forbidden}"
                )
        if "ensureArenaCapacity" in metadata_text:
            errors.append("R2/M2 retained shard-lock-local arena block growth")
        if "KisPageMetadataCoordinator::apply(" in metadata_text:
            errors.append(
                "C5 metadata coordinator regained the full-snapshot transition writer"
            )
        if metadata_text.count("Arena::prepareBlock()") != 1:
            errors.append(
                "R2/M2 arena block preparation escaped the single growth carrier"
            )
        growth_unlock = metadata_text.find("locker->unlock()")
        growth_prepare = metadata_text.find("growth->prepare(plan")
        growth_relock = metadata_text.find("locker->relock()")
        if not (growth_unlock >= 0 and
                growth_unlock < growth_prepare < growth_relock):
            errors.append(
                "R2/M2 arena growth is not visibly bracketed by shard unlock/relock"
            )
        if re.search(
            r"std::list\s*<\s*KisPageVersionStateSnapshot\s*>\s+ordered",
            metadata_text,
        ):
            errors.append("R2/M2 authority regressed to snapshot-owned version records")
        install_begin = metadata_text.find(
            "bool KisPageMetadataCoordinator::installPublicationImpl"
        )
        install_end = metadata_text.find(
            "KisPageMetadataCoordinator::KisPageMetadataCoordinator", install_begin
        )
        install_text = metadata_text[install_begin:install_end]
        for forbidden in (
            "reserveInsertions(",
            "reserveSlots(",
            "growth.blocks.attach(",
            "records.replaceAll(",
            "records.installPreparedAdditions(",
            "records.snapshot(entry.detachedVersion",
            "entry.shard->installHeader(",
        ):
            if forbidden in install_text:
                errors.append(
                    f"R2/M4 metadata install reacquires or allocates capacity: {forbidden}"
                )

    epoch_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisImageEpochReferenceModel.h"
    )
    if epoch_header.is_file():
        epoch_text = code_only(epoch_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "using InstallMetadataFunction = bool (*)(void *, KisImageEpochId)",
            "class PreparedRootReservation",
            "prepareRestore",
            "installRestore",
            "void *context",
            "InstallMetadataFunction installMetadata",
        ):
            if contract not in epoch_text:
                errors.append(
                    f"R2/M4 allocation-free epoch install callback is missing: {contract}"
                )
    public_store_header = REPOSITORY_ROOT / "libs/image/pagestore/KisPageStore.h"
    if public_store_header.is_file() and "KisPageMetadataArena_p.h" in code_only(
        public_store_header.read_text(encoding="utf-8", errors="replace")
    ):
        errors.append("R2/M2 typed metadata slots escaped into KisPageStore.h")

    write_coordinator_header = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageWriteCoordinator_p.h"
    )
    write_coordinator_source = (
        REPOSITORY_ROOT / "libs/image/pagestore/KisPageWriteCoordinator.cpp"
    )
    for path in (write_coordinator_header, write_coordinator_source):
        if not path.is_file():
            errors.append(f"R3/M5A write coordinator service is missing: {display_path(path)}")
    if write_coordinator_header.is_file():
        write_header = code_only(write_coordinator_header.read_text(
            encoding="utf-8", errors="replace"
        ))
        for contract in (
            "KisPageWriteCoordinator final",
            "KisPageWriteAdmission final",
            "class ClaimSet final",
            "KisBackingBudgetController final",
            "KisBackingBudgetReservation final",
            "KisMutationPageEntry final",
            "KisMutationWriteSet final",
            "struct TransactionActivity",
            "QHash<quint64, TransactionActivity> transactionActivities",
            "std::optional<KisMutationPageEntry> inlineEntry",
            "std::optional<QHash<KisPageKey, EntryIndex>> index",
            "static_assert(sizeof(KisPageWriteIntent) <= 32)",
            "static_assert(sizeof(KisMutationPageEntry) <= 104)",
            "QSharedPointer<const KisPageReplicaSource> initialization",
        ):
            if contract not in write_header:
                errors.append(f"BR1 converged write schema is missing: {contract}")
        entry_begin = write_header.find("KisMutationPageEntry final")
        entry_end = write_header.find("KisMutationWriteSet final", entry_begin)
        entry_text = write_header[entry_begin:entry_end]
        for forbidden in (
            "KisPageTransition",
            "KisPageAllocationDescriptor",
            "KisPreparedPageProof",
            "QSharedPointer<KisCpuResidentBinding>",
            "std::shared_ptr",
            "KisReplicaHandoffClaim",
            "KisCpuResidentBindingHandle",
            "KisBackingBudgetReservation",
        ):
            if forbidden in entry_text:
                errors.append(
                    f"R3/M5A compact mutation entry retained a cold owning field: {forbidden}"
                )
    if write_coordinator_source.is_file():
        write_source = code_only(write_coordinator_source.read_text(
            encoding="utf-8", errors="replace"
        ))
        selector_begin = write_source.find("KisPageWritePlanKind KisPageWriteCoordinator::select")
        selector_end = write_source.find(
            "void KisPageWriteCoordinator::recordPrepared",
            selector_begin,
        )
        selector_text = write_source[selector_begin:selector_end]
        for allowed in (
            "KisPageWritePlanKind::SemanticOnly",
            "KisPageWritePlanKind::ReusePending",
            "KisPageWritePlanKind::FreshPayload",
            "KisPageWritePlanKind::FreshDiscard",
            "KisPageWritePlanKind::FreshCow",
        ):
            if allowed not in selector_text:
                errors.append(f"R3/M5A fresh selector path is missing: {allowed}")
        for forbidden in (
            "KisPageWritePlanKind::HandoffRecoverable",
            "KisPageWritePlanKind::HandoffCommitOnly",
        ):
            if forbidden in selector_text:
                errors.append(f"R3/M5A opened handoff before M6/M7: {forbidden}")
    if page_store_source.is_file():
        page_begin = store_text.find("class KisPageMutationSession::Private")
        page_end = store_text.find("struct ColdPageSet", page_begin)
        cold_page = store_text[page_begin:page_end]
        for forbidden in ("KisPageTransition", "guardActive", "nativePrepared", "void *data"):
            if forbidden in cold_page:
                errors.append(f"mutation cold resources regained duplicate state: {forbidden}")
        if "static_assert(sizeof(Page) <= 688)" not in cold_page:
            errors.append("mutation cold resource layout lost its 688-byte ceiling")
        for contract in (
            "KisPageWriteAdmission writeAdmission",
            "KisBackingBudgetController backingBudget",
            "KisPageWriteCoordinator writeCoordinator",
        ):
            if contract not in store_text:
                errors.append(
                    f"KisPageStore::Private must embed the R3/M5A service by value: {contract}"
                )
        for forbidden in (
            "mutationPreparations",
            "genericWriteTransactions",
            "mutationTransactions",
        ):
            if forbidden in store_text:
                errors.append(
                    f"R3/M5A retained a parallel transaction activity map: {forbidden}"
                )

    return errors


def check_baseline_shape(baseline: dict[str, Any], manifest: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    if baseline.get("schema") != 1:
        errors.append("baseline source inventory: schema must be 1")
    if baseline.get("milestone") != "BR1-R3-M5B":
        errors.append("baseline source inventory: milestone must be BR1-R3-M5B")
    expected = manifest.get("baseline_commit")
    if baseline.get("baseline_commit") != expected:
        errors.append("baseline source inventory commit does not match intrusion manifest")
    records = baseline.get("dirty_source_union")
    if not isinstance(records, dict) or not records:
        errors.append("baseline source inventory has no dirty_source_union")
    else:
        tracked, untracked = changed_source_sets(manifest["baseline_commit"])
        current_paths = {path for path in tracked + untracked
                         if (REPOSITORY_ROOT / path).is_file()}
        if set(records) != current_paths:
            errors.append("source checkpoint inventory changed; refresh checkpoint after review")
        for path_name, record in records.items():
            if not isinstance(record, dict):
                errors.append(f"baseline source record must be an object: {path_name}")
                continue
            for field in (
                "sha256", "lines", "public_types", "member_fields",
                "container_declarations", "synchronization_declarations",
                "include_edges", "pagestore_identifiers",
            ):
                if field not in record:
                    errors.append(f"baseline source record lacks {field}: {path_name}")
            path = REPOSITORY_ROOT / path_name
            if not path.is_file() or record.get("sha256") != sha256(path):
                errors.append(f"source checkpoint is stale: {path_name}")
    return errors


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as destination:
        json.dump(value, destination, indent=2, sort_keys=True)
        destination.write("\n")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--ownership", type=Path, default=DEFAULT_OWNERSHIP)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--census", type=Path, default=DEFAULT_CENSUS)
    parser.add_argument("--callgraph", type=Path, default=DEFAULT_CALLGRAPH)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=REPOSITORY_ROOT / "_build-br0-cpu",
        help="build directory whose compile_commands.json and CMakeCache.txt are frozen",
    )
    parser.add_argument(
        "--write-baseline",
        action="store_true",
        help="replace the frozen source inventory from the current dirty worktree",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        manifest = load_json(arguments.manifest.resolve())
        ownership = load_json(arguments.ownership.resolve())
        census = load_json(arguments.census.resolve())
        callgraph = load_json(arguments.callgraph.resolve())
        if arguments.write_baseline:
            # Updating source hashes must not silently grant a higher intrusion
            # budget. Check the previous ceilings before accepting the snapshot.
            if arguments.baseline.is_file():
                violations = validate_ratchet(load_json(arguments.baseline.resolve()), manifest)
                if violations:
                    raise AuditFailure("cannot refresh baseline: " + "; ".join(violations))
            baseline = build_baseline(manifest, arguments.build_dir.resolve())
            write_json(arguments.baseline.resolve(), baseline)
            print(
                "Wrote BR1 PageStore source inventory: "
                f"{display_path(arguments.baseline.resolve())} "
                f"({len(baseline['dirty_source_union'])} dirty source/build files, "
                f"{len(baseline['ratchet']['external_pagestore_usage'])} external intrusion files)."
            )
        baseline = load_json(arguments.baseline.resolve())
    except (AuditFailure, subprocess.CalledProcessError, ValueError) as error:
        print(f"PageStore architecture audit failed: {error}", file=sys.stderr)
        return 1

    errors: list[str] = []
    errors.extend(validate_manifest(manifest))
    errors.extend(validate_ownership(ownership))
    errors.extend(validate_runtime_census(census, manifest))
    errors.extend(check_baseline_shape(baseline, manifest))
    errors.extend(validate_callgraph(callgraph, manifest, ownership, census, baseline))
    errors.extend(validate_ratchet(baseline, manifest))
    if errors:
        for error in errors:
            print(f"PageStore architecture violation: {error}", file=sys.stderr)
        return 1

    ratchet = baseline["ratchet"]
    print(
        "PageStore architecture checks passed: "
        f"{len(manifest['entries'])} manifest entries, "
        f"{len(ownership['facts'])} ownership facts, "
        f"{len(census['coverage'])} runtime census groups, "
        f"{len(callgraph['apis'])} indexed APIs, "
        f"{callgraph['call-edge-count']} reverse-call edges, "
        f"{len(ratchet['external_pagestore_usage'])} frozen external intrusion files."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
