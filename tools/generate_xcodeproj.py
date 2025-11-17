#!/usr/bin/env python3
"""Generate the hand-authored Xcode project for pscal.

The CMake generator that ships with Xcode does not understand all of the
options this project needs.  This script emits an `.xcodeproj` that mirrors
the targets and default build options from the canonical CMake build without
relying on CMake itself.  It produces deterministic object identifiers so
the generated project is stable across runs.
"""

from __future__ import annotations

import argparse
import hashlib
import subprocess
from collections import OrderedDict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, Iterable, List, Tuple


PROJECT_NAME = "Pscal"
REPO_ROOT = Path(__file__).resolve().parent.parent


SDL_HEADER_HINTS = [
    "/Library/Frameworks/SDL3.framework/Headers",
    "$(HOME)/Library/Frameworks/SDL3.framework/Headers",
    "/Library/Frameworks/SDL2.framework/Headers",
    "$(HOME)/Library/Frameworks/SDL2.framework/Headers",
    "/opt/homebrew/include",
    "/opt/homebrew/include/SDL3",
    "/opt/homebrew/include/SDL2",
    "/opt/homebrew/opt/sdl3/include",
    "/opt/homebrew/opt/sdl3/include/SDL3",
    "/opt/homebrew/opt/sdl2/include",
    "/opt/homebrew/opt/sdl2/include/SDL2",
    "/usr/local/include",
    "/usr/local/include/SDL3",
    "/usr/local/include/SDL2",
    "/usr/local/opt/sdl3/include",
    "/usr/local/opt/sdl3/include/SDL3",
    "/usr/local/opt/sdl2/include",
    "/usr/local/opt/sdl2/include/SDL2",
    "/usr/include",
    "/usr/include/SDL3",
    "/usr/include/SDL2",
]


def determine_version_info(release_build: bool) -> Tuple[str, str]:
    """Mirror the CMake version and tag discovery logic."""

    timestamp = datetime.now().strftime("%Y%m%d.%H%M")
    suffix = "_REL" if release_build else "_DEV"
    program_version = f"{timestamp}{suffix}"

    git_tag = "untagged"
    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--abbrev=0"],
            check=True,
            capture_output=True,
            cwd=REPO_ROOT,
            text=True,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        pass
    else:
        candidate = result.stdout.strip()
        if candidate:
            git_tag = candidate

    return program_version, git_tag


def escape_define(value: str) -> str:
    """Escape a string for inclusion in GCC_PREPROCESSOR_DEFINITIONS."""

    return value.replace("\\", "\\\\").replace("\"", "\\\"")


def md5_id(*parts: str) -> str:
    """Return a deterministic 24-character uppercase hex identifier."""

    digest = hashlib.md5("::".join(parts).encode("utf-8")).hexdigest()
    return digest[:24].upper()


@dataclass(frozen=True)
class PBXRef:
    identifier: str
    comment: str | None = None


class PBXObject:
    def __init__(self, comment: str, isa: str, **fields):
        self.comment = comment
        self.fields = OrderedDict()
        self.fields["isa"] = isa
        for key, value in fields.items():
            self.fields[key] = value


class DirNode:
    def __init__(self) -> None:
        self.subdirs: Dict[str, "DirNode"] = {}
        self.files: List[str] = []


def collect_source_tree(paths: Iterable[Path]) -> DirNode:
    root = DirNode()
    for path in sorted(paths):
        parts = list(path.parts)
        node = root
        for component in parts[:-1]:
            node = node.subdirs.setdefault(component, DirNode())
        node.files.append(parts[-1])
    for node in root.subdirs.values():
        node.files.sort()
    return root


def quote(value: str) -> str:
    bare_ok = value.isidentifier() or value.isdigit() or value in {"YES", "NO"}
    if bare_ok and "$" not in value and "." not in value and "/" not in value:
        return value
    return f'"{value}"'


def format_value(value, indent: int, comment_lookup) -> str:
    indent_str = "\t" * indent
    if isinstance(value, PBXRef):
        if value.comment:
            return f"{value.identifier} /* {value.comment} */"
        return value.identifier
    if isinstance(value, str):
        return quote(value)
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, list):
        if not value:
            return "()"
        lines = ["("]
        for item in value:
            if isinstance(item, PBXRef) and item.comment is None:
                item = PBXRef(item.identifier, comment_lookup(item.identifier))
            lines.append(
                f"\n{indent_str}\t\t{format_value(item, indent + 1, comment_lookup)},"
            )
        lines.append(f"\n{indent_str}\t")
        lines.append(")")
        return "".join(lines)
    if isinstance(value, dict):
        if not value:
            return "{}"
        lines = ["{"]
        for key, val in value.items():
            lines.append(
                f"\n{indent_str}\t{key} = {format_value(val, indent + 1, comment_lookup)};"
            )
        lines.append(f"\n{indent_str}")
        lines.append("}")
        return "".join(lines)
    raise TypeError(f"Unsupported value type: {type(value)!r}")


def format_objects(objects: Dict[str, PBXObject]) -> str:
    type_order = [
        "PBXBuildFile",
        "PBXFileReference",
        "PBXFrameworksBuildPhase",
        "PBXSourcesBuildPhase",
        "PBXGroup",
        "PBXNativeTarget",
        "PBXProject",
        "XCBuildConfiguration",
        "XCConfigurationList",
    ]

    buckets: Dict[str, List[Tuple[str, PBXObject]]] = {t: [] for t in type_order}
    for identifier, obj in objects.items():
        isa = obj.fields["isa"]
        buckets.setdefault(isa, []).append((identifier, obj))

    def comment_lookup(identifier: str) -> str | None:
        obj = objects.get(identifier)
        return obj.comment if obj else None

    lines: List[str] = []
    for isa in type_order:
        entries = buckets.get(isa, [])
        for identifier, obj in sorted(entries, key=lambda item: item[0]):
            comment_suffix = f" /* {obj.comment} */" if obj.comment else ""
            lines.append(f"\t\t{identifier}{comment_suffix} = {{")
            for key, value in obj.fields.items():
                lines.append(
                    f"\t\t\t{key} = {format_value(value, 3, comment_lookup)};"
                )
            lines.append("\t\t};")
    return "\n".join(lines)


def build_project(output_path: Path, release_build: bool) -> None:
    # Source lists mirror the default CMake configuration (SDL disabled by default).
    pscal_sources = [
        "src/Pascal/main.c",
        "src/Pascal/globals.c",
        "src/core/types.c",
        "src/core/utils.c",
        "src/core/list.c",
        "src/core/preproc.c",
        "src/core/version.c",
        "src/Pascal/lexer.c",
        "src/Pascal/parser.c",
        "src/ast/ast.c",
        "src/Pascal/opt.c",
        "src/symbol/symbol.c",
        "src/backend_ast/builtin.c",
        "src/backend_ast/builtin_network_api.c",
        "src/compiler/bytecode.c",
        "src/compiler/compiler.c",
        "src/core/cache.c",
        "src/vm/vm.c",
    ]
    ext_sources = [
        "src/ext_builtins/register.c",
        "src/ext_builtins/registry.c",
        "src/ext_builtins/query_builtin.c",
        "src/ext_builtins/dump.c",
        "src/ext_builtins/math/chudnovsky.c",
        "src/ext_builtins/math/factorial.c",
        "src/ext_builtins/math/fibonacci.c",
        "src/ext_builtins/math/mandelbrot.c",
        "src/ext_builtins/math/register.c",
        "src/ext_builtins/strings/register.c",
        "src/ext_builtins/strings/atoi.c",
        "src/ext_builtins/system/fileexists.c",
        "src/ext_builtins/system/getpid.c",
        "src/ext_builtins/system/realtimeclock.c",
        "src/ext_builtins/system/swap.c",
        "src/ext_builtins/system/register.c",
        "src/ext_builtins/yyjson/yyjson_builtins.c",
        "src/ext_builtins/yyjson/register.c",
        "src/ext_builtins/sqlite/sqlite_builtins.c",
        "src/ext_builtins/user/register.c",
        "src/third_party/yyjson/yyjson.c",
    ]

    def dedupe(seq: Iterable[str]) -> List[str]:
        seen = set()
        ordered = []
        for item in seq:
            if item not in seen:
                seen.add(item)
                ordered.append(item)
        return ordered

    pscal_full = dedupe(pscal_sources + ext_sources)
    pscalvm_sources = dedupe(
        [s for s in pscal_sources if s != "src/Pascal/main.c"]
        + ["src/vm/vm_main.c"]
        + ext_sources
    )
    pscald_sources = dedupe(
        [s for s in pscal_sources if s != "src/Pascal/main.c"]
        + ["src/disassembler/main.c"]
        + ext_sources
    )

    clike_core = [
        "src/clike/main.c",
        "src/clike/lexer.c",
        "src/clike/parser.c",
        "src/clike/ast.c",
        "src/clike/builtins.c",
        "src/clike/semantics.c",
        "src/clike/codegen.c",
        "src/clike/opt.c",
        "src/clike/preproc.c",
        "src/Pascal/globals.c",
        "src/core/utils.c",
        "src/core/types.c",
        "src/core/list.c",
        "src/core/preproc.c",
        "src/core/version.c",
        "src/core/cache.c",
        "src/compiler/bytecode.c",
        "src/vm/vm.c",
        "src/backend_ast/builtin.c",
        "src/backend_ast/builtin_network_api.c",
        "src/symbol/symbol.c",
        "src/clike/stubs.c",
    ]
    clike_sources = dedupe(clike_core + ext_sources)
    clike_repl_sources = dedupe(
        [s for s in clike_core if s != "src/clike/main.c"]
        + ["src/clike/repl.c"]
        + ext_sources
    )

    rea_core = [
        "src/rea/main.c",
        "src/rea/lexer.c",
        "src/rea/parser.c",
        "src/rea/semantic.c",
        "src/Pascal/lexer.c",
        "src/Pascal/parser.c",
        "src/ast/ast.c",
        "src/Pascal/globals.c",
        "src/core/utils.c",
        "src/core/types.c",
        "src/core/list.c",
        "src/core/preproc.c",
        "src/core/version.c",
        "src/core/cache.c",
        "src/compiler/bytecode.c",
        "src/compiler/compiler.c",
        "src/vm/vm.c",
        "src/backend_ast/builtin.c",
        "src/backend_ast/builtin_network_api.c",
        "src/symbol/symbol.c",
    ]
    rea_sources = dedupe(rea_core + ext_sources)

    json2bc_core = [
        "src/tools/json2bc.c",
        "src/tools/ast_json_loader.c",
        "src/rea/type_stubs.c",
        "src/ast/ast.c",
        "src/core/utils.c",
        "src/core/types.c",
        "src/core/list.c",
        "src/core/version.c",
        "src/core/cache.c",
        "src/compiler/bytecode.c",
        "src/compiler/compiler.c",
        "src/backend_ast/builtin.c",
        "src/backend_ast/builtin_network_api.c",
        "src/backend_ast/sdl.c",
        "src/backend_ast/sdl3d.c",
        "src/backend_ast/gl.c",
        "src/backend_ast/audio.c",
        "src/symbol/symbol.c",
        "src/vm/vm.c",
        "src/Pascal/globals.c",
    ]
    json2bc_sources = dedupe(json2bc_core + ext_sources)

    targets = OrderedDict(
        [
            (
                "pascal",
                {
                    "sources": pscal_full,
                    "debug_defs": ["FRONTEND_PASCAL", "DEBUGNOT"],
                    "release_defs": ["FRONTEND_PASCAL", "RELEASE"],
                },
            ),
            ("pscalvm", {"sources": pscalvm_sources}),
            (
                "dascal",
                {
                    "sources": pscal_full,
                    "debug_defs": ["DEBUG"],
                    "release_defs": ["DEBUG"],
                },
            ),
            ("pscald", {"sources": pscald_sources}),
            (
                "clike",
                {
                    "sources": clike_sources,
                    "debug_defs": ["FRONTEND_CLIKE"],
                    "release_defs": ["FRONTEND_CLIKE"],
                },
            ),
            (
                "clike-repl",
                {
                    "sources": clike_repl_sources,
                    "debug_defs": ["FRONTEND_CLIKE"],
                    "release_defs": ["FRONTEND_CLIKE"],
                },
            ),
            (
                "rea",
                {
                    "sources": rea_sources,
                    "debug_defs": ["FRONTEND_REA"],
                    "release_defs": ["FRONTEND_REA"],
                },
            ),
            (
                "pscal-runner",
                {
                    "sources": ["xcode/Support/pscal_runner_main.c"],
                },
            ),
            ("pscaljson2bc", {"sources": json2bc_sources}),
        ]
    )

    all_source_paths = {Path(path) for target in targets.values() for path in target["sources"]}

    objects: Dict[str, PBXObject] = {}

    def add_object(identifier: str, obj: PBXObject) -> PBXRef:
        if identifier in objects:
            raise ValueError(f"Duplicate object identifier: {identifier}")
        objects[identifier] = obj
        return PBXRef(identifier, obj.comment)

    file_refs: Dict[str, PBXRef] = {}

    def ensure_file_ref(path: Path) -> PBXRef:
        key = str(path)
        if key in file_refs:
            return file_refs[key]
        identifier = md5_id("FILE", key)
        obj = PBXObject(
            comment=path.name,
            isa="PBXFileReference",
            fileEncoding=4,
            lastKnownFileType="sourcecode.c.c",
            path=path.name,
            sourceTree="<group>",
        )
        file_refs[key] = add_object(identifier, obj)
        return file_refs[key]

    # Group tree
    dir_tree = collect_source_tree(all_source_paths)
    if "src" not in dir_tree.subdirs:
        raise RuntimeError("Expected all sources to live under src/")

    group_refs: Dict[Tuple[str, ...], PBXRef] = {}

    def emit_group(node: DirNode, path_parts: Tuple[str, ...]) -> PBXRef:
        identifier = md5_id("GROUP", "/".join(path_parts) if path_parts else "root")
        children: List[PBXRef] = []
        for name in sorted(node.subdirs):
            child = emit_group(node.subdirs[name], path_parts + (name,))
            children.append(child)
        for filename in node.files:
            child_path = Path(*path_parts, filename)
            children.append(ensure_file_ref(child_path))

        fields = {
            "children": children,
            "sourceTree": "<group>",
        }
        if path_parts:
            fields["path"] = path_parts[-1]
            fields["name"] = path_parts[-1]
        else:
            fields["name"] = PROJECT_NAME.lower()

        group_ref = add_object(identifier, PBXObject(comment=fields.get("name"), isa="PBXGroup", **fields))
        group_refs[path_parts] = group_ref
        return group_ref

    src_group = emit_group(dir_tree.subdirs["src"], ("src",))
    extra_top_groups: List[PBXRef] = []
    for name in sorted(name for name in dir_tree.subdirs if name != "src"):
        extra_top_groups.append(emit_group(dir_tree.subdirs[name], (name,)))

    # Root group and products group
    products_identifier = md5_id("GROUP", "Products")
    products_group = PBXObject(comment="Products", isa="PBXGroup", children=[], name="Products", sourceTree="<group>")
    products_ref = add_object(products_identifier, products_group)

    root_identifier = md5_id("GROUP", "")
    root_fields = {
        "children": [src_group, *extra_top_groups, products_ref],
        "sourceTree": "<group>",
        "path": "..",
        "name": PROJECT_NAME,
    }
    root_group = PBXObject(
        comment=PROJECT_NAME.lower(),
        isa="PBXGroup",
        **root_fields,
    )
    root_ref = add_object(root_identifier, root_group)

    build_files: Dict[Tuple[str, str], PBXRef] = {}

    target_refs: Dict[str, PBXRef] = {}
    target_attributes = OrderedDict()

    project_debug_id = md5_id("CONFIG", "PROJECT", "Debug")
    project_release_id = md5_id("CONFIG", "PROJECT", "Release")
    project_config_list_id = md5_id("CONFIGLIST", "PROJECT")

    program_version, git_tag = determine_version_info(release_build)

    base_preprocessor_defs = [
        "ENABLE_EXT_BUILTIN_MATH=1",
        "ENABLE_EXT_BUILTIN_STRINGS=1",
        "ENABLE_EXT_BUILTIN_SYSTEM=1",
        "ENABLE_EXT_BUILTIN_USER=1",
        "ENABLE_EXT_BUILTIN_YYJSON=1",
        "ENABLE_EXT_BUILTIN_SQLITE=1",
        "ENABLE_EXT_BUILTIN_3D=1",
        "ENABLE_EXT_BUILTIN_GRAPHICS=1",
        f'PROGRAM_VERSION=\\"{escape_define(program_version)}\\"',
        f'PSCAL_GIT_TAG=\\"{escape_define(git_tag)}\\"',
    ]
    # The generated .xcodeproj lives in xcode/ which means PROJECT_DIR points to
    # that subdirectory. Header includes in the source expect to resolve against
    # the repository root (e.g. "core/utils.h"), so the header search path must
    # explicitly reach back up to the real src/ tree.
    header_search_paths = ["$(PROJECT_DIR)/../src", "$(PROJECT_DIR)/../src/**"]
    header_search_paths.extend(SDL_HEADER_HINTS)
    header_search_paths = list(dict.fromkeys(header_search_paths))

    common_project_settings = OrderedDict(
        [
            ("ALWAYS_SEARCH_USER_PATHS", "NO"),
            ("CLANG_C_LANGUAGE_STANDARD", "c11"),
            ("CODE_SIGNING_ALLOWED", "NO"),
            ("ENABLE_BITCODE", "NO"),
            ("GCC_PREPROCESSOR_DEFINITIONS", ["$(inherited)"] + base_preprocessor_defs),
            ("HEADER_SEARCH_PATHS", header_search_paths),
            ("LIBRARY_SEARCH_PATHS", ["$(inherited)"]),
            ("MACOSX_DEPLOYMENT_TARGET", "11.0"),
            ("OTHER_CFLAGS", ["$(inherited)", "-Wall"]),
            ("OTHER_LDFLAGS", ["$(inherited)", "-lcurl", "-lsqlite3", "-lm", "-lpthread"]),
            ("SDKROOT", "macosx"),
        ]
    )

    project_debug_settings = OrderedDict(common_project_settings)
    project_debug_settings["COPY_PHASE_STRIP"] = "NO"
    project_debug_settings["GCC_DYNAMIC_NO_PIC"] = "NO"
    project_debug_settings["GCC_OPTIMIZATION_LEVEL"] = "0"
    project_debug_settings["GCC_PREPROCESSOR_DEFINITIONS"] = list(
        common_project_settings["GCC_PREPROCESSOR_DEFINITIONS"]
    ) + ["DEBUG=1"]
    project_debug_settings["ONLY_ACTIVE_ARCH"] = "YES"

    project_release_settings = OrderedDict(common_project_settings)
    project_release_settings["COPY_PHASE_STRIP"] = "YES"
    project_release_settings["GCC_OPTIMIZATION_LEVEL"] = "3"
    project_release_settings["GCC_PREPROCESSOR_DEFINITIONS"] = list(
        common_project_settings["GCC_PREPROCESSOR_DEFINITIONS"]
    ) + ["NDEBUG=1"]

    project_debug = PBXObject(
        comment="Debug",
        isa="XCBuildConfiguration",
        buildSettings=project_debug_settings,
        name="Debug",
    )

    project_release = PBXObject(
        comment="Release",
        isa="XCBuildConfiguration",
        buildSettings=project_release_settings,
        name="Release",
    )

    add_object(project_debug_id, project_debug)
    add_object(project_release_id, project_release)

    project_config_list = PBXObject(
        comment=f"Build configuration list for PBXProject \"{PROJECT_NAME}\"",
        isa="XCConfigurationList",
        buildConfigurations=[PBXRef(project_debug_id, "Debug"), PBXRef(project_release_id, "Release")],
        defaultConfigurationIsVisible="0",
        defaultConfigurationName="Release",
    )
    add_object(project_config_list_id, project_config_list)

    for target_name, data in targets.items():
        sources = data["sources"]
        source_refs = []
        for source in sources:
            build_key = (target_name, source)
            if build_key in build_files:
                source_refs.append(build_files[build_key])
                continue
            file_ref = ensure_file_ref(Path(source))
            build_identifier = md5_id("BUILD", target_name, source)
            build_file = PBXObject(
                comment=f"{Path(source).name} in Sources",
                isa="PBXBuildFile",
                fileRef=file_ref,
            )
            build_ref = add_object(build_identifier, build_file)
            build_files[build_key] = build_ref
            source_refs.append(build_ref)

        sources_phase_id = md5_id("SOURCES", target_name)
        sources_phase = PBXObject(
            comment="Sources",
            isa="PBXSourcesBuildPhase",
            buildActionMask="2147483647",
            files=source_refs,
            runOnlyForDeploymentPostprocessing="0",
        )
        add_object(sources_phase_id, sources_phase)

        frameworks_phase_id = md5_id("FRAMEWORKS", target_name)
        frameworks_phase = PBXObject(
            comment="Frameworks",
            isa="PBXFrameworksBuildPhase",
            buildActionMask="2147483647",
            files=[],
            runOnlyForDeploymentPostprocessing="0",
        )
        add_object(frameworks_phase_id, frameworks_phase)

        debug_defs = data.get("debug_defs", [])
        release_defs = data.get("release_defs", [])

        target_debug_id = md5_id("CONFIG", target_name, "Debug")
        target_release_id = md5_id("CONFIG", target_name, "Release")
        target_config_list_id = md5_id("CONFIGLIST", target_name)

        base_target_settings = OrderedDict(
            [
                ("CODE_SIGN_STYLE", "Automatic"),
                ("CODE_SIGNING_ALLOWED", "NO"),
                ("DEVELOPMENT_TEAM", ""),
                ("ENABLE_BITCODE", "NO"),
                ("HEADER_SEARCH_PATHS", ["$(inherited)"]),
                ("LIBRARY_SEARCH_PATHS", ["$(inherited)"]),
                ("OTHER_CFLAGS", ["$(inherited)"]),
                ("OTHER_LDFLAGS", ["$(inherited)"]),
                ("PRODUCT_NAME", "$(TARGET_NAME)"),
            ]
        )

        debug_settings = OrderedDict(base_target_settings)
        debug_defs_full = ["$(inherited)"] + debug_defs
        if target_name == "pascal":
            debug_settings["GCC_OPTIMIZATION_LEVEL"] = "0"
        debug_settings["GCC_PREPROCESSOR_DEFINITIONS"] = debug_defs_full

        release_settings = OrderedDict(base_target_settings)
        release_defs_full = ["$(inherited)"] + release_defs
        if target_name == "pascal":
            release_settings["GCC_OPTIMIZATION_LEVEL"] = "3"
        release_settings["GCC_PREPROCESSOR_DEFINITIONS"] = release_defs_full

        target_debug = PBXObject(
            comment="Debug",
            isa="XCBuildConfiguration",
            buildSettings=debug_settings,
            name="Debug",
        )
        target_release = PBXObject(
            comment="Release",
            isa="XCBuildConfiguration",
            buildSettings=release_settings,
            name="Release",
        )
        add_object(target_debug_id, target_debug)
        add_object(target_release_id, target_release)

        target_config_list = PBXObject(
            comment=f"Build configuration list for PBXNativeTarget \"{target_name}\"",
            isa="XCConfigurationList",
            buildConfigurations=[PBXRef(target_debug_id, "Debug"), PBXRef(target_release_id, "Release")],
            defaultConfigurationIsVisible="0",
            defaultConfigurationName="Release",
        )
        add_object(target_config_list_id, target_config_list)

        product_identifier = md5_id("PRODUCT", target_name)
        product = PBXObject(
            comment=target_name,
            isa="PBXFileReference",
            explicitFileType="compiled.mach-o.executable",
            includeInIndex="0",
            path=target_name,
            sourceTree="BUILT_PRODUCTS_DIR",
        )
        product_ref = add_object(product_identifier, product)
        products_group.fields["children"].append(product_ref)

        native_target = PBXObject(
            comment=target_name,
            isa="PBXNativeTarget",
            buildConfigurationList=PBXRef(target_config_list_id, f"Build configuration list for PBXNativeTarget \"{target_name}\""),
            buildPhases=[PBXRef(sources_phase_id, "Sources"), PBXRef(frameworks_phase_id, "Frameworks")],
            buildRules=[],
            dependencies=[],
            name=target_name,
            productName=target_name,
            productReference=product_ref,
            productType="com.apple.product-type.tool",
        )
        target_ref = add_object(md5_id("TARGET", target_name), native_target)
        target_refs[target_name] = target_ref
        target_attributes[target_ref.identifier] = OrderedDict(
            [
                ("CreatedOnToolsVersion", "15.0"),
                ("ProvisioningStyle", "Automatic"),
            ]
        )

    products_group.fields["children"].sort(key=lambda ref: ref.comment or ref.identifier)

    project_identifier = md5_id("PROJECT", PROJECT_NAME)
    project = PBXObject(
        comment=PROJECT_NAME,
        isa="PBXProject",
        attributes=OrderedDict(
            [
                ("BuildIndependentTargetsInParallel", "YES"),
                ("LastUpgradeCheck", "1500"),
                ("TargetAttributes", target_attributes),
            ]
        ),
        buildConfigurationList=PBXRef(project_config_list_id, f"Build configuration list for PBXProject \"{PROJECT_NAME}\""),
        compatibilityVersion="Xcode 14.0",
        developmentRegion="en",
        hasScannedForEncodings="0",
        knownRegions=["en", "Base"],
        mainGroup=root_ref,
        productRefGroup=products_ref,
        projectDirPath="",
        projectRoot="",
        targets=[target_refs[name] for name in targets],
    )
    add_object(project_identifier, project)

    objects_text = format_objects(objects)

    output_path.write_text(
        "// !$*UTF8*$!\n" +
        "{\n" +
        "\tarchiveVersion = 1;\n" +
        "\tclasses = {};\n" +
        "\tobjectVersion = 56;\n" +
        "\tobjects = {\n" +
        objects_text +
        "\n\t};\n" +
        f"\trootObject = {project_identifier} /* {PROJECT_NAME} */;\n" +
        "}\n",
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate the custom Xcode project")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("xcode/Pscal.xcodeproj/project.pbxproj"),
        help="Destination project.pbxproj path",
    )
    parser.add_argument(
        "--release-build",
        action="store_true",
        help="Append _REL to PROGRAM_VERSION (mirrors CMake's -DRELEASE_BUILD=ON)",
    )
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    build_project(args.output, args.release_build)


if __name__ == "__main__":
    main()
