#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Krita contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Generate the BR1 R0 Clang/IndexStore PageStore reverse-call graph.

The graph is deliberately generated from the real compile database instead of
from token searches.  Clang therefore resolves overloads, template
instantiations, virtual calls, macro expansions and address-taken functions in
the same language mode and include environment as the build.

This script writes a deterministic, reviewable JSON artifact.  It never
changes build outputs: every selected translation unit is re-parsed with
``-fsyntax-only`` and a temporary IndexStore/output directory.
"""

from __future__ import annotations

import argparse
from collections import Counter, deque
import concurrent.futures
import ctypes
import ctypes.util
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable


SCRIPT_PATH = Path(__file__).resolve()
REPOSITORY_ROOT = SCRIPT_PATH.parents[2]
AUDIT_ROOT = SCRIPT_PATH.parent / "pagestore-audit"
DEFAULT_MANIFEST = AUDIT_ROOT / "intrusion-manifest.json"
DEFAULT_OWNERSHIP = AUDIT_ROOT / "state-ownership.json"
DEFAULT_CENSUS = AUDIT_ROOT / "runtime-census.json"
DEFAULT_OUTPUT = AUDIT_ROOT / "reverse-callgraph.json"

INVENTORY_PREFIXES = ("libs/image/", "libs/vulkanbackend/")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inc"}
SCANNED_SUFFIXES = SOURCE_SUFFIXES | HEADER_SUFFIXES

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
PAGESTORE_TOKEN_RE = re.compile(
    r"\b(?:KisPageStore\w*|KisCapturedReadView|KisCpu(?:Resident|Read|Write)\w*|"
    r"KisPageMutationSession|KisTilePageStore\w*|KisTiles3PageReplicaProvider|"
    r"KisTiledDataManagerPageStore\w*|KisVulkanPageReplicaProvider)\b"
)

# indexstore_symbol_kind_t values from clang/include/indexstore/indexstore.h.
FUNCTION_KINDS = {
    12: "function",
    16: "instance-method",
    17: "class-method",
    18: "static-method",
    22: "constructor",
    23: "destructor",
    24: "conversion-function",
}

# indexstore_symbol_role_t bit values.
ROLE_DECLARATION = 1 << 0
ROLE_DEFINITION = 1 << 1
ROLE_REFERENCE = 1 << 2
ROLE_READ = 1 << 3
ROLE_WRITE = 1 << 4
ROLE_CALL = 1 << 5
ROLE_DYNAMIC = 1 << 6
ROLE_ADDRESS_OF = 1 << 7
ROLE_IMPLICIT = 1 << 8
ROLE_CHILD_OF = 1 << 9
ROLE_BASE_OF = 1 << 10
ROLE_OVERRIDE_OF = 1 << 11
ROLE_RECEIVED_BY = 1 << 12
ROLE_CALLED_BY = 1 << 13
ROLE_EXTENDED_BY = 1 << 14
ROLE_ACCESSOR_OF = 1 << 15
ROLE_CONTAINED_BY = 1 << 16
ROLE_SPECIALIZATION_OF = 1 << 18

CALLER_RELATION_ROLES = ROLE_CALLED_BY | ROLE_CONTAINED_BY


class GenerationFailure(Exception):
    pass


class StringRef(ctypes.Structure):
    _fields_ = [("data", ctypes.c_char_p), ("length", ctypes.c_size_t)]


def string_ref_text(value: StringRef) -> str:
    if not value.data:
        return ""
    return ctypes.string_at(value.data, value.length).decode("utf-8", "replace")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source)
    except (OSError, json.JSONDecodeError) as error:
        raise GenerationFailure(f"cannot read {path}: {error}") from error


def write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8") as destination:
        json.dump(value, destination, indent=2, sort_keys=True)
        destination.write("\n")
    temporary.replace(path)


def relative_project_path(path: str | Path) -> str | None:
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = REPOSITORY_ROOT / candidate
    try:
        return candidate.resolve().relative_to(REPOSITORY_ROOT).as_posix()
    except (OSError, ValueError):
        return None


def is_inventory_path(path: str) -> bool:
    return path.startswith(INVENTORY_PREFIXES)


def is_test_path(path: str) -> bool:
    return "/tests/" in f"/{path}/"


def source_fingerprints() -> dict[str, str]:
    # Freeze the selection universe as well as indexed files: a new caller or
    # changed include can change the TU closure without changing compile flags.
    return {
        relative_project_path(path): sha256(path)
        for prefix in INVENTORY_PREFIXES
        for path in sorted((REPOSITORY_ROOT / prefix).rglob("*"))
        if path.is_file() and (
            path.suffix in SCANNED_SUFFIXES | {".cmake"}
            or path.name == "CMakeLists.txt")
    }


def command_arguments(entry: dict[str, Any]) -> list[str]:
    arguments = entry.get("arguments")
    if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
        return list(arguments)
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    raise GenerationFailure(f"compile command has neither arguments nor command: {entry!r}")


def source_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


class IncludeClosure:
    """Conservative repository-local include closure used to select TUs."""

    def __init__(self) -> None:
        self._files: dict[str, Path] = {}
        self._basenames: dict[str, list[Path]] = {}
        for prefix in INVENTORY_PREFIXES:
            root = REPOSITORY_ROOT / prefix
            if not root.is_dir():
                continue
            for path in root.rglob("*"):
                if not path.is_file() or path.suffix not in SCANNED_SUFFIXES:
                    continue
                relative = path.resolve().relative_to(REPOSITORY_ROOT).as_posix()
                self._files[relative] = path.resolve()
                self._basenames.setdefault(path.name, []).append(path.resolve())
        self._memo: dict[Path, bool] = {}

    def _resolve_include(self, owner: Path, include: str) -> Path | None:
        candidates = (
            owner.parent / include,
            REPOSITORY_ROOT / include,
            REPOSITORY_ROOT / "libs/image" / include,
            REPOSITORY_ROOT / "libs/vulkanbackend" / include,
        )
        for candidate in candidates:
            if candidate.is_file():
                relative = relative_project_path(candidate)
                if relative and is_inventory_path(relative):
                    return candidate.resolve()
        matches = self._basenames.get(Path(include).name, [])
        if len(matches) == 1:
            return matches[0]
        suffix_matches = [
            match for match in matches
            if match.as_posix().endswith("/" + include)
        ]
        return suffix_matches[0] if len(suffix_matches) == 1 else None

    def reaches_pagestore(self, path: Path, active: set[Path] | None = None) -> bool:
        path = path.resolve()
        if path in self._memo:
            return self._memo[path]
        relative = relative_project_path(path)
        if relative is None or not is_inventory_path(relative):
            self._memo[path] = False
            return False
        if relative.startswith("libs/image/pagestore/"):
            self._memo[path] = True
            return True
        text = source_text(path)
        if PAGESTORE_TOKEN_RE.search(text):
            self._memo[path] = True
            return True
        recursion = set() if active is None else set(active)
        if path in recursion:
            return False
        recursion.add(path)
        for include in INCLUDE_RE.findall(text):
            dependency = self._resolve_include(path, include)
            if dependency is not None and self.reaches_pagestore(dependency, recursion):
                self._memo[path] = True
                return True
        self._memo[path] = False
        return False


@dataclass(frozen=True)
class TranslationUnit:
    path: str
    directory: str
    arguments: tuple[str, ...]
    reason: str


def select_translation_units(database: list[Any]) -> list[TranslationUnit]:
    closure = IncludeClosure()
    selected: dict[str, TranslationUnit] = {}
    for raw_entry in database:
        if not isinstance(raw_entry, dict) or not isinstance(raw_entry.get("file"), str):
            continue
        relative = relative_project_path(raw_entry["file"])
        if relative is None or not is_inventory_path(relative):
            continue
        path = REPOSITORY_ROOT / relative
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        if relative.startswith("libs/image/pagestore/"):
            reason = "pagestore-implementation"
        elif PAGESTORE_TOKEN_RE.search(source_text(path)):
            reason = "pagestore-identifier"
        elif closure.reaches_pagestore(path):
            reason = "transitive-pagestore-include"
        else:
            continue
        directory = raw_entry.get("directory")
        if not isinstance(directory, str) or not directory:
            raise GenerationFailure(f"compile command has no directory: {relative}")
        selected.setdefault(
            relative,
            TranslationUnit(
                path=relative,
                directory=directory,
                arguments=tuple(command_arguments(raw_entry)),
                reason=reason,
            ),
        )
    if not selected:
        raise GenerationFailure("compile database yielded no PageStore translation units")
    return [selected[path] for path in sorted(selected)]


def index_arguments(unit: TranslationUnit, index_path: Path, output_path: Path) -> list[str]:
    result: list[str] = []
    arguments = list(unit.arguments)
    index = 0
    paired_options = {"-o", "-MF", "-MT", "-MQ", "-MJ", "-serialize-diagnostics"}
    standalone_options = {"-c", "-MD", "-MMD", "-MP", "-MG"}
    while index < len(arguments):
        argument = arguments[index]
        if argument in paired_options:
            index += 2
            continue
        if argument in standalone_options:
            index += 1
            continue
        if argument.startswith("-fdiagnostics-color") or argument == "-fcolor-diagnostics":
            index += 1
            continue
        result.append(argument)
        index += 1
    result.extend(
        [
            "-fsyntax-only",
            "-index-store-path",
            str(index_path),
            "-index-ignore-system-symbols",
            "-index-ignore-pcms",
            "-o",
            str(output_path),
        ]
    )
    return result


def compile_translation_unit(
    unit: TranslationUnit,
    index_path: Path,
    output_root: Path,
    timeout: int,
) -> tuple[str, str]:
    output_name = hashlib.sha256(unit.path.encode("utf-8")).hexdigest() + ".o"
    command = index_arguments(unit, index_path, output_root / output_name)
    try:
        completed = subprocess.run(
            command,
            cwd=unit.directory,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        return unit.path, str(error)
    if completed.returncode != 0:
        diagnostics = (completed.stderr or completed.stdout).strip()
        return unit.path, diagnostics[-8000:]
    return unit.path, ""


def discover_indexstore_library(explicit: Path | None, compiler: str) -> Path:
    candidates: list[Path] = []
    if explicit is not None:
        candidates.append(explicit)
    environment = os.environ.get("LIBINDEXSTORE_PATH")
    if environment:
        candidates.append(Path(environment))
    compiler_path = Path(compiler)
    if compiler_path.is_absolute():
        candidates.extend(
            [
                compiler_path.parent.parent / "lib/libIndexStore.dylib",
                compiler_path.parent.parent / "lib/libIndexStore.so",
            ]
        )
    discovered = ctypes.util.find_library("IndexStore")
    if discovered:
        candidates.append(Path(discovered))
    try:
        xcrun = subprocess.run(
            ["xcrun", "--find", "clang"], capture_output=True, text=True, check=True
        ).stdout.strip()
        if xcrun:
            candidates.append(Path(xcrun).parent.parent / "lib/libIndexStore.dylib")
    except (OSError, subprocess.CalledProcessError):
        pass
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise GenerationFailure(
        "cannot locate libIndexStore; pass --indexstore-library or set LIBINDEXSTORE_PATH"
    )


class IndexStore:
    UNIT_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, StringRef)
    DEPENDENCY_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    OCCURRENCE_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    RELATION_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def __init__(self, library_path: Path, store_path: Path) -> None:
        self.library = ctypes.CDLL(str(library_path))
        self._declare_api()
        self.handle = self.library.indexstore_store_create(str(store_path).encode(), None)
        if not self.handle:
            raise GenerationFailure(f"cannot open IndexStore at {store_path}")

    def _declare_api(self) -> None:
        library = self.library
        library.indexstore_store_create.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        library.indexstore_store_create.restype = ctypes.c_void_p
        library.indexstore_store_dispose.argtypes = [ctypes.c_void_p]
        library.indexstore_store_units_apply_f.argtypes = [
            ctypes.c_void_p, ctypes.c_bool, ctypes.c_void_p, self.UNIT_CALLBACK
        ]
        library.indexstore_store_units_apply_f.restype = ctypes.c_bool

        library.indexstore_unit_reader_create.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)
        ]
        library.indexstore_unit_reader_create.restype = ctypes.c_void_p
        library.indexstore_unit_reader_dispose.argtypes = [ctypes.c_void_p]
        library.indexstore_unit_dependency_get_kind.argtypes = [ctypes.c_void_p]
        library.indexstore_unit_dependency_get_kind.restype = ctypes.c_int
        library.indexstore_unit_dependency_get_filepath.argtypes = [ctypes.c_void_p]
        library.indexstore_unit_dependency_get_filepath.restype = StringRef
        library.indexstore_unit_dependency_get_name.argtypes = [ctypes.c_void_p]
        library.indexstore_unit_dependency_get_name.restype = StringRef
        library.indexstore_unit_reader_dependencies_apply_f.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, self.DEPENDENCY_CALLBACK
        ]
        library.indexstore_unit_reader_dependencies_apply_f.restype = ctypes.c_bool

        library.indexstore_record_reader_create.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)
        ]
        library.indexstore_record_reader_create.restype = ctypes.c_void_p
        library.indexstore_record_reader_dispose.argtypes = [ctypes.c_void_p]
        library.indexstore_record_reader_occurrences_apply_f.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, self.OCCURRENCE_CALLBACK
        ]
        library.indexstore_record_reader_occurrences_apply_f.restype = ctypes.c_bool

        library.indexstore_occurrence_get_symbol.argtypes = [ctypes.c_void_p]
        library.indexstore_occurrence_get_symbol.restype = ctypes.c_void_p
        library.indexstore_occurrence_get_roles.argtypes = [ctypes.c_void_p]
        library.indexstore_occurrence_get_roles.restype = ctypes.c_uint64
        library.indexstore_occurrence_get_line_col.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)
        ]
        library.indexstore_occurrence_relations_apply_f.argtypes = [
            ctypes.c_void_p, ctypes.c_void_p, self.RELATION_CALLBACK
        ]
        library.indexstore_occurrence_relations_apply_f.restype = ctypes.c_bool

        library.indexstore_symbol_get_kind.argtypes = [ctypes.c_void_p]
        library.indexstore_symbol_get_kind.restype = ctypes.c_int
        for getter in ("name", "usr", "codegen_name"):
            function = getattr(library, f"indexstore_symbol_get_{getter}")
            function.argtypes = [ctypes.c_void_p]
            function.restype = StringRef
        library.indexstore_symbol_relation_get_roles.argtypes = [ctypes.c_void_p]
        library.indexstore_symbol_relation_get_roles.restype = ctypes.c_uint64
        library.indexstore_symbol_relation_get_symbol.argtypes = [ctypes.c_void_p]
        library.indexstore_symbol_relation_get_symbol.restype = ctypes.c_void_p

    def close(self) -> None:
        if self.handle:
            self.library.indexstore_store_dispose(self.handle)
            self.handle = None

    def records(self) -> list[tuple[str, str]]:
        records: set[tuple[str, str]] = set()

        @self.UNIT_CALLBACK
        def visit_unit(_context: int, unit_name: StringRef) -> bool:
            reader = self.library.indexstore_unit_reader_create(
                self.handle, string_ref_text(unit_name).encode(), None
            )
            if not reader:
                return False

            @self.DEPENDENCY_CALLBACK
            def visit_dependency(_inner_context: int, dependency: int) -> bool:
                # INDEXSTORE_UNIT_DEPENDENCY_RECORD == 2.
                if self.library.indexstore_unit_dependency_get_kind(dependency) == 2:
                    filepath = string_ref_text(
                        self.library.indexstore_unit_dependency_get_filepath(dependency)
                    )
                    record_name = string_ref_text(
                        self.library.indexstore_unit_dependency_get_name(dependency)
                    )
                    if filepath and record_name:
                        records.add((filepath, record_name))
                return True

            applied = self.library.indexstore_unit_reader_dependencies_apply_f(
                reader, None, visit_dependency
            )
            self.library.indexstore_unit_reader_dispose(reader)
            return bool(applied)

        applied = self.library.indexstore_store_units_apply_f(
            self.handle, True, None, visit_unit
        )
        if not applied:
            raise GenerationFailure("IndexStore unit/dependency traversal failed")
        return sorted(records)

    def occurrences(
        self,
        record_name: str,
        receiver: Callable[[dict[str, Any]], None],
    ) -> None:
        reader = self.library.indexstore_record_reader_create(
            self.handle, record_name.encode(), None
        )
        if not reader:
            raise GenerationFailure(f"cannot open IndexStore record {record_name}")

        @self.OCCURRENCE_CALLBACK
        def visit_occurrence(_context: int, occurrence: int) -> bool:
            symbol = self.library.indexstore_occurrence_get_symbol(occurrence)
            line = ctypes.c_uint()
            column = ctypes.c_uint()
            self.library.indexstore_occurrence_get_line_col(
                occurrence, ctypes.byref(line), ctypes.byref(column)
            )
            relations: list[dict[str, Any]] = []

            @self.RELATION_CALLBACK
            def visit_relation(_inner_context: int, relation: int) -> bool:
                related = self.library.indexstore_symbol_relation_get_symbol(relation)
                relations.append(
                    {
                        "roles": int(self.library.indexstore_symbol_relation_get_roles(relation)),
                        "kind": int(self.library.indexstore_symbol_get_kind(related)),
                        "name": string_ref_text(self.library.indexstore_symbol_get_name(related)),
                        "usr": string_ref_text(self.library.indexstore_symbol_get_usr(related)),
                    }
                )
                return True

            self.library.indexstore_occurrence_relations_apply_f(
                occurrence, None, visit_relation
            )
            receiver(
                {
                    "roles": int(self.library.indexstore_occurrence_get_roles(occurrence)),
                    "kind": int(self.library.indexstore_symbol_get_kind(symbol)),
                    "name": string_ref_text(self.library.indexstore_symbol_get_name(symbol)),
                    "usr": string_ref_text(self.library.indexstore_symbol_get_usr(symbol)),
                    "codegen-name": string_ref_text(
                        self.library.indexstore_symbol_get_codegen_name(symbol)
                    ),
                    "line": int(line.value),
                    "column": int(column.value),
                    "relations": relations,
                }
            )
            return True

        applied = self.library.indexstore_record_reader_occurrences_apply_f(
            reader, None, visit_occurrence
        )
        self.library.indexstore_record_reader_dispose(reader)
        if not applied:
            raise GenerationFailure(f"IndexStore occurrence traversal failed: {record_name}")


def usr_qualified_name(usr: str, fallback: str) -> str:
    # Parameter USRs follow the function name in a Clang USR.  Only parse
    # scopes before the first @F@ marker or parameter classes would be
    # mistaken for owners (for example Class::QString::method).
    scope, marker, function_tail = usr.partition("@F@")
    components = re.findall(r"@(?:N|S|C|U|E|P)@([^@#]+)", scope)
    if marker:
        function = function_tail.split("#", 1)[0].split("@", 1)[0]
        if function:
            components.append(function)
    elif fallback and (not components or components[-1] != fallback):
        components.append(fallback)
    return "::".join(components) or fallback


def location(path: str, occurrence: dict[str, Any]) -> dict[str, Any]:
    return {
        "column": occurrence["column"],
        "line": occurrence["line"],
        "path": path,
    }


def relation_symbols(
    occurrence: dict[str, Any], role_mask: int
) -> list[dict[str, Any]]:
    return [
        relation for relation in occurrence["relations"]
        if relation["roles"] & role_mask
    ]


def edge_key(edge: dict[str, Any]) -> tuple[Any, ...]:
    return (
        edge["path"], edge["line"], edge["column"], edge["caller-usr"],
        edge["dynamic"], edge["implicit"],
    )


def build_graph(
    store: IndexStore,
    records: list[tuple[str, str]],
    units: list[TranslationUnit],
    compile_commands: Path,
    library_path: Path,
    manifest: dict[str, Any],
    ownership: dict[str, Any],
    census: dict[str, Any],
) -> dict[str, Any]:
    project_records: list[tuple[str, str]] = []
    for filepath, record_name in records:
        relative = relative_project_path(filepath)
        if relative is not None and is_inventory_path(relative):
            project_records.append((relative, record_name))
    project_records = sorted(set(project_records))
    if not project_records:
        raise GenerationFailure("IndexStore contains no repository inventory records")

    api_symbols: dict[str, dict[str, Any]] = {}
    seen_symbols: dict[str, str] = {}
    record_paths = {path for path, _record in project_records}

    def api_declaration(path: str, occurrence: dict[str, Any]) -> None:
        usr = occurrence["usr"]
        if usr:
            seen_symbols[usr] = usr_qualified_name(usr, occurrence["name"])
        if (
            occurrence["kind"] not in FUNCTION_KINDS
            or not occurrence["roles"] & (ROLE_DECLARATION | ROLE_DEFINITION)
            or not path.startswith("libs/image/pagestore/")
            or not usr
        ):
            return
        api = api_symbols.setdefault(
            usr,
            {
                "codegen-name": occurrence["codegen-name"],
                "declarations": [],
                "definitions": [],
                "kind": FUNCTION_KINDS[occurrence["kind"]],
                "name": occurrence["name"],
                "qualified-name": usr_qualified_name(usr, occurrence["name"]),
                "usr": usr,
            },
        )
        target = "definitions" if occurrence["roles"] & ROLE_DEFINITION else "declarations"
        api[target].append(location(path, occurrence))

    for path, record_name in project_records:
        if path.startswith("libs/image/pagestore/"):
            store.occurrences(record_name, lambda occurrence, p=path: api_declaration(p, occurrence))
    if not api_symbols:
        raise GenerationFailure("no PageStore function/method declarations found in IndexStore")

    for api in api_symbols.values():
        api["declarations"] = sorted(
            {tuple(item.values()): item for item in api["declarations"]}.values(),
            key=lambda item: (item["path"], item["line"], item["column"]),
        )
        api["definitions"] = sorted(
            {tuple(item.values()): item for item in api["definitions"]}.values(),
            key=lambda item: (item["path"], item["line"], item["column"]),
        )
        api["production-callers"] = []
        api["test-callers"] = []
        api["callback-registrations"] = []
        api["non-call-references"] = []
        api["specializations-and-overrides"] = []

    unresolved: list[dict[str, Any]] = []

    def call_occurrence(path: str, occurrence: dict[str, Any]) -> None:
        usr = occurrence["usr"]
        if usr:
            seen_symbols[usr] = usr_qualified_name(usr, occurrence["name"])
        for relation in occurrence["relations"]:
            if relation["usr"]:
                seen_symbols[relation["usr"]] = usr_qualified_name(
                    relation["usr"], relation["name"]
                )
        if usr not in api_symbols:
            return
        roles = occurrence["roles"]
        callers = relation_symbols(occurrence, CALLER_RELATION_ROLES)
        if roles & ROLE_REFERENCE and not roles & (ROLE_CALL | ROLE_ADDRESS_OF):
            api_symbols[usr]["non-call-references"].append(location(path, occurrence))
        if roles & ROLE_CALL:
            if callers:
                for caller in callers:
                    edge = {
                        **location(path, occurrence),
                        "caller": usr_qualified_name(caller["usr"], caller["name"]),
                        "caller-usr": caller["usr"],
                        "dynamic": bool(roles & ROLE_DYNAMIC),
                        "implicit": bool(roles & ROLE_IMPLICIT),
                    }
                    destination = "test-callers" if is_test_path(path) else "production-callers"
                    api_symbols[usr][destination].append(edge)
            else:
                # Namespace-scope initializers have no function caller but their
                # source callsite is still a resolved reverse edge.
                edge = {
                    **location(path, occurrence),
                    "caller": "<translation-unit-initializer>",
                    "caller-usr": "",
                    "dynamic": bool(roles & ROLE_DYNAMIC),
                    "implicit": bool(roles & ROLE_IMPLICIT),
                }
                destination = "test-callers" if is_test_path(path) else "production-callers"
                api_symbols[usr][destination].append(edge)
        if roles & ROLE_ADDRESS_OF:
            registration = {
                **location(path, occurrence),
                "containers": [
                    {
                        "name": usr_qualified_name(caller["usr"], caller["name"]),
                        "usr": caller["usr"],
                    }
                    for caller in callers
                ],
                "scope": "test" if is_test_path(path) else "production",
            }
            api_symbols[usr]["callback-registrations"].append(registration)
            if not callers:
                unresolved.append(
                    {
                        **location(path, occurrence),
                        "api": api_symbols[usr]["qualified-name"],
                        "api-usr": usr,
                        "reason": "address-taken PageStore API has no containing/calling symbol",
                    }
                )
        special_relations = relation_symbols(
            occurrence, ROLE_OVERRIDE_OF | ROLE_SPECIALIZATION_OF
        )
        for relation in special_relations:
            api_symbols[usr]["specializations-and-overrides"].append(
                {
                    **location(path, occurrence),
                    "related": usr_qualified_name(relation["usr"], relation["name"]),
                    "related-usr": relation["usr"],
                    "relation": (
                        "override" if relation["roles"] & ROLE_OVERRIDE_OF else "specialization"
                    ),
                }
            )

    for path, record_name in project_records:
        store.occurrences(record_name, lambda occurrence, p=path: call_occurrence(p, occurrence))

    for api in api_symbols.values():
        for field in ("production-callers", "test-callers"):
            unique = {edge_key(edge): edge for edge in api[field]}
            api[field] = sorted(unique.values(), key=edge_key)
        for field in ("callback-registrations", "specializations-and-overrides", "non-call-references"):
            api[field] = sorted(
                api[field],
                key=lambda item: (item["path"], item["line"], item["column"]),
            )

    manifest_bindings: list[dict[str, Any]] = []
    manifest_entries = manifest.get("entries", [])
    for entry in manifest_entries:
        if not isinstance(entry, dict):
            continue
        entry_path = entry.get("path", "")
        symbol_names = set(entry.get("symbols", []))
        static_callers = set(entry.get("static-callers", []))
        declared_apis = sorted(
            api["usr"]
            for api in api_symbols.values()
            if any(
                item["path"] == entry_path
                for item in api["declarations"] + api["definitions"]
            )
            or api["name"] in symbol_names
            or api["qualified-name"] in symbol_names
        )
        matched_callers = sorted(
            {
                qualified
                for qualified in seen_symbols.values()
                if qualified in static_callers
                or qualified.split("::")[-1] in static_callers
            }
        )
        manifest_bindings.append(
            {
                "coverage": (
                    "indexed"
                    if entry_path in record_paths
                    else "deferred-outside-cpu-compile-database"
                ),
                "declared-api-usrs": declared_apis,
                "id": entry.get("id", ""),
                "matched-static-callers": matched_callers,
                "path": entry_path,
                "source-record-indexed": entry_path in record_paths,
            }
        )

    physical_apis = sorted(
        (
            api for api in api_symbols.values()
            if api["qualified-name"] == "KisCpuWriteBindingReservation::acquire"
        ),
        key=lambda api: api["usr"],
    )
    production_count = sum(len(api["production-callers"]) for api in physical_apis)
    test_count = sum(len(api["test-callers"]) for api in physical_apis)
    callback_count = sum(len(api["callback-registrations"]) for api in physical_apis)
    physical_ownership = next(
        (
            fact for fact in ownership.get("facts", [])
            if isinstance(fact, dict) and fact.get("id") == "BR1-OWN-PHYSICAL-ALLOCATION"
        ),
        {},
    )
    physical_census = next(
        (
            item for item in census.get("coverage", [])
            if isinstance(item, dict) and item.get("id") == "physical-exclusive-gate"
        ),
        {},
    )
    reasons: list[str] = []
    if production_count:
        reasons.append("fresh native writes retain the move-only binding reservation")
    else:
        reasons.append("no production binding reservation caller was indexed")
    if test_count:
        reasons.append("test-only callers retain the physical-claim failure contract")
    if callback_count:
        reasons.append("address-taken registrations remain")
    if physical_ownership.get("status") != "canonical":
        reasons.append("physical allocation ownership remains split")
    if physical_census.get("status") not in {"instrumented", "existing"}:
        reasons.append("production reservation metrics are missing")

    apis = sorted(api_symbols.values(), key=lambda api: (api["qualified-name"], api["usr"]))
    call_count = sum(
        len(api["production-callers"]) + len(api["test-callers"])
        for api in apis
    )
    callback_total = sum(len(api["callback-registrations"]) for api in apis)
    return {
        "api-count": len(apis),
        "apis": apis,
        "call-edge-count": call_count,
        "callback-registration-count": callback_total,
        "compile-commands": {
            "path": relative_project_path(compile_commands) or str(compile_commands.resolve()),
            "sha256": sha256(compile_commands),
        },
        "generator": relative_project_path(SCRIPT_PATH),
        "indexstore-library": str(library_path),
        "indexed-record-count": len(project_records),
        "indexed-translation-units": [
            {
                "path": unit.path,
                "reason": unit.reason,
                "scope": "test" if is_test_path(unit.path) else "production",
            }
            for unit in units
        ],
        "manifest-bindings": manifest_bindings,
        "milestone": manifest["milestone"],
        "physical-exclusive-gate": {
            "api-usrs": [api["usr"] for api in physical_apis],
            "callback-registration-count": callback_count,
            "d0-eligible": False,
            "decision": "retain-fresh-write-reservation-until-M6-provider-handoff",
            "ownership-fact": "BR1-OWN-PHYSICAL-ALLOCATION",
            "ownership-status": physical_ownership.get("status", "missing"),
            "production-call-site-count": production_count,
            "reasons": reasons,
            "static-proof": "production-callers-bound" if production_count else "production-caller-missing",
            "test-call-site-count": test_count,
        },
        "schema": 1,
        "unresolved-edges": sorted(
            unresolved,
            key=lambda item: (item["path"], item["line"], item["column"], item["api-usr"]),
        ),
    }


def reachability_review(apis: list[dict[str, Any]]) -> dict[str, Any]:
    """Conservative boundary reachability, NOT an automatic dead-code verdict.

    A call in a production file is an edge, not a root. Keep outside callers
    as boundary roots until their own application entry paths are audited.
    Implicit lifetime/template/virtual edges need a separate review class:
    IndexStore does not provide whole-program dispatch or RAII reachability.
    """
    nodes = {api["usr"]: api for api in apis}
    edges: dict[str, set[str]] = {usr: set() for usr in nodes}
    roots: dict[str, dict[str, str]] = {kind: {} for kind in ("production", "test", "indirect")}

    def add_edge(caller: str, callee: str, scope: str, reason: str) -> None:
        if caller in nodes:
            edges[caller].add(callee)
        else:
            roots[scope].setdefault(callee, reason)

    for usr, api in nodes.items():
        for scope in ("production", "test"):
            for call in api[f"{scope}-callers"]:
                add_edge(call["caller-usr"], usr, scope,
                         f"{call['caller']} at {call['path']}:{call['line']}")
        for registration in api["callback-registrations"]:
            for container in registration["containers"]:
                add_edge(container["usr"], usr, registration["scope"], container["name"])
            if not registration["containers"]:
                roots["indirect"][usr] = "unresolved address-taken container"
        for relation in api["specializations-and-overrides"]:
            related = relation["related-usr"]
            if related in nodes:
                # Conservative: a call to either declaration can keep both.
                edges[related].add(usr)
                edges[usr].add(related)
            else:
                roots["indirect"][usr] = "override/specialization outside indexed API nodes"
        if any(not is_test_path(ref["path"]) for ref in api.get("non-call-references", [])):
            roots["indirect"][usr] = "production reference without a resolved call edge"
        if (api["kind"] in {"constructor", "destructor", "conversion-function"}
                or api["name"].startswith("operator") or api["name"] == "qHash"
                or "@FT@" in usr or "@ST>" in usr):
            roots["indirect"].setdefault(usr, "implicit lifetime, operator or template review")

    # BFS witnesses are kept separately for production, tests and uncertain
    # implicit roots. Test roots must never keep a production island alive.
    def witnesses(seeds: dict[str, str]) -> dict[str, str | None]:
        predecessor: dict[str, str | None] = {usr: None for usr in sorted(seeds)}
        pending = deque(predecessor)
        while pending:
            caller = pending.popleft()
            for callee in sorted(edges[caller]):
                if callee not in predecessor:
                    predecessor[callee] = caller
                    pending.append(callee)
        return predecessor

    paths = {scope: witnesses(seeds) for scope, seeds in roots.items()}
    functions = []
    for usr in sorted(nodes):
        status = ("production-boundary-reachable" if usr in paths["production"] else
                  "indirect-review" if usr in paths["indirect"] else
                  "test-only-review" if usr in paths["test"] else "unreached-review")
        functions.append({"usr": usr, "status": status,
                          "predecessors": {scope: path[usr] for scope, path in paths.items() if usr in path}})

    # Strongly connected components of the non-production subgraph expose
    # mutually calling islands. Iterative DFS avoids a recursion-depth limit.
    remaining = set(nodes) - paths["production"].keys()
    reverse = {usr: set() for usr in remaining}
    for caller in remaining:
        for callee in edges[caller] & remaining:
            reverse[callee].add(caller)
    seen: set[str] = set()
    order: list[str] = []
    for start in sorted(remaining):
        stack = [(start, False)]
        while stack:
            usr, finished = stack.pop()
            if finished:
                order.append(usr)
            elif usr not in seen:
                seen.add(usr)
                stack.append((usr, True))
                stack.extend((callee, False) for callee in sorted(edges[usr] & remaining, reverse=True))
    components = []
    seen.clear()
    for start in reversed(order):
        if start in seen:
            continue
        component = []
        pending = [start]
        seen.add(start)
        for usr in pending:
            component.append(usr)
            for caller in sorted(reverse[usr] - seen):
                seen.add(caller)
                pending.append(caller)
        components.append(sorted(component))
    return {"scope": "indexed PageStore API boundary; not whole-program/configuration proof",
            "deletion-authorized": False, "roots": roots, "functions": functions,
            "summary": dict(sorted(Counter(item["status"] for item in functions).items())),
            "nonproduction-components": sorted(components)}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--compile-commands",
        type=Path,
        default=REPOSITORY_ROOT / "_build-br0-cpu/compile_commands.json",
    )
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--ownership", type=Path, default=DEFAULT_OWNERSHIP)
    parser.add_argument("--census", type=Path, default=DEFAULT_CENSUS)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--indexstore-library", type=Path)
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--compile-timeout", type=int, default=180)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        compile_commands = arguments.compile_commands.resolve()
        database = load_json(compile_commands)
        if not isinstance(database, list):
            raise GenerationFailure("compile_commands.json must contain an array")
        manifest = load_json(arguments.manifest.resolve())
        ownership = load_json(arguments.ownership.resolve())
        census = load_json(arguments.census.resolve())
        if not all(isinstance(value, dict) for value in (manifest, ownership, census)):
            raise GenerationFailure("audit inputs must contain JSON objects")
        fingerprints = source_fingerprints()
        input_paths = (compile_commands, arguments.manifest.resolve(),
                       arguments.ownership.resolve(), arguments.census.resolve(), SCRIPT_PATH)
        input_hashes = {str(path): sha256(path) for path in input_paths}
        units = select_translation_units(database)
        compiler = units[0].arguments[0]
        library_path = discover_indexstore_library(arguments.indexstore_library, compiler)
        print(
            f"Indexing {len(units)} PageStore-reachable translation units "
            f"with {arguments.jobs} workers...",
            flush=True,
        )
        with tempfile.TemporaryDirectory(prefix="krita-pagestore-index-") as temporary:
            temporary_root = Path(temporary)
            index_path = temporary_root / "index"
            output_root = temporary_root / "outputs"
            index_path.mkdir()
            output_root.mkdir()
            failures: list[tuple[str, str]] = []
            with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, arguments.jobs)) as pool:
                futures = [
                    pool.submit(
                        compile_translation_unit,
                        unit,
                        index_path,
                        output_root,
                        arguments.compile_timeout,
                    )
                    for unit in units
                ]
                for future in concurrent.futures.as_completed(futures):
                    path, diagnostics = future.result()
                    if diagnostics:
                        failures.append((path, diagnostics))
            if failures:
                detail = "\n\n".join(
                    f"[{path}]\n{diagnostics}" for path, diagnostics in sorted(failures)
                )
                raise GenerationFailure(
                    f"{len(failures)} translation units failed IndexStore parsing:\n{detail}"
                )
            store = IndexStore(library_path, index_path)
            try:
                graph = build_graph(
                    store,
                    store.records(),
                    units,
                    compile_commands,
                    library_path,
                    manifest,
                    ownership,
                    census,
                )
            finally:
                store.close()
        if graph["unresolved-edges"]:
            raise GenerationFailure(
                f"{len(graph['unresolved-edges'])} unresolved PageStore callback edges; "
                "register or eliminate them before freezing the graph"
            )
        if fingerprints != source_fingerprints() or any(
            sha256(Path(path)) != digest for path, digest in input_hashes.items()
        ):
            raise GenerationFailure("source or audit inputs changed during indexing; regenerate")
        graph["reachability-review"] = reachability_review(graph["apis"])
        graph["source-fingerprints"] = fingerprints
        graph["audit-input-fingerprints"] = {
            relative_project_path(path) or path: digest
            for path, digest in input_hashes.items()
        }
        write_json(arguments.output.resolve(), graph)
        physical = graph["physical-exclusive-gate"]
        print(
            f"Wrote {arguments.output.resolve()}: {graph['api-count']} APIs, "
            f"{graph['call-edge-count']} call edges, "
            f"{graph['callback-registration-count']} callback registrations; "
            f"physical reservation production/test callers "
            f"{physical['production-call-site-count']}/{physical['test-call-site-count']}.",
            flush=True,
        )
        return 0
    except (GenerationFailure, OSError, ValueError) as error:
        print(f"PageStore call graph generation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
