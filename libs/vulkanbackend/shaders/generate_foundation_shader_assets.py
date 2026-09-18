#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Krita contributors
# SPDX-License-Identifier: GPL-2.0-or-later

"""Validate reflection and emit deterministic BR0 foundation shader assets."""

import argparse
import hashlib
import json
import pathlib
import struct


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_if_changed(path, data):
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return
    path.write_bytes(data)


def validate_reflection(reflection):
    entry_points = reflection.get("entryPoints", [])
    compute_entries = [
        entry for entry in entry_points
        if entry.get("name") == "main" and entry.get("mode") == "comp"
    ]
    if len(compute_entries) != 1:
        raise ValueError("foundation shader must expose exactly one compute entry point named main")
    if compute_entries[0].get("workgroup_size") != [64, 1, 1]:
        raise ValueError("foundation shader workgroup ABI must remain 64 x 1 x 1")

    bindings = sorted(
        (resource.get("set"), resource.get("binding"))
        for resource in reflection.get("ssbos", [])
    )
    if bindings != [(0, 0), (0, 1)]:
        raise ValueError("foundation shader must expose storage buffers at set 0 bindings 0 and 1")

    push_constants = reflection.get("push_constants", [])
    if len(push_constants) != 1:
        raise ValueError("foundation shader must expose exactly one push-constant block")
    type_id = push_constants[0].get("type")
    members = reflection.get("types", {}).get(type_id, {}).get("members", [])
    if len(members) != 1 or members[0].get("type") != "uint" or members[0].get("offset") != 0:
        raise ValueError("foundation shader push-constant ABI must be one uint at offset zero")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spirv", required=True)
    parser.add_argument("--source", required=True)
    parser.add_argument("--reflection", required=True)
    parser.add_argument("--contract", required=True)
    parser.add_argument("--header", required=True)
    parser.add_argument("--cpp", required=True)
    parser.add_argument("--canonical-reflection", required=True)
    parser.add_argument("--generated-manifest", required=True)
    args = parser.parse_args()

    spirv = pathlib.Path(args.spirv).read_bytes()
    if len(spirv) == 0 or len(spirv) % 4:
        raise ValueError("compiled shader is empty or is not a complete SPIR-V word stream")
    words = struct.unpack("<{}I".format(len(spirv) // 4), spirv)
    if words[0] != 0x07230203:
        raise ValueError("compiled shader does not have the SPIR-V magic word")

    reflection = json.loads(pathlib.Path(args.reflection).read_text(encoding="utf-8"))
    validate_reflection(reflection)
    canonical_reflection = (json.dumps(
        reflection, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ) + "\n").encode("utf-8")

    contract = json.loads(pathlib.Path(args.contract).read_text(encoding="utf-8"))
    assets = contract.get("assets", [])
    if contract.get("foundationAbi") != 1 or len(assets) != 1:
        raise ValueError("foundation shader manifest must declare ABI 1 and exactly one asset")
    asset = assets[0]
    if asset.get("id") != "foundation.buffer_checksum" or asset.get("semanticAbi") != 1:
        raise ValueError("foundation checksum shader manifest ABI does not match the BR0 contract")
    declared_bindings = sorted(
        (binding.get("set"), binding.get("binding"))
        for binding in asset.get("descriptorBindings", [])
    )
    if declared_bindings != [(0, 0), (0, 1)]:
        raise ValueError("foundation manifest descriptor bindings do not match reflection ABI")
    if (asset.get("stage") != "compute" or
            asset.get("entryPoint") != "main" or
            asset.get("workgroupSize") != [64, 1, 1] or
            asset.get("pushConstantBytes") != 4 or
            asset.get("source") != pathlib.Path(args.source).name):
        raise ValueError("foundation manifest execution ABI does not match the compiled shader")

    source = pathlib.Path(args.source).read_bytes()
    spirv_hash = sha256(spirv)
    reflection_hash = sha256(canonical_reflection)
    source_hash = sha256(source)

    header = """/* Generated file; do not edit. */
#pragma once

#include <cstddef>
#include <cstdint>

namespace KisVulkanFoundationShaderAssets {
extern const std::uint32_t checksumSpirv[];
extern const std::size_t checksumSpirvWordCount;
extern const char checksumSpirvSha256[];
extern const char checksumReflectionSha256[];
extern const char checksumSourceSha256[];
constexpr std::uint32_t checksumSemanticAbi = 1;
}
"""
    word_lines = []
    for offset in range(0, len(words), 8):
        word_lines.append("    " + ", ".join(
            "0x{:08x}u".format(word) for word in words[offset:offset + 8]
        ))
    cpp = """/* Generated file; do not edit. */
#include "KisVulkanFoundationShaderAssets.h"

namespace KisVulkanFoundationShaderAssets {
const std::uint32_t checksumSpirv[] = {
%s
};
const std::size_t checksumSpirvWordCount = sizeof(checksumSpirv) / sizeof(checksumSpirv[0]);
const char checksumSpirvSha256[] = "%s";
const char checksumReflectionSha256[] = "%s";
const char checksumSourceSha256[] = "%s";
}
""" % (",\n".join(word_lines), spirv_hash, reflection_hash, source_hash)

    generated_manifest = dict(contract)
    generated_manifest["status"] = "sealed"
    generated_manifest["readiness"] = {
        "canSeal": True,
        "reason": "SPIR-V validation and reflection ABI checks completed during this build."
    }
    generated_asset = dict(asset)
    generated_asset["wordCount"] = len(words)
    generated_asset["sourceSha256"] = source_hash
    generated_asset["spirvSha256"] = spirv_hash
    generated_asset["reflectionSha256"] = reflection_hash
    generated_manifest["assets"] = [generated_asset]
    manifest_bytes = (json.dumps(
        generated_manifest, indent=2, sort_keys=True, ensure_ascii=True
    ) + "\n").encode("utf-8")

    write_if_changed(args.header, header.encode("utf-8"))
    write_if_changed(args.cpp, cpp.encode("utf-8"))
    write_if_changed(args.canonical_reflection, canonical_reflection)
    write_if_changed(args.generated_manifest, manifest_bytes)


if __name__ == "__main__":
    main()
