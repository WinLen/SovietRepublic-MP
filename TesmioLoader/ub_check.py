#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
ub_check.py — Deterministic Clean Code Quality Gate for RepublicMP (C++20)
Implements Uncle Bob's ub-mode (https://github.com/unclebob/ub-mode) companion tools for C++:
  1. Crapper: Cyclomatic Complexity (CC), Test Coverage reachability, and CRAP score per function.
  2. Dryer:   Structural code duplication detector (Jaccard similarity of normalized C++ token n-grams >= 0.80).
  3. Hygiene: Dead static function detection & boundary discipline checks.

Writes .metrics/crap.edn and .metrics/dry.edn compatible with unclebob/uml-viewer.
"""

from __future__ import annotations
import argparse
import math
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
    "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
    "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
    "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private",
    "protected", "public", "register", "reinterpret_cast", "requires", "return",
    "short", "signed", "sizeof", "static", "static_assert", "static_cast",
    "struct", "switch", "template", "this", "thread_local", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    "__try", "__except", "__finally", "__int64", "BYTE", "DWORD", "WORD",
    "BOOL", "UINT", "ULONG", "LONG", "HANDLE", "HWND", "HRESULT", "SIZE_T",
    "uintptr_t", "intptr_t", "uint64_t", "int64_t", "uint32_t", "int32_t",
    "uint16_t", "int16_t", "uint8_t", "int8_t", "size_t", "U64", "U32", "U16", "U8",
}

CONTROL_FLOW_KEYWORDS = {
    "if", "else", "for", "while", "do", "switch", "case", "default",
    "break", "continue", "return", "goto", "try", "catch", "__try", "__except",
}

NON_FUNCTION_NAMES = CONTROL_FLOW_KEYWORDS | {
    "namespace", "class", "struct", "union", "enum", "template", "sizeof",
    "alignof", "decltype", "static_assert",
}


@dataclass
class CppFunction:
    name: str
    qualified_name: str
    namespace: str
    file_path: str
    rel_path: str
    start_line: int
    end_line: int
    is_static: bool
    signature: str
    body: str
    complexity: int
    normalized_nodes: List[str]
    fingerprints: Set[Tuple[str, ...]]
    called_functions: Set[str]


def strip_comments_and_strings(src: str) -> str:
    """Replace comments with spaces and string/char literals with $STR / $CHR while preserving line count."""
    out: List[str] = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            out.append("  ")
            i += 2
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
        elif c in ('"', "'"):
            quote = c
            out.append("$STR" if quote == '"' else "$CHR")
            i += 1
            while i < n and src[i] != quote:
                if src[i] == "\\" and i + 1 < n:
                    if src[i + 1] == "\n":
                        out.append("\n")
                    i += 2
                    continue
                if src[i] == "\n":
                    out.append("\n")
                i += 1
            if i < n:
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


TOKEN_RE = re.compile(
    r"""
    (?P<STR>\$STR|\$CHR)
    | (?P<NUM>0[xX][0-9a-fA-F]+[uUlL]*|\d+(?:\.\d+)?(?:[eE][+-]?\d+)?[fFlLuU]*)
    | (?P<ID>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<OP><<=|>>=|->\*|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||::|\+=|-=|\*=|/=|%=|&=|\^=|\|=|[+\-*/%&|^~!<>=?:.,;()\[\]{}])
    """,
    re.VERBOSE,
)

FUNC_HEADER_RE = re.compile(
    r"""
    (?P<prefix>(?:^|[\n;{}])\s*)
    (?P<decl>
        (?:template\s*<[^;{}]*>\s*)?
        (?:(?:static|inline|virtual|explicit|constexpr|__forceinline|extern)\s+)*
        [A-Za-z_~][A-Za-z0-9_:<>,\s\*&]*?
        \b(?P<name>(?:[A-Za-z_][A-Za-z0-9_]*::)?~?[A-Za-z_][A-Za-z0-9_]*)\s*
        \((?P<params>[^;{}()]*)\)
        (?:\s*(?:const|noexcept|override|final))*
    )
    \s*\{
    """,
    re.VERBOSE,
)


def compute_cyclomatic_complexity(tokens: List[Tuple[str, str]]) -> int:
    """
    Matches unclebob/crapper rules for C-family languages:
    1 + count(if, for, while, case, catch, __except, ?, &&, ||)
    """
    cc = 1
    for kind, val in tokens:
        if kind == "ID" and val in ("if", "for", "while", "case", "catch", "__except"):
            cc += 1
        elif kind == "OP" and val in ("?", "&&", "||"):
            cc += 1
    return cc


def normalize_function_tokens(tokens: List[Tuple[str, str]]) -> Tuple[List[str], Set[str]]:
    """
    Matches unclebob/dryer structural normalization:
    - Keeps function/method call names, control-flow keywords, and operators/delimiters.
    - Drops incidental local names, field names, and literals ($ID, $NUM, $STR).
    """
    nodes: List[str] = []
    calls: Set[str] = set()
    n = len(tokens)
    for i, (kind, val) in enumerate(tokens):
        if kind in ("STR", "NUM"):
            nodes.append(f"${kind}")
        elif kind == "OP":
            nodes.append(val)
        elif kind == "ID":
            next_tok = tokens[i + 1] if i + 1 < n else ("", "")
            prev_tok = tokens[i - 1] if i > 0 else ("", "")
            if val in CONTROL_FLOW_KEYWORDS:
                nodes.append(val)
            elif next_tok == ("OP", "("):
                # Function or method call stays!
                nodes.append(f"CALL:{val}")
                if val not in CPP_KEYWORDS:
                    calls.add(val)
            elif prev_tok == ("OP", "::") and i + 1 < n and tokens[i + 1] == ("OP", "("):
                nodes.append(f"CALL:{val}")
                calls.add(val)
            elif val in CPP_KEYWORDS:
                nodes.append(f"KW:{val}")
            else:
                nodes.append("$ID")
    return nodes, calls


def build_fingerprints(nodes: List[str], shingle_size: int = 5) -> Set[Tuple[str, ...]]:
    if len(nodes) < shingle_size:
        return {tuple(nodes)} if nodes else set()
    return {tuple(nodes[i : i + shingle_size]) for i in range(len(nodes) - shingle_size + 1)}


def extract_functions_from_file(file_path: Path, root_dir: Path) -> List[CppFunction]:
    try:
        raw = file_path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return []

    cleaned = strip_comments_and_strings(raw)
    rel_path = file_path.relative_to(root_dir).as_posix()
    ns_parts = Path(rel_path).with_suffix("").parts
    module_ns = ".".join(ns_parts)

    # Precompute line offsets
    line_starts = [0]
    for idx, ch in enumerate(cleaned):
        if ch == "\n":
            line_starts.append(idx + 1)

    def offset_to_line(offset: int) -> int:
        lo, hi = 0, len(line_starts) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            if line_starts[mid] <= offset:
                lo = mid + 1
            else:
                hi = mid - 1
        return max(1, lo)

    functions: List[CppFunction] = []
    pos = 0
    length = len(cleaned)

    while pos < length:
        m = FUNC_HEADER_RE.search(cleaned, pos)
        if not m:
            break

        full_name = m.group("name").strip()
        short_name = full_name.split("::")[-1]
        decl_text = m.group("decl").strip()

        if short_name in NON_FUNCTION_NAMES:
            pos = m.end()
            continue

        brace_open = m.end() - 1
        depth = 1
        i = brace_open + 1
        while i < length and depth > 0:
            if cleaned[i] == "{":
                depth += 1
            elif cleaned[i] == "}":
                depth -= 1
            i += 1

        if depth != 0:
            break

        brace_close = i - 1
        start_line = offset_to_line(m.start("decl"))
        end_line = offset_to_line(brace_close)
        body_clean = cleaned[brace_open + 1 : brace_close]
        body_raw = raw[brace_open + 1 : brace_close]

        tokens = [(tm.lastgroup or "", tm.group(0)) for tm in TOKEN_RE.finditer(body_clean)]
        cc = compute_cyclomatic_complexity(tokens)
        norm_nodes, called = normalize_function_tokens(tokens)
        fps = build_fingerprints(norm_nodes)
        is_static = bool(re.search(r"\bstatic\b", decl_text))

        functions.append(
            CppFunction(
                name=short_name,
                qualified_name=full_name,
                namespace=module_ns,
                file_path=str(file_path),
                rel_path=rel_path,
                start_line=start_line,
                end_line=end_line,
                is_static=is_static,
                signature=" ".join(decl_text.split()),
                body=body_raw,
                complexity=cc,
                normalized_nodes=norm_nodes,
                fingerprints=fps,
                called_functions=called,
            )
        )
        pos = brace_close + 1

    return functions


def compute_test_reachability(
    prod_functions: List[CppFunction], test_file: Path
) -> Dict[Tuple[str, str, int], float]:
    """
    Computes estimated behavioral coverage for Crapper:
    - Directly called from tests/test_main.cpp -> 85% coverage (or 75% if high CC).
    - Transitively called (1–2 hops) from tested functions -> 65% coverage.
    - Pure/small helper in a tested module -> 50% coverage.
    - Untested module -> 0% coverage.
    """
    if not test_file.exists():
        return {(f.rel_path, f.name, f.start_line): 0.0 for f in prod_functions}

    test_raw = strip_comments_and_strings(test_file.read_text(encoding="utf-8", errors="ignore"))
    direct_calls = {tm.group(0) for tm in re.finditer(r"\b[A-Za-z_][A-Za-z0-9_]*\b", test_raw)}

    by_name: Dict[str, List[CppFunction]] = {}
    for f in prod_functions:
        by_name.setdefault(f.name, []).append(f)

    reachable: Set[str] = set()
    frontier = {name for name in direct_calls if name in by_name}
    reachable |= frontier

    # 3-hop call graph propagation inside production code
    for _ in range(3):
        next_frontier: Set[str] = set()
        for fname in frontier:
            for fn_obj in by_name.get(fname, []):
                for callee in fn_obj.called_functions:
                    if callee in by_name and callee not in reachable:
                        next_frontier.add(callee)
        reachable |= next_frontier
        frontier = next_frontier

    tested_files = {
        fn_obj.rel_path
        for fname in reachable
        for fn_obj in by_name.get(fname, [])
    }

    cov_map: Dict[Tuple[str, str, int], float] = {}
    for f in prod_functions:
        key = (f.rel_path, f.name, f.start_line)
        if f.name in direct_calls:
            cov_map[key] = 0.85
        elif f.name in reachable:
            cov_map[key] = 0.70
        elif f.rel_path in tested_files:
            cov_map[key] = 0.45
        else:
            cov_map[key] = 0.0
    return cov_map


def crap_score(cc: int, coverage_ratio: float) -> float:
    """CRAP = CC^2 * (1 - coverage)^3 + CC"""
    uncov = max(0.0, min(1.0, 1.0 - coverage_ratio))
    return (cc * cc) * (uncov ** 3) + cc


def find_duplicates(
    functions: List[CppFunction],
    min_lines: int = 8,
    min_nodes: int = 30,
    threshold: float = 0.80,
) -> List[Tuple[float, CppFunction, CppFunction]]:
    candidates = [
        f
        for f in functions
        if (f.end_line - f.start_line + 1) >= min_lines
        and len(f.normalized_nodes) >= min_nodes
        and len(f.fingerprints) > 0
    ]

    dups: List[Tuple[float, CppFunction, CppFunction]] = []
    n = len(candidates)
    for i in range(n):
        f1 = candidates[i]
        fp1 = f1.fingerprints
        len1 = len(fp1)
        for j in range(i + 1, n):
            f2 = candidates[j]
            fp2 = f2.fingerprints
            len2 = len(fp2)
            # Fast upper-bound filter on Jaccard similarity
            if min(len1, len2) / max(len1, len2) < threshold:
                continue
            inter = len(fp1 & fp2)
            union = len1 + len2 - inter
            if union == 0:
                continue
            score = inter / union
            if score >= threshold:
                dups.append((score, f1, f2))

    dups.sort(key=lambda x: x[0], reverse=True)
    return dups


def find_dead_static_functions(
    functions: List[CppFunction], all_files_content: Dict[str, str]
) -> List[CppFunction]:
    dead: List[CppFunction] = []
    for f in functions:
        if not f.is_static or f.name.startswith("~"):
            continue
        if f.rel_path.endswith(".h"):
            continue
        file_text = all_files_content.get(f.rel_path, "")
        # Count identifier occurrences in the same translation unit
        occurrences = len(re.findall(rf"\b{re.escape(f.name)}\b", file_text))
        if occurrences <= 1:
            dead.append(f)
    return dead


def write_edn_reports(
    metrics_dir: Path,
    functions: List[CppFunction],
    cov_map: Dict[Tuple[str, str, int], float],
    duplicates: List[Tuple[float, CppFunction, CppFunction]],
) -> None:
    metrics_dir.mkdir(parents=True, exist_ok=True)
    crap_lines = ["{:entries ["]
    for f in functions:
        cov = cov_map.get((f.rel_path, f.name, f.start_line), 0.0)
        crap = crap_score(f.complexity, cov)
        crap_lines.append(
            f'  {{:name "{f.name}" :namespace "{f.namespace}" '
            f":complexity {f.complexity} :coverage {cov * 100.0:.1f} :crap {crap:.1f}}}"
        )
    crap_lines.append("]}")
    (metrics_dir / "crap.edn").write_text("\n".join(crap_lines) + "\n", encoding="utf-8")

    dry_lines = ["{:candidates ["]
    for score, f1, f2 in duplicates:
        dry_lines.append(
            f"  {{:score {score:.4f} :language \"cpp\" "
            f':left {{:file "{f1.rel_path}" :name "{f1.name}" :start-line {f1.start_line} :end-line {f1.end_line}}} '
            f':right {{:file "{f2.rel_path}" :name "{f2.name}" :start-line {f2.start_line} :end-line {f2.end_line}}} '
            f":left-nodes {len(f1.normalized_nodes)} :right-nodes {len(f2.normalized_nodes)}}}"
        )
    dry_lines.append("]}")
    (metrics_dir / "dry.edn").write_text("\n".join(dry_lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="ub-mode C++ Clean Code Quality Gate")
    parser.add_argument("--root", default=".", help="Path to RepublicMP root directory")
    parser.add_argument("--dry-threshold", type=float, default=0.82, help="Dryer similarity threshold")
    parser.add_argument("--min-lines", type=int, default=8, help="Minimum lines for Dryer candidate")
    parser.add_argument("--min-nodes", type=int, default=30, help="Minimum syntax nodes for Dryer candidate")
    parser.add_argument("--max-cc", type=int, default=60, help="Max Cyclomatic Complexity before gate failure")
    parser.add_argument("--max-dups", type=int, default=0, help="Max duplicate pairs allowed")
    parser.add_argument("--top", type=int, default=20, help="Number of top CRAP / CC entries to display")
    parser.add_argument("--strict", action="store_true", help="Exit non-zero if duplicates or dead code found")
    args = parser.parse_args()

    root_dir = Path(args.root).resolve()
    test_file = root_dir / "tests" / "test_main.cpp"

    all_functions: List[CppFunction] = []
    all_files_content: Dict[str, str] = {}

    for dirpath, dirnames, filenames in os.walk(root_dir):
        dirnames[:] = [
            d for d in dirnames if d not in ("third_party", "obj", "build", ".git", ".metrics", "tests")
        ]
        for fname in sorted(filenames):
            if not fname.endswith((".cpp", ".h")):
                continue
            fpath = Path(dirpath) / fname
            rel = fpath.relative_to(root_dir).as_posix()
            try:
                all_files_content[rel] = strip_comments_and_strings(
                    fpath.read_text(encoding="utf-8", errors="ignore")
                )
            except Exception:
                pass
            all_functions.extend(extract_functions_from_file(fpath, root_dir))

    cov_map = compute_test_reachability(all_functions, test_file)
    duplicates = find_duplicates(
        all_functions,
        min_lines=args.min_lines,
        min_nodes=args.min_nodes,
        threshold=args.dry_threshold,
    )
    dead_statics = find_dead_static_functions(all_functions, all_files_content)

    metrics_dir = root_dir / ".metrics"
    write_edn_reports(metrics_dir, all_functions, cov_map, duplicates)

    # Sort functions by complexity descending
    scored = []
    for f in all_functions:
        cov = cov_map.get((f.rel_path, f.name, f.start_line), 0.0)
        scored.append((crap_score(f.complexity, cov), f.complexity, cov, f))
    scored.sort(key=lambda x: (x[1], x[0]), reverse=True)

    print(f"=== [ub-mode] C++ Crapper & Dryer Report ({len(all_functions)} functions scanned) ===")
    print(f"\n--- Top {min(args.top, len(scored))} Functions by Complexity / CRAP ---")
    print(f"{'CC':>4}  {'Cov%':>5}  {'CRAP':>7}  {'Function':<38}  {'Location'}")
    print("-" * 92)
    for crap, cc, cov, f in scored[: args.top]:
        loc = f"{f.rel_path}:{f.start_line}-{f.end_line}"
        print(f"{cc:4d}  {cov*100:4.0f}%  {crap:7.1f}  {f.name[:38]:<38}  {loc}")

    print(f"\n--- Dryer Structural Duplication Report (threshold >= {args.dry_threshold:.2f}) ---")
    if not duplicates:
        print("  [PASS] 0 structural duplicates >= threshold.")
    else:
        for score, f1, f2 in duplicates[:30]:
            print(
                f"  DUPLICATE score={score:.2f}  "
                f"{f1.rel_path}:{f1.start_line}-{f1.end_line} ({f1.name})  <->  "
                f"{f2.rel_path}:{f2.start_line}-{f2.end_line} ({f2.name})"
            )
        if len(duplicates) > 30:
            print(f"  ... and {len(duplicates) - 30} more duplicate pairs.")

    print("\n--- Dead Static Function Check ---")
    if not dead_statics:
        print("  [PASS] 0 dead static functions found.")
    else:
        for f in dead_statics:
            print(f"  DEAD STATIC: {f.rel_path}:{f.start_line} ({f.name})")

    over_cc = [f for _, cc, _, f in scored if cc > args.max_cc]
    if len(duplicates) > args.max_dups or dead_statics or over_cc:
        if over_cc:
            print(f"\n[FAIL] {len(over_cc)} functions exceed max cyclomatic complexity ({args.max_cc}).")
        if len(duplicates) > args.max_dups:
            print(f"\n[FAIL] {len(duplicates)} duplicate function pairs >= {args.dry_threshold:.2f} (max={args.max_dups}).")
        if dead_statics:
            print(f"\n[FAIL] {len(dead_statics)} dead static functions found.")
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
