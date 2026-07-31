#!/usr/bin/env python3
"""
compile_shaders.py
Compiles OceanFFT Slang shaders to WGSL via slangc and bakes the resulting WGSL
source into a C++ header for Emscripten / WebAssembly builds where shipping the
Slang runtime would inflate binary size far too much. I know there's some kind of
protobuf IR support for Dawn/Tint now, but I'm not diving into that just for this
demo.

Wave ops variants
-----------------
IFFT shaders are compiled twice:
  _WaveOps   — default, uses WaveReadLaneAt / WavePrefixSum
               (requires WebGPU subgroups extension, Chrome 130+ / flag)
               We do an adapter check at runtime to see if we can use this
  _NoWaveOps — compiled with -DOCEAN_FFT_DISABLE_WAVE_OPS; uses shared-memory
               butterfly passes instead
The baked C++ header includes both sets plus an inline selector:
  GetRadix2_IFFTWgsl(bool waveOps)
Spectrum shaders do not use wave ops and are compiled once.

WGSL / WebGPU caveats
---------------------
- f16 / half: requires the 'enable f16;' WGSL extension (Chrome 121+, FF Nightly).
- Texture1D:  not in WebGPU core — the PhaseRotationLUT bindings in the IFFT
  shaders must become Texture2D<f16> with height=1.
- [[vk::binding(slot, set)]] is automatically remapped to @group(set) @binding(slot)
  by slangc for the WGSL target.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
SHADER_DIR = REPO_ROOT / "shaders" / "compute"

# ---------------------------------------------------------------------------
# Shader manifest
# ---------------------------------------------------------------------------

@dataclass
class ShaderEntry:
    # Source file relative to SHADER_DIR
    source: str
    # Slang entry-point name (-entry argument)
    entry: str
    # Base output stem; wave-variant entries get _WaveOps / _NoWaveOps appended
    output_stem: str
    # If True, compile two variants: one with wave ops, one without
    wave_variants: bool = False
    # Extra slangc flags shared by all variants of this entry
    extra_flags: list[str] = field(default_factory=list)


SHADERS: list[ShaderEntry] = [
    # Spectrum shaders — no wave ops, single variant each
    ShaderEntry(
        source="OceanFft_InitSpectrum.slang",
        entry="SpectrumInitCS",
        output_stem="SpectrumInitCS",
    ),
    ShaderEntry(
        source="OceanFft_InitSpectrum.slang",
        entry="SpectrumConjugateCS",
        output_stem="SpectrumConjugateCS",
    ),
    # Radix-2 IFFT — uses wave ops, bake both variants
    ShaderEntry(
        source="OceanFft_IFFT_Radix2.slang",
        entry="Radix2_IFFT",
        output_stem="Radix2_IFFT",
        wave_variants=True,
    ),
    ShaderEntry(
        source="OceanFft_IFFT_Radix2.slang",
        entry="Radix2_IFFT_Permute",
        output_stem="Radix2_IFFT_Permute",
        wave_variants=True,
    ),
    # Radix-4 IFFT — only valid for FFT_SIZE 256 or 1024, NOT 512.
    # Uncomment and set -DFFT_SIZE appropriately when using this path.
    ShaderEntry(
         source="OceanFft_IFFT_Radix4.slang",
         entry="Radix4_IFFT",
         output_stem="Radix4_IFFT",
         wave_variants=True,
    ),
    ShaderEntry(
         source="OceanFft_IFFT_Radix4.slang",
         entry="Radix4_IFFT_Permute",
         output_stem="Radix4_IFFT_Permute",
         wave_variants=True,
    ),
]

# Record for a single compiled shader, used to make header baking a little more expressive
@dataclass
class CompiledShader:
    base_stem: str       # logical name, e.g. "Radix2_IFFT"
    file_stem: str       # actual output stem, e.g. "Radix2_IFFT_WaveOps"
    wave_ops: bool | None  # True=wave, False=no-wave, None=single variant
    path: Path


def find_slangc(hint: str | None) -> Path:
    """Return path to slangc, searching hint -> PATH -> common locations."""
    if hint:
        p = Path(hint)
        if p.is_file():
            return p
        raise FileNotFoundError(f"slangc not found at specified path: {hint}")

    found = shutil.which("slangc")
    if found:
        return Path(found)
    # ordered in preference, esp. for build outputs to hopefully make it less slow
    candidates = [
        Path(r"C:\Program Files\slang\bin\slangc.exe"),
        Path(r"C:\tools\slang\bin\slangc.exe"),
        REPO_ROOT / "third_party" / "slang" / "bin" / "slangc.exe",
        REPO_ROOT / "build" / "slang" / "Release" / "slangc.exe",
        REPO_ROOT / "build" / "slang" / "RelWithDebInfo" / "slangc.exe",
        REPO_ROOT / "build" / "slang" / "Debug" / "slangc.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c

    raise FileNotFoundError(
        "slangc not found on PATH or in common locations.\n"
        "Build Slang from third_party/slang or install it, then either:\n"
        "  a) Add its bin/ directory to PATH, or\n"
        "  b) Pass --slangc <path> to this script."
    )


def _invoke_slangc(
    slangc: Path,
    src: Path,
    entry: str,
    out: Path,
    extra_flags: list[str],
    verbose: bool,
) -> None:
    """ 
    Individual slangc invocation for a single entry-point variant - uses subprocess.run().
    Calls sys.exit() on failure.
    """
    cmd = [
        str(slangc),
        str(src),
        "-target", "wgsl",
        "-I", str(SHADER_DIR),  # lets slangc resolve the OceanFft module
        "-entry", entry,
        "-o", str(out),
        *extra_flags,
    ]
    if verbose:
        print("  $", " ".join(cmd))

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"\n[ERROR] slangc failed for {src.name}::{entry}", file=sys.stderr)
        if result.stdout:
            print(result.stdout, file=sys.stderr)
        if result.stderr:
            print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)
    if verbose and result.stdout:
        print(result.stdout.rstrip())

# Wrapper to dispatch and compile a single ShaderEntry - permuting various established as permutations
# as needed to reduce back to a kind of "ubershader" setup
def compile_entry(
    slangc: Path,
    entry: ShaderEntry,
    output_dir: Path,
    force: bool,
    verbose: bool,
) -> list[CompiledShader]:
    """Compile one manifest entry, returning 1 or 2 CompiledShader records."""
    src = SHADER_DIR / entry.source
    src_mtime = src.stat().st_mtime
    results: list[CompiledShader] = []

    if entry.wave_variants:
        variants: list[tuple[str, list[str], bool | None]] = [
            ("_WaveOps",   [],                               True),
            ("_NoWaveOps", ["-DOCEAN_FFT_DISABLE_WAVE_OPS"], False),
        ]
    else:
        variants = [("", [], None)]

    for suffix, defines, wave_flag in variants:
        file_stem = entry.output_stem + suffix
        out = output_dir / f"{file_stem}.wgsl"

        if not force and out.exists() and out.stat().st_mtime >= src_mtime:
            print(f"  [skip]  {entry.source}::{entry.entry}{suffix}  (up to date)")
        else:
            label = f"{entry.source}::{entry.entry}{suffix}"
            print(f"  Compiling {label} ...")
            output_dir.mkdir(parents=True, exist_ok=True)
            _invoke_slangc(slangc, src, entry.entry, out, entry.extra_flags + defines, verbose)
            print(f"    -> {out}")

        results.append(CompiledShader(
            base_stem=entry.output_stem,
            file_stem=file_stem,
            wave_ops=wave_flag,
            path=out,
        ))

    return results

# snippets used to construct the baked C++ header. shouldn't change much
_HEADER_TOP = """\
#pragma once
// Auto-generated by scripts/compile_shaders.py -- do not edit manually.
// Rebuild with:  python scripts/compile_shaders.py --bake-header {rel_path}

#include <string_view>

namespace OceanFFT::Shaders {{
"""

_HEADER_BOTTOM = "}} // namespace OceanFFT::Shaders\n"

_RAW_DELIM = "WGSL_END"


def _ident(stem: str) -> str:
    return "k" + re.sub(r"[^A-Za-z0-9_]", "_", stem) + "Wgsl"


def _constant(stem: str, wgsl_path: Path) -> str:
    src = wgsl_path.read_text(encoding="utf-8")
    return (
        f"inline constexpr std::string_view {_ident(stem)} ="
        f" R\"{_RAW_DELIM}(\n{src}){_RAW_DELIM}\";"
    )


def _selector(base_stem: str) -> str:
    fn = "Get" + re.sub(r"[^A-Za-z0-9_]", "_", base_stem) + "Wgsl"
    wave   = _ident(base_stem + "_WaveOps")
    nowave = _ident(base_stem + "_NoWaveOps")
    return (
        f"inline std::string_view {fn}(bool waveOps) noexcept {{\n"
        f"    return waveOps ? {wave} : {nowave};\n"
        f"}}"
    )


def bake_header(shaders: list[CompiledShader], header_path: Path, rel_path: Path) -> None:
    header_path.parent.mkdir(parents=True, exist_ok=True)

    # preserve manifest order, group by base_stem for selector generation
    groups: dict[str, list[CompiledShader]] = {}
    for s in shaders:
        groups.setdefault(s.base_stem, []).append(s)

    blocks: list[str] = []
    for base_stem, group in groups.items():
        for s in group:
            blocks.append(_constant(s.file_stem, s.path))

        # emit a runtime selector when both wave variants are present
        has_wave   = any(s.wave_ops is True  for s in group)
        has_nowave = any(s.wave_ops is False for s in group)
        if has_wave and has_nowave:
            blocks.append(_selector(base_stem))

    body = "\n\n".join(blocks) + "\n"
    content = _HEADER_TOP.format(rel_path=rel_path) + "\n" + body + "\n" + _HEADER_BOTTOM
    header_path.write_text(content, encoding="utf-8")
    print(f"  Wrote baked header -> {header_path}")


# ---------------------------------------------------------------------------
# Watch mode
# ---------------------------------------------------------------------------

def watch_and_compile(args: argparse.Namespace) -> None:
    try:
        from watchdog.observers import Observer  # type: ignore
        from watchdog.events import FileSystemEventHandler  # type: ignore
    except ImportError:
        print("[ERROR] --watch requires: pip install watchdog", file=sys.stderr)
        sys.exit(1)

    class SlangHandler(FileSystemEventHandler):
        def on_modified(self, event):
            if not event.is_directory and event.src_path.endswith(".slang"):
                print(f"\n[watch] Changed: {event.src_path}")
                run_compilation(args)

    run_compilation(args)
    observer = Observer()
    observer.schedule(SlangHandler(), str(SHADER_DIR), recursive=False)
    observer.start()
    print(f"\n[watch] Watching {SHADER_DIR}  (Ctrl-C to stop)")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        observer.stop()
    observer.join()


# ---------------------------------------------------------------------------
# Main pass
# ---------------------------------------------------------------------------

def run_compilation(args: argparse.Namespace) -> None:
    output_dir = Path(args.output_dir)
    compiled: list[CompiledShader] = []

    if not args.no_compile:
        slangc = find_slangc(args.slangc)
        print(f"Using slangc: {slangc}\n")
        for entry in SHADERS:
            compiled.extend(compile_entry(slangc, entry, output_dir, args.force, args.verbose))
    else:
        for entry in SHADERS:
            suffixes = (["_WaveOps", "_NoWaveOps"] if entry.wave_variants else [""])
            for suffix in suffixes:
                file_stem = entry.output_stem + suffix
                p = output_dir / f"{file_stem}.wgsl"
                wave_flag: bool | None = (
                    True  if suffix == "_WaveOps"   else
                    False if suffix == "_NoWaveOps" else
                    None
                )
                if p.exists():
                    compiled.append(CompiledShader(entry.output_stem, file_stem, wave_flag, p))
                else:
                    print(f"  [warn] {p} not found, skipping", file=sys.stderr)

    if args.bake_header and compiled:
        print(f"\nBaking {len(compiled)} shader variant(s) into C++ header ...")
        header_path = Path(args.bake_header)
        try:
            rel_path = header_path.relative_to(REPO_ROOT)
        except ValueError:
            rel_path = header_path
        bake_header(compiled, header_path, rel_path)

    print("\nDone.")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Compile OceanFFT Slang shaders to WGSL and optionally bake into a C++ header.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""\
            Emscripten CMake integration example
            -------------------------------------
            find_package(Python3 REQUIRED)
            add_custom_command(
                OUTPUT  ${CMAKE_SOURCE_DIR}/include/generated/OceanShaders.hpp
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/scripts/compile_shaders.py
                        --slangc <path-to-native-slangc>
                        --bake-header
                        ${CMAKE_SOURCE_DIR}/include/generated/OceanShaders.hpp
                DEPENDS ${CMAKE_SOURCE_DIR}/shaders/compute/*.slang
                COMMENT "Slang -> WGSL -> C++ header"
            )
            add_custom_target(OceanShaders ALL
                DEPENDS ${CMAKE_SOURCE_DIR}/include/generated/OceanShaders.hpp)
            add_dependencies(OceanFFT OceanShaders)
            # DO NOT link slang or slangc into the Emscripten target
        """),
    )
    p.add_argument("--slangc", metavar="PATH", default=None,
                   help="Path to slangc. Defaults to PATH search then common install locations.")
    p.add_argument("--shader-dir", metavar="DIR", default=str(SHADER_DIR),
                   help=f"Shader source directory. Default: {SHADER_DIR}")
    p.add_argument("--output-dir", metavar="DIR", default=str(REPO_ROOT / "build" / "wgsl"),
                   help="Directory for compiled .wgsl files. Default: build/wgsl/")
    p.add_argument("--bake-header", metavar="FILE", default=None,
                   help="Embed all WGSL variants into this C++ header file.")
    p.add_argument("--no-compile", action="store_true",
                   help="Skip slangc; only regenerate the header from existing .wgsl files.")
    p.add_argument("--force", action="store_true",
                   help="Recompile all shaders regardless of timestamps.")
    p.add_argument("--watch", action="store_true",
                   help="Watch shaders/compute/ for changes and recompile (requires watchdog).")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="Print slangc command lines.")
    return p


def main() -> None:
    args = build_parser().parse_args()
    global SHADER_DIR
    SHADER_DIR = Path(args.shader_dir)

    if args.watch:
        watch_and_compile(args)
    else:
        run_compilation(args)


if __name__ == "__main__":
    main()
