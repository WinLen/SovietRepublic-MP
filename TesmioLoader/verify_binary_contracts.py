#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""
verify_binary_contracts.py — Binary, IAT, Hook-Collision, Register-Contract,
and Citizen-Init Call-Coverage Verifier for RepublicMP / SOVIET64.exe (pure stdlib).

Checks:
  1. PE TimeDateStamp vs kSupportedVersions in TesmioLoader/src/tesmiolauncher.cpp
  2. Cross-module write-site & TsmRoutineSig collision check + duplicate inline hooks
     (runs unconditionally even without SOVIET64.exe)
  3. 100% .text byte verification of all Expect/Prologue arrays, TsmRoutineSig tables,
     sim_patches, session_patches, kStepCalls, kPickRaySites, kPresentationSites,
     kToolCalls, game_calls kSites, kTestbedPatches, brush_tools, hover_tools,
     two_states_research, road_sync, and tesmioloader P_MENU_VERSION_SITE
  4. 100% IAT 0x8Bxxxx resolution against PE Import Directory
  5. Exact x64 register/stack/displacement contracts (BUY_PRESS_SITE + 0x422E60,
     DEPOT_MARK_RETURN + 0x2CE7F0, CountPopulation, FillingrStart)
  6. Exhaustive .text scan for E8/E9 calls to CITIZEN_INIT_RVA (0x865C00)
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


# ---------------------------------------------------------------------------
# PE32+ Parser
# ---------------------------------------------------------------------------

@dataclass
class PeSection:
    name: str
    virtual_size: int
    virtual_address: int
    raw_size: int
    raw_offset: int
    characteristics: int


class PeFile:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if len(self.data) < 0x100 or self.data[:2] != b"MZ":
            raise ValueError(f"Not a valid MZ executable: {path}")
        pe_off = struct.unpack_from("<I", self.data, 0x3C)[0]
        if pe_off + 24 > len(self.data) or self.data[pe_off:pe_off + 4] != b"PE\0\0":
            raise ValueError(f"Invalid PE header in {path}")
        self.pe_offset = pe_off
        self.machine = struct.unpack_from("<H", self.data, pe_off + 4)[0]
        self.num_sections = struct.unpack_from("<H", self.data, pe_off + 6)[0]
        self.time_date_stamp = struct.unpack_from("<I", self.data, pe_off + 8)[0]
        opt_size = struct.unpack_from("<H", self.data, pe_off + 20)[0]
        opt_off = pe_off + 24
        if opt_off + 112 > len(self.data) or opt_off + opt_size > len(self.data):
            raise ValueError(f"Truncated PE optional header in {path}")
        magic = struct.unpack_from("<H", self.data, opt_off)[0]
        if magic != 0x20B:
            raise ValueError(f"Expected PE32+ (0x20B), got 0x{magic:X}")
        self.image_base = struct.unpack_from("<Q", self.data, opt_off + 24)[0]
        num_dirs = struct.unpack_from("<I", self.data, opt_off + 108)[0]
        dirs_off = opt_off + 112
        dir_count = min(num_dirs, 16)
        if dirs_off + dir_count * 8 > opt_off + opt_size or dirs_off + dir_count * 8 > len(self.data):
            raise ValueError(f"Truncated PE data directories in {path}")
        self.data_dirs: List[Tuple[int, int]] = []
        for i in range(dir_count):
            rva, sz = struct.unpack_from("<II", self.data, dirs_off + i * 8)
            self.data_dirs.append((rva, sz))

        sec_off = opt_off + opt_size
        if sec_off + self.num_sections * 40 > len(self.data):
            raise ValueError(f"Truncated PE section table in {path}")
        self.sections: List[PeSection] = []
        for i in range(self.num_sections):
            o = sec_off + i * 40
            raw_name = self.data[o:o + 8].split(b"\0", 1)[0].decode("ascii", errors="replace")
            vsize, va, rsize, roff = struct.unpack_from("<4I", self.data, o + 8)
            chars = struct.unpack_from("<I", self.data, o + 36)[0]
            self.sections.append(PeSection(raw_name, vsize, va, rsize, roff, chars))

        self.iat_rva, self.iat_size = self.data_dirs[12] if len(self.data_dirs) > 12 else (0, 0)
        self.iat_map: Dict[int, str] = {}
        self._parse_imports()

    def _find_backed_section(self, rva: int) -> Optional[PeSection]:
        for s in self.sections:
            if s.raw_size == 0:
                continue
            delta = rva - s.virtual_address
            if 0 <= delta < s.raw_size and (s.raw_offset + delta) < len(self.data):
                return s
        return None

    def rva_to_offset(self, rva: int) -> Optional[int]:
        s = self._find_backed_section(rva)
        if s is None:
            return None
        return s.raw_offset + (rva - s.virtual_address)

    def read_rva(self, rva: int, length: int) -> Optional[bytes]:
        if length < 0:
            return None
        s = self._find_backed_section(rva)
        if s is None:
            return None
        delta = rva - s.virtual_address
        if delta + length > s.raw_size:
            return None
        off = s.raw_offset + delta
        if off + length > len(self.data):
            return None
        return self.data[off:off + length]

    def read_u32_rva(self, rva: int) -> Optional[int]:
        b = self.read_rva(rva, 4)
        return struct.unpack_from("<I", b, 0)[0] if b else None

    def read_i32_rva(self, rva: int) -> Optional[int]:
        b = self.read_rva(rva, 4)
        return struct.unpack_from("<i", b, 0)[0] if b else None

    def read_cstr_rva(self, rva: int, max_len: int = 256) -> str:
        s = self._find_backed_section(rva)
        if s is None:
            return ""
        delta = rva - s.virtual_address
        off = s.raw_offset + delta
        sec_end = min(len(self.data), s.raw_offset + s.raw_size, off + max_len)
        end = self.data.find(b"\0", off, sec_end)
        if end < 0:
            end = sec_end
        return self.data[off:end].decode("ascii", errors="replace")

    def read_wstr_rva(self, rva: int, max_chars: int = 128) -> str:
        raw = self.read_rva(rva, max_chars * 2)
        if raw is None:
            raw_sec = self._find_backed_section(rva)
            if raw_sec is None:
                return ""
            delta = rva - raw_sec.virtual_address
            avail = min(max_chars * 2, raw_sec.raw_size - delta, len(self.data) - (raw_sec.raw_offset + delta))
            raw = self.data[raw_sec.raw_offset + delta:raw_sec.raw_offset + delta + max(0, avail)]
        chars = []
        for i in range(0, len(raw) - 1, 2):
            val = struct.unpack_from("<H", raw, i)[0]
            if val == 0:
                break
            chars.append(chr(val))
        return "".join(chars)

    def section_by_name(self, name: str) -> Optional[PeSection]:
        for s in self.sections:
            if s.name == name:
                return s
        return None

    def _parse_imports(self) -> None:
        if len(self.data_dirs) <= 1:
            return
        imp_rva, imp_size = self.data_dirs[1]
        if imp_rva == 0 or imp_size == 0:
            return
        max_descs = imp_size // 20
        for idx in range(max_descs):
            desc_bytes = self.read_rva(imp_rva + idx * 20, 20)
            if desc_bytes is None:
                break
            ilt_rva, ts, _fwd, name_rva, iat_rva = struct.unpack_from("<5I", desc_bytes, 0)
            if ilt_rva == 0 and name_rva == 0 and iat_rva == 0:
                break
            if iat_rva == 0 or name_rva == 0:
                continue
            dll_name = self.read_cstr_rva(name_rva)
            if ilt_rva == 0 and ts != 0:
                continue  # Bound IAT without ILT stores raw timestamps/addresses, not Hint/Name RVAs
            thunk_rva = ilt_rva if ilt_rva != 0 else iat_rva
            slot_idx = 0
            while True:
                t_bytes = self.read_rva(thunk_rva + slot_idx * 8, 8)
                if t_bytes is None:
                    break
                val = struct.unpack_from("<Q", t_bytes, 0)[0]
                if val == 0:
                    break
                slot_rva = iat_rva + slot_idx * 8
                if val & (1 << 63):
                    sym = f"Ordinal#{val & 0xFFFF}"
                else:
                    hint_rva = val & 0x7FFFFFFF
                    sym = self.read_cstr_rva(hint_rva + 2, 512)
                self.iat_map[slot_rva] = f"{dll_name}!{sym}"
                slot_idx += 1


# ---------------------------------------------------------------------------
# C++ Source Preprocessor & Integer Expression Evaluator
# ---------------------------------------------------------------------------

def strip_cpp_comments(src: str) -> str:
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
            q = c
            out.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i:i + 2])
                    i += 2
                    continue
                out.append(src[i])
                i += 1
            if i < n:
                out.append(src[i])
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def join_line_continuations(src: str) -> str:
    return re.sub(r"\\\r?\n", " ", src)


INT_SUFFIX_RE = re.compile(r"\b(0[xX][0-9A-Fa-f]+|\d+)[uUlL]+\b")
CAST_RE = re.compile(r"\(\s*(?:unsigned(?:\s+int|\s+long|\s+__int64)?|int|uint32_t|uint64_t|int32_t|size_t|uintptr_t|BYTE)\s*\)")


def _eval_ast(node: ast.AST, env: Dict[str, int]) -> int:
    if isinstance(node, ast.Expression):
        return _eval_ast(node.body, env)
    if isinstance(node, ast.Constant) and isinstance(node.value, int) and not isinstance(node.value, bool):
        return int(node.value)
    if isinstance(node, ast.Name):
        if node.id in env:
            return env[node.id]
        raise KeyError(node.id)
    if isinstance(node, ast.UnaryOp):
        v = _eval_ast(node.operand, env)
        if isinstance(node.op, ast.UAdd):
            return +v
        if isinstance(node.op, ast.USub):
            return -v
        if isinstance(node.op, ast.Invert):
            return ~v
        raise ValueError(f"Unsupported unary op: {node.op}")
    if isinstance(node, ast.BinOp):
        l = _eval_ast(node.left, env)
        r = _eval_ast(node.right, env)
        if isinstance(node.op, ast.Add):
            return l + r
        if isinstance(node.op, ast.Sub):
            return l - r
        if isinstance(node.op, ast.Mult):
            return l * r
        if isinstance(node.op, ast.FloorDiv) or isinstance(node.op, ast.Div):
            return l // r
        if isinstance(node.op, ast.LShift):
            return l << r
        if isinstance(node.op, ast.RShift):
            return l >> r
        if isinstance(node.op, ast.BitOr):
            return l | r
        if isinstance(node.op, ast.BitAnd):
            return l & r
        if isinstance(node.op, ast.BitXor):
            return l ^ r
    raise ValueError(f"Unsupported AST node: {ast.dump(node)}")


def eval_int_expr(expr: str, env: Dict[str, int]) -> Optional[int]:
    s = expr.strip()
    if not s or '"' in s or "'" in s or "." in s:
        return None
    s = CAST_RE.sub("", s)
    s = INT_SUFFIX_RE.sub(r"\1", s)
    try:
        tree = ast.parse(s, mode="eval")
        return _eval_ast(tree, env)
    except Exception:
        return None


DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\(([^)]*)\))?\s+(.*)$", re.MULTILINE)
CONST_INT_RE = re.compile(
    r"\b(?:static\s+)?(?:constexpr|const)\s+(?:unsigned(?:\s+int|\s+long)?|int|uint32_t|int32_t|size_t|uintptr_t|DWORD|BYTE)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;{}]+);"
)


def collect_constants(clean_src: str, base_env: Optional[Dict[str, int]] = None) -> Dict[str, int]:
    local_env: Dict[str, int] = {}
    pending: List[Tuple[str, str]] = []
    for m in DEFINE_RE.finditer(clean_src):
        if m.group(2) is not None:
            continue
        name, val = m.group(1), m.group(3).strip()
        if val:
            pending.append((name, val))
    for m in CONST_INT_RE.finditer(clean_src):
        pending.append((m.group(1), m.group(2).strip()))

    combined_base: Dict[str, int] = dict(base_env) if base_env else {}
    for _ in range(8):
        progressed = False
        eval_env = dict(combined_base)
        eval_env.update(local_env)
        for name, expr in pending:
            if name in local_env:
                continue
            val = eval_int_expr(expr, eval_env)
            if val is not None:
                local_env[name] = val
                progressed = True
        if not progressed:
            break
    merged = dict(combined_base)
    merged.update(local_env)
    return merged


def expand_nop_macros(inner: str, env: Dict[str, int]) -> Optional[bytes]:
    nop_map = {
        "NOP2": [0x66, 0x90],
        "NOP5": [0x0F, 0x1F, 0x44, 0x00, 0x00],
        "NOP6": [0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00],
        "NOP8": [0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00],
        "NOP21": [0x90] * 21,
        "NOP23": [0x90] * 23,
    }
    out: List[int] = []
    for tok in inner.split(","):
        t = tok.strip()
        if not t:
            continue
        if t in nop_map:
            out.extend(nop_map[t])
        else:
            v = eval_int_expr(t, env)
            if v is None or not (0 <= v <= 0xFF):
                return None
            out.append(v)
    return bytes(out)


def split_top_level_braced_entries(block: str) -> List[str]:
    entries: List[str] = []
    depth = 0
    start = -1
    in_str = False
    i = 0
    while i < len(block):
        ch = block[i]
        if ch == '"' and (i == 0 or block[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str:
            if ch == "{":
                if depth == 0:
                    start = i
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0 and start >= 0:
                    entries.append(block[start + 1:i])
                    start = -1
        i += 1
    return entries


def split_top_level_commas(text: str) -> List[str]:
    parts: List[str] = []
    depth_brace = 0
    depth_paren = 0
    in_str = False
    start = 0
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == '"' and (i == 0 or text[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str:
            if ch == "{":
                depth_brace += 1
            elif ch == "}":
                depth_brace -= 1
            elif ch == "(":
                depth_paren += 1
            elif ch == ")":
                depth_paren -= 1
            elif ch == "," and depth_brace == 0 and depth_paren == 0:
                parts.append(text[start:i].strip())
                start = i + 1
        i += 1
    tail = text[start:].strip()
    if tail:
        parts.append(tail)
    return parts


def find_matching_brace(text: str, open_idx: int) -> int:
    depth = 0
    in_str = False
    i = open_idx
    while i < len(text):
        ch = text[i]
        if ch == '"' and (i == 0 or text[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str:
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


def find_matching_paren(text: str, open_idx: int) -> int:
    depth = 0
    in_str = False
    i = open_idx
    while i < len(text):
        ch = text[i]
        if ch == '"' and (i == 0 or text[i - 1] != "\\"):
            in_str = not in_str
        elif not in_str:
            if ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    return i
        i += 1
    return -1


# ---------------------------------------------------------------------------
# C++ Contract Extraction Data Structures
# ---------------------------------------------------------------------------

@dataclass
class WriteInterval:
    rva: int
    size: int
    kind: str  # "inline_hook", "patch", "call_relay", "iat_call_relay"
    label: str
    file: str

    @property
    def end(self) -> int:
        return self.rva + self.size


@dataclass
class ByteCheck:
    rva: int
    expected: bytes
    label: str
    file: str


@dataclass
class SigEntry:
    rva: int
    expected: bytes
    label: str
    file: str

    @property
    def end(self) -> int:
        return self.rva + len(self.expected)


@dataclass
class CallTargetCheck:
    site_rva: int
    target_rva: int
    label: str
    file: str


@dataclass
class PickRayCheck:
    site_rva: int
    expected: bytes
    file: str


@dataclass
class ExtractedContracts:
    supported_stamps: List[Tuple[str, int]]
    write_intervals: List[WriteInterval]
    byte_checks: List[ByteCheck]
    routine_sigs: List[SigEntry]
    call_checks: List[CallTargetCheck]
    iat_call_checks: List[CallTargetCheck]
    pick_ray_checks: List[PickRayCheck]
    presentation_sites: List[Tuple[int, str]]
    iat_literals: List[Tuple[int, str, int]]  # (rva, rel_file, line_no)
    session_citizen_init_sites: Set[int]


def extract_byte_arrays(clean_src: str, env: Dict[str, int], include_static: bool = True) -> Dict[str, bytes]:
    arrays: Dict[str, bytes] = {}
    pat = re.compile(
        r"\b(?P<static>static\s+)?(?:const\s+)?(?:unsigned\s+char|BYTE|uint8_t)\s+(?P<name>k[A-Za-z0-9_]+)\s*\[\s*\d*\s*\]\s*=\s*\{"
    )
    for m in pat.finditer(clean_src):
        if not include_static and m.group("static"):
            continue
        name = m.group("name")
        open_idx = m.end() - 1
        close_idx = find_matching_brace(clean_src, open_idx)
        if close_idx < 0:
            continue
        inner = clean_src[open_idx + 1:close_idx]
        bs = expand_nop_macros(inner, env)
        if bs is not None and len(bs) > 0:
            arrays[name] = bs
    return arrays


def resolve_rva_arg(arg: str, env: Dict[str, int], clean_prefix: str = "") -> Optional[int]:
    s = arg.strip()
    m = re.match(r"^(?:g_exeBase|base)\s*\+\s*(.+)$", s)
    if m:
        return eval_int_expr(m.group(1), env)
    direct = eval_int_expr(s, env)
    if direct is not None:
        return direct
    if clean_prefix and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", s):
        pat = re.compile(
            rf"\b(?:const\s+)?(?:BYTE|unsigned\s+char|uint8_t|void)\s*\*\s*{re.escape(s)}\s*=\s*(?:g_exeBase|base)\s*\+\s*([^;]+);"
        )
        matches = list(pat.finditer(clean_prefix))
        if matches:
            return eval_int_expr(matches[-1].group(1), env)
    return None


def resolve_expect_len(arg: str, arr_bytes: bytes, env: Dict[str, int]) -> int:
    s = arg.strip()
    if s.startswith("sizeof(") and s.endswith(")"):
        return len(arr_bytes)
    v = eval_int_expr(s, env)
    return v if v is not None else len(arr_bytes)


def extract_supported_versions(tesmio_dir: Path) -> List[Tuple[str, int]]:
    launcher = tesmio_dir / "src" / "tesmiolauncher.cpp"
    raw = launcher.read_text(encoding="utf-8", errors="replace")
    clean = strip_cpp_comments(join_line_continuations(raw))
    m = re.search(r"kSupportedVersions\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        return []
    close_idx = find_matching_brace(clean, m.end() - 1)
    block = clean[m.end():close_idx]
    out: List[Tuple[str, int]] = []
    for entry in split_top_level_braced_entries(block):
        fields = split_top_level_commas(entry)
        if len(fields) >= 3:
            stamp = eval_int_expr(fields[1], {})
            ver = fields[2].strip().lstrip("L").strip('"')
            if stamp is not None:
                out.append((ver, stamp))
    return out


def extract_all_contracts(tesmio_dir: Path) -> ExtractedContracts:
    rep_dir = tesmio_dir / "RepublicMP"
    src_dir = tesmio_dir / "src"

    # Pass 1: Collect global header constants across RepublicMP + src
    header_files: List[Path] = []
    for root in (rep_dir, src_dir):
        for p in sorted(root.rglob("*.h")):
            if any(x in p.parts for x in ("tests", "third_party", "build", "obj", "plugins")):
                continue
            header_files.append(p)

    global_env: Dict[str, int] = {}
    header_texts = [strip_cpp_comments(join_line_continuations(p.read_text(encoding="utf-8", errors="replace")))
                    for p in header_files]
    for _ in range(4):
        for txt in header_texts:
            global_env = collect_constants(txt, global_env)

    # Collect source files (.cpp + .h)
    cpp_files: List[Path] = []
    for root in (rep_dir, src_dir):
        for p in sorted(root.rglob("*")):
            if p.suffix not in (".cpp", ".h"):
                continue
            if any(x in p.parts for x in ("tests", "third_party", "build", "obj", "plugins")):
                continue
            cpp_files.append(p)

    res = ExtractedContracts(
        supported_stamps=extract_supported_versions(tesmio_dir),
        write_intervals=[],
        byte_checks=[],
        routine_sigs=[],
        call_checks=[],
        iat_call_checks=[],
        pick_ray_checks=[],
        presentation_sites=[],
        iat_literals=[],
        session_citizen_init_sites=set(),
    )

    # Shared byte arrays: headers + non-static external-linkage arrays in .cpp (e.g. local_builds.cpp kRoadCommitExpect)
    shared_arrays: Dict[str, bytes] = {}
    for p in cpp_files:
        raw = p.read_text(encoding="utf-8", errors="replace")
        clean = strip_cpp_comments(join_line_continuations(raw))
        env = collect_constants(clean, global_env)
        shared_arrays.update(extract_byte_arrays(clean, env, include_static=(p.suffix == ".h")))

    iat_lit_re = re.compile(r"\b(0[xX]0*8[bB][0-9A-Fa-f]{4})(?:[uUlL]*)\b")

    for p in cpp_files:
        rel = str(p.relative_to(tesmio_dir))
        raw = p.read_text(encoding="utf-8", errors="replace")
        clean_lines_preserved = strip_cpp_comments(raw)
        clean = strip_cpp_comments(join_line_continuations(raw))
        env = collect_constants(clean, global_env)
        arrays = dict(shared_arrays)
        arrays.update(extract_byte_arrays(clean, env))

        # Collect 0x8Bxxxx literals (with line numbers from comment-stripped lines)
        for line_no, line in enumerate(clean_lines_preserved.splitlines(), 1):
            for lm in iat_lit_re.finditer(line):
                res.iat_literals.append((int(lm.group(1), 16), rel, line_no))

        if p.suffix != ".cpp":
            continue

        # 1) InstallInlineHook(...) calls
        for m in re.finditer(r"\bInstallInlineHook\s*\(", clean):
            paren_end = find_matching_paren(clean, m.end() - 1)
            if paren_end < 0:
                continue
            args = split_top_level_commas(clean[m.end():paren_end])
            if len(args) < 6:
                continue
            rva = resolve_rva_arg(args[0], env, clean[:m.start()])
            exp_name = args[3].strip()
            if rva is not None and exp_name in arrays:
                arr = arrays[exp_name]
                slen = resolve_expect_len(args[4], arr, env)
                lbl = args[5].strip().strip('"')
                res.write_intervals.append(WriteInterval(rva, slen, "inline_hook", lbl, rel))
                res.byte_checks.append(ByteCheck(rva, arr[:slen], lbl, rel))

        # 2) TwoStatesInlineHookSpec tables (kSpecs[])
        for m in re.finditer(r"\bTwoStatesInlineHookSpec\s+[A-Za-z0-9_]+\s*\[\s*\d*\s*\]\s*=\s*\{", clean):
            close_idx = find_matching_brace(clean, m.end() - 1)
            if close_idx < 0:
                continue
            for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
                fields = split_top_level_commas(entry)
                if len(fields) < 6:
                    continue
                rva = eval_int_expr(fields[0], env)
                exp_name = fields[3].strip()
                if rva is not None and exp_name in arrays:
                    arr = arrays[exp_name]
                    slen = resolve_expect_len(fields[4], arr, env)
                    lbl = fields[5].strip().strip('"')
                    res.write_intervals.append(WriteInterval(rva, slen, "inline_hook", lbl, rel))
                    res.byte_checks.append(ByteCheck(rva, arr[:slen], lbl, rel))

        # 3) PatchJmp14WithNops(...) calls
        for m in re.finditer(r"\bPatchJmp14WithNops\s*\(", clean):
            paren_end = find_matching_paren(clean, m.end() - 1)
            if paren_end < 0:
                continue
            args = split_top_level_commas(clean[m.end():paren_end])
            if len(args) < 5:
                continue
            rva = resolve_rva_arg(args[0], env, clean[:m.start()])
            exp_name = args[2].strip()
            if rva is not None and exp_name in arrays:
                arr = arrays[exp_name]
                slen = resolve_expect_len(args[1], arr, env)
                lbl = args[4].strip().strip('"')
                res.write_intervals.append(WriteInterval(rva, slen, "patch", lbl, rel))
                res.byte_checks.append(ByteCheck(rva, arr[:slen], lbl, rel))

        # 4) ValidateSpaceActionSite(siteRva, expect, ...)
        for m in re.finditer(r"\bValidateSpaceActionSite\s*\(", clean):
            paren_end = find_matching_paren(clean, m.end() - 1)
            if paren_end < 0:
                continue
            args = split_top_level_commas(clean[m.end():paren_end])
            if len(args) < 4:
                continue
            rva = eval_int_expr(args[0], env)
            exp_name = args[1].strip()
            if rva is not None and exp_name in arrays:
                arr = arrays[exp_name]
                lbl = args[3].strip().strip('"')
                res.write_intervals.append(WriteInterval(rva, len(arr), "patch", lbl, rel))
                res.byte_checks.append(ByteCheck(rva, arr, lbl, rel))

        # 5) SafeBytesMatch(g_exeBase + rva, k*Expect, sizeof(...)) — read-only .text check
        for m in re.finditer(r"\bSafeBytesMatch\s*\(", clean):
            paren_end = find_matching_paren(clean, m.end() - 1)
            if paren_end < 0:
                continue
            args = split_top_level_commas(clean[m.end():paren_end])
            if len(args) < 3:
                continue
            rva = resolve_rva_arg(args[0], env, clean[:m.start()])
            exp_name = args[1].strip()
            if rva is not None and exp_name in arrays:
                arr = arrays[exp_name]
                slen = resolve_expect_len(args[2], arr, env)
                res.byte_checks.append(ByteCheck(rva, arr[:slen], exp_name, rel))

        # 6) TsmRoutineSig tables
        for m in re.finditer(r"\bTsmRoutineSig\s+[A-Za-z0-9_]+\s*\[\s*\d*\s*\]\s*=\s*\{", clean):
            close_idx = find_matching_brace(clean, m.end() - 1)
            if close_idx < 0:
                continue
            for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
                fields = split_top_level_commas(entry)
                if len(fields) < 3:
                    continue
                rva = eval_int_expr(fields[0], env)
                btext = fields[1].strip()
                if btext.startswith("{") and btext.endswith("}"):
                    bs = expand_nop_macros(btext[1:-1], env)
                else:
                    bs = None
                sz = eval_int_expr(fields[2], env)
                if rva is not None and bs is not None:
                    use_sz = sz if (sz is not None and sz > 0) else 16
                    res.routine_sigs.append(SigEntry(rva, bs[:use_sz], f"sig@0x{rva:X}", rel))

        # 7) File-specific tables
        if p.name == "sim_patches.cpp":
            _extract_sim_patches(clean, env, arrays, rel, res)
        elif p.name == "session_patches.cpp":
            _extract_session_patches(clean, env, rel, res)
        elif p.name == "determinism.cpp":
            _extract_determinism(clean, env, rel, res)
        elif p.name == "game_calls.cpp":
            _extract_game_calls(clean, env, rel, res)
        elif p.name == "imgui_hook.cpp":
            _extract_testbed_patches(clean, env, rel, res)
        elif p.name == "brush_tools.cpp":
            _extract_brush_tools(clean, env, rel, res)
        elif p.name == "hover_tools.cpp":
            _extract_routine_entries(clean, env, rel, res, "hoverRoutine")
        elif p.name == "two_states_research.cpp":
            _extract_two_states_research(clean, env, arrays, rel, res)
        elif p.name == "road_sync.cpp":
            _extract_road_sync(clean, env, rel, res)
        elif p.name == "tesmioloader.cpp":
            _extract_tesmioloader(clean, env, arrays, rel, res)
        elif p.name == "native_menu.cpp":
            _extract_native_menu(clean, env, rel, res)
        elif p.name == "two_states_persistence.cpp":
            _extract_two_states_persistence(clean, env, rel, res)

    return res


def _extract_sim_patches(clean: str, env: Dict[str, int], arrays: Dict[str, bytes],
                         rel: str, res: ExtractedContracts) -> None:
    # kPatches[] (DetPatch)
    m = re.search(r"\bDetPatch\s+kPatches\s*\[\s*\]\s*=\s*\{", clean)
    if m:
        close_idx = find_matching_brace(clean, m.end() - 1)
        for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
            fields = split_top_level_commas(entry)
            if len(fields) < 5:
                continue
            rva = eval_int_expr(fields[0], env)
            sz = eval_int_expr(fields[1], env)
            orig_s = fields[2].strip()
            lbl = fields[4].strip().strip('"')
            if rva is not None and sz is not None and orig_s.startswith("{"):
                bs = expand_nop_macros(orig_s[1:-1], env)
                if bs is not None:
                    res.write_intervals.append(WriteInterval(rva, sz, "patch", lbl, rel))
                    res.byte_checks.append(ByteCheck(rva, bs[:sz], lbl, rel))

    # kStepCalls[] + kStepTargets[]
    mc = re.search(r"\bkStepCalls\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    mt = re.search(r"\bkStepTargets\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    if mc and mt:
        cc = find_matching_brace(clean, mc.end() - 1)
        ct = find_matching_brace(clean, mt.end() - 1)
        calls = [eval_int_expr(x, env) for x in split_top_level_commas(clean[mc.end():cc])]
        targs = [eval_int_expr(x, env) for x in split_top_level_commas(clean[mt.end():ct])]
        for s_rva, t_rva in zip(calls, targs):
            if s_rva is not None and t_rva is not None:
                res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", f"stepCall@0x{s_rva:X}", rel))
                res.call_checks.append(CallTargetCheck(s_rva, t_rva, f"stepCall@0x{s_rva:X}", rel))

    # kToolCalls[][2]
    mtc = re.search(r"\bkToolCalls\s*\[\s*\]\s*\[\s*2\s*\]\s*=\s*\{", clean)
    if mtc:
        close_idx = find_matching_brace(clean, mtc.end() - 1)
        for entry in split_top_level_braced_entries(clean[mtc.end():close_idx]):
            fields = split_top_level_commas(entry)
            if len(fields) == 2:
                s_rva = eval_int_expr(fields[0], env)
                t_rva = eval_int_expr(fields[1], env)
                if s_rva is not None and t_rva is not None:
                    res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", f"toolCall@0x{s_rva:X}", rel))
                    res.call_checks.append(CallTargetCheck(s_rva, t_rva, f"toolCall@0x{s_rva:X}", rel))

    # kPickRaySites[] + kPickRayOriginal[]
    mp_sites = re.search(r"\bkPickRaySites\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    mp_orig = re.search(r"\bkPickRayOriginal\s*\[\s*[^\]]*\]\s*\[\s*\d+\s*\]\s*=\s*\{", clean)
    if mp_sites and mp_orig:
        cs = find_matching_brace(clean, mp_sites.end() - 1)
        co = find_matching_brace(clean, mp_orig.end() - 1)
        sites = [eval_int_expr(x, env) for x in split_top_level_commas(clean[mp_sites.end():cs])]
        origs = split_top_level_braced_entries(clean[mp_orig.end():co])
        for s_rva, o_txt in zip(sites, origs):
            bs = expand_nop_macros(o_txt, env)
            if s_rva is not None and bs is not None:
                res.write_intervals.append(WriteInterval(s_rva, len(bs), "iat_call_relay", f"pickRay@0x{s_rva:X}", rel))
                res.pick_ray_checks.append(PickRayCheck(s_rva, bs, rel))

    # 8 single relay sites
    relay_pairs = [
        ("GETNODE_SITE", "kGetNodeOriginal", "iat_call_relay"),
        ("SAPLING_RENDER_CALL", "kSaplingRenderOriginal", "call_relay"),
        ("SAPLING_LATE_CALL", "kSaplingLateOriginal", "call_relay"),
        ("FRAME_LOCK_SITE", "kFrameLockOriginal", "iat_call_relay"),
        ("UI_ROUTINE_CALL", "kUiCallOriginal", "call_relay"),
        ("PREPARE_CALL", "kPrepareCallOriginal", "call_relay"),
        ("PLACE_PRESS_SITE", "kPlacePressOriginal", "iat_call_relay"),
        ("BUY_PRESS_SITE", "kBuyPressOriginal", "iat_call_relay"),
    ]
    for site_sym, arr_sym, kind in relay_pairs:
        if site_sym in env and arr_sym in arrays:
            rva = env[site_sym]
            bs = arrays[arr_sym]
            res.write_intervals.append(WriteInterval(rva, len(bs), kind, site_sym, rel))
            res.byte_checks.append(ByteCheck(rva, bs, site_sym, rel))


def _extract_session_patches(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    m = re.search(r"\bSessionPatch\s+kPatches\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        return
    close_idx = find_matching_brace(clean, m.end() - 1)
    for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
        fields = split_top_level_commas(entry)
        if len(fields) < 5:
            continue
        rva = eval_int_expr(fields[0], env)
        sz = eval_int_expr(fields[1], env)
        orig_s = fields[2].strip()
        lbl = fields[4].strip().strip('"')
        if rva is not None and sz is not None and orig_s.startswith("{"):
            bs = expand_nop_macros(orig_s[1:-1], env)
            if bs is not None:
                res.write_intervals.append(WriteInterval(rva, sz, "patch", lbl, rel))
                res.byte_checks.append(ByteCheck(rva, bs[:sz], lbl, rel))
                if "citizen init" in lbl.lower():
                    res.session_citizen_init_sites.add(rva)


def _extract_determinism(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    m = re.search(r"\bkPresentationSites\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        return
    close_idx = find_matching_brace(clean, m.end() - 1)
    for tok in split_top_level_commas(clean[m.end():close_idx]):
        v = eval_int_expr(tok, env)
        if v is not None:
            res.presentation_sites.append((v, rel))


def _extract_game_calls(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    m = re.search(r"\bGameCallSite\s+kSites\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        return
    close_idx = find_matching_brace(clean, m.end() - 1)
    for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
        fields = split_top_level_commas(entry)
        if len(fields) < 3:
            continue
        site_rva = eval_int_expr(fields[0], env)
        target_rva = eval_int_expr(fields[1], env)
        lbl = fields[2].strip().strip('"')
        if site_rva is not None and target_rva is not None:
            res.write_intervals.append(WriteInterval(site_rva, 5, "call_relay", lbl, rel))
            res.call_checks.append(CallTargetCheck(site_rva, target_rva, lbl, rel))


def _extract_testbed_patches(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    m = re.search(r"\bTestbedCodePatch\s+kTestbedPatches\s*\[\s*\]\s*=\s*\{", clean)
    if not m:
        return
    close_idx = find_matching_brace(clean, m.end() - 1)
    for entry in split_top_level_braced_entries(clean[m.end():close_idx]):
        fields = split_top_level_commas(entry)
        if len(fields) < 4:
            continue
        rva = eval_int_expr(fields[0], env)
        sz = eval_int_expr(fields[1], env)
        orig_s = fields[2].strip()
        if rva is not None and sz is not None and orig_s.startswith("{"):
            bs = expand_nop_macros(orig_s[1:-1], env)
            if bs is not None:
                res.write_intervals.append(WriteInterval(rva, sz, "patch", f"testbed@0x{rva:X}", rel))
                res.byte_checks.append(ByteCheck(rva, bs[:sz], f"testbed@0x{rva:X}", rel))


def _extract_routine_entries(clean: str, env: Dict[str, int], rel: str,
                             res: ExtractedContracts, label_prefix: str) -> None:
    mr = re.search(r"\bkRoutines\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    me = re.search(r"\bkEntries\s*\[\s*[^\]]*\]\s*\[\s*\d+\s*\]\s*=\s*\{", clean)
    if mr and me:
        cr = find_matching_brace(clean, mr.end() - 1)
        ce = find_matching_brace(clean, me.end() - 1)
        rvas = [eval_int_expr(x, env) for x in split_top_level_commas(clean[mr.end():cr])]
        entries = split_top_level_braced_entries(clean[me.end():ce])
        for rva, e_txt in zip(rvas, entries):
            bs = expand_nop_macros(e_txt, env)
            if rva is not None and bs is not None:
                res.byte_checks.append(ByteCheck(rva, bs, f"{label_prefix}@0x{rva:X}", rel))


def _extract_brush_tools(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    _extract_routine_entries(clean, env, rel, res, "brushRoutine")

    mi = re.search(r"\bInputSite\s+kInputSites\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    ms = re.search(r"\bkInputSlots\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    if mi and ms:
        ci = find_matching_brace(clean, mi.end() - 1)
        cs = find_matching_brace(clean, ms.end() - 1)
        slots = [eval_int_expr(x, env) for x in split_top_level_commas(clean[ms.end():cs])]
        for entry in split_top_level_braced_entries(clean[mi.end():ci]):
            fields = split_top_level_commas(entry)
            if len(fields) == 2:
                s_rva = eval_int_expr(fields[0], env)
                g_idx = eval_int_expr(fields[1], env)
                if s_rva is not None and g_idx is not None and 0 <= g_idx < len(slots) and slots[g_idx] is not None:
                    res.write_intervals.append(WriteInterval(s_rva, 6, "iat_call_relay", f"brushInput@0x{s_rva:X}", rel))
                    res.iat_call_checks.append(CallTargetCheck(s_rva, slots[g_idx], f"brushInput@0x{s_rva:X}", rel))

    ml = re.search(r"\bkLedgerCalls\s*\[\s*[^\]]*\]\s*=\s*\{", clean)
    if ml and "LEDGER_RVA" in env:
        cl = find_matching_brace(clean, ml.end() - 1)
        for tok in split_top_level_commas(clean[ml.end():cl]):
            s_rva = eval_int_expr(tok, env)
            if s_rva is not None:
                res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", f"brushLedger@0x{s_rva:X}", rel))
                res.call_checks.append(CallTargetCheck(s_rva, env["LEDGER_RVA"], f"brushLedger@0x{s_rva:X}", rel))

    md = re.search(r"\bkDemGroundSites\s*\[\s*2\s*\]\s*\[\s*2\s*\]\s*=\s*\{", clean)
    if md:
        cd = find_matching_brace(clean, md.end() - 1)
        for entry in split_top_level_braced_entries(clean[md.end():cd]):
            fields = split_top_level_commas(entry)
            if len(fields) == 2:
                s_rva = eval_int_expr(fields[0], env)
                t_rva = eval_int_expr(fields[1], env)
                if s_rva is not None and t_rva is not None:
                    res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", f"demGround@0x{s_rva:X}", rel))
                    res.call_checks.append(CallTargetCheck(s_rva, t_rva, f"demGround@0x{s_rva:X}", rel))


def _extract_road_sync(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    if "CANCEL_SEG_RVA" in env:
        cancel_rva = env["CANCEL_SEG_RVA"]
        for m in re.finditer(r"\bPatchCallToCancelSeg\s*\(\s*([^)]+)\)", clean):
            s_rva = eval_int_expr(m.group(1), env)
            if s_rva is not None:
                res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", f"cancelSeg@0x{s_rva:X}", rel))
                res.call_checks.append(CallTargetCheck(s_rva, cancel_rva, f"cancelSeg@0x{s_rva:X}", rel))
    # 0x3EEAA8 SECOND_ROAD empty-segments guard (2-byte je offset patch)
    if "0x3EEAA8" in clean:
        res.write_intervals.append(WriteInterval(0x3EEAA8, 2, "patch", "SECOND_ROAD je +0x0B", rel))
        res.byte_checks.append(ByteCheck(0x3EEAA8, bytes([0x74, 0x11]), "SECOND_ROAD je +0x11", rel))


def _extract_two_states_research(clean: str, env: Dict[str, int], arrays: Dict[str, bytes],
                                 rel: str, res: ExtractedContracts) -> None:
    if "kResearchRecomputeExpect" not in arrays:
        return
    if not re.search(r"\bSafeBytesMatch\s*\([^,]+,\s*kResearchRecomputeExpect\b", clean):
        return
    arr = arrays["kResearchRecomputeExpect"]
    for m in re.finditer(r"\bTwoStates_MatchesResearchRecomputePrologueForTest\s*\(", clean):
        paren_end = find_matching_paren(clean, m.end() - 1)
        if paren_end < 0:
            continue
        args = split_top_level_commas(clean[m.end():paren_end])
        if len(args) < 2:
            continue
        rva = resolve_rva_arg(args[0], env, clean[:m.start()])
        if rva is not None:
            slen = resolve_expect_len(args[1], arr, env)
            res.byte_checks.append(ByteCheck(rva, arr[:slen], "kResearchRecomputeExpect", rel))


def _extract_tesmioloader(clean: str, env: Dict[str, int], arrays: Dict[str, bytes],
                          rel: str, res: ExtractedContracts) -> None:
    if "P_MENU_VERSION_SITE" in env and "kMenuLeaOrig" in arrays:
        rva = env["P_MENU_VERSION_SITE"]
        lea_len = env.get("MENU_LEA_LEN", 7)
        bs = arrays["kMenuLeaOrig"]
        res.write_intervals.append(WriteInterval(rva, lea_len, "patch", "P_MENU_VERSION_SITE", rel))
        res.byte_checks.append(ByteCheck(rva, bs, "P_MENU_VERSION_SITE", rel))


def _extract_native_menu(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    if "MENU_CALL_SITE_RVA" in env and "MENU_BUILD_FN_RVA" in env:
        s_rva = env["MENU_CALL_SITE_RVA"]
        t_rva = env["MENU_BUILD_FN_RVA"]
        res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", "MENU_CALL_SITE_RVA", rel))
        res.call_checks.append(CallTargetCheck(s_rva, t_rva, "MENU_CALL_SITE_RVA", rel))
    if "SYSTAB_CALL_SITE_RVA" in env and "SYSTAB_BUILD_FN_RVA" in env:
        s_rva = env["SYSTAB_CALL_SITE_RVA"]
        t_rva = env["SYSTAB_BUILD_FN_RVA"]
        res.write_intervals.append(WriteInterval(s_rva, 5, "call_relay", "SYSTAB_CALL_SITE_RVA", rel))
        res.call_checks.append(CallTargetCheck(s_rva, t_rva, "SYSTAB_CALL_SITE_RVA", rel))
    if "SYSTAB_ROOT_TITLE_RVA" in env and "IMPORT_DRAW_TEXT_RVA" in env:
        s_rva = env["SYSTAB_ROOT_TITLE_RVA"]
        iat_rva = env["IMPORT_DRAW_TEXT_RVA"]
        res.write_intervals.append(WriteInterval(s_rva, 6, "iat_call_relay", "SYSTAB_ROOT_TITLE_RVA", rel))
        res.iat_call_checks.append(CallTargetCheck(s_rva, iat_rva, "SYSTAB_ROOT_TITLE_RVA", rel))


def _extract_two_states_persistence(clean: str, env: Dict[str, int], rel: str, res: ExtractedContracts) -> None:
    if "RVA_MASTER_SAVE_AUTOSAVE_JE" in env:
        rva = env["RVA_MASTER_SAVE_AUTOSAVE_JE"]
        res.write_intervals.append(WriteInterval(rva, 2, "patch", "RVA_MASTER_SAVE_AUTOSAVE_JE", rel))
        res.byte_checks.append(ByteCheck(rva, bytes([0x74, 0x1F]), "RVA_MASTER_SAVE_AUTOSAVE_JE", rel))


# ---------------------------------------------------------------------------
# Verification Checks 1 .. 6
# ---------------------------------------------------------------------------

def check_1_pe_version(pe: PeFile, contracts: ExtractedContracts) -> Tuple[bool, str]:
    stamps = {s for _, s in contracts.supported_stamps}
    if pe.time_date_stamp not in stamps:
        exp = ", ".join(f"{v}(0x{s:08X})" for v, s in contracts.supported_stamps)
        return False, f"PE TimeDateStamp 0x{pe.time_date_stamp:08X} not in kSupportedVersions [{exp}]"
    return True, f"PE TimeDateStamp 0x{pe.time_date_stamp:08X} matches kSupportedVersions"


def check_2_collisions(contracts: ExtractedContracts) -> Tuple[bool, str]:
    errors: List[str] = []

    # A. Duplicate inline hook RVAs
    seen_hooks: Dict[int, WriteInterval] = {}
    inline_hooks = [w for w in contracts.write_intervals if w.kind == "inline_hook"]
    for w in inline_hooks:
        if w.rva in seen_hooks:
            prev = seen_hooks[w.rva]
            errors.append(f"Duplicate inline hook at 0x{w.rva:X}: {prev.file}({prev.label}) vs {w.file}({w.label})")
        else:
            seen_hooks[w.rva] = w

    # B. Pairwise write-interval overlaps & valid sizes
    valid_w = []
    for w in contracts.write_intervals:
        if w.size <= 0:
            errors.append(f"Invalid write interval size ({w.size}) at 0x{w.rva:X}: {w.file}({w.label})")
        else:
            valid_w.append(w)

    sorted_w = sorted(valid_w, key=lambda x: (x.rva, x.end))
    for i in range(len(sorted_w)):
        a = sorted_w[i]
        for j in range(i + 1, len(sorted_w)):
            b = sorted_w[j]
            if b.rva >= a.end:
                break
            errors.append(
                f"Write overlap [0x{a.rva:X}..0x{a.end:X}) {a.file}({a.label}) "
                f"vs [0x{b.rva:X}..0x{b.end:X}) {b.file}({b.label})"
            )

    # C. Write intervals vs TsmRoutineSig intervals
    for w in valid_w:
        for s in contracts.routine_sigs:
            if w.rva < s.end and s.rva < w.end:
                errors.append(
                    f"Write [0x{w.rva:X}..0x{w.end:X}) {w.file}({w.label}) "
                    f"overlaps TsmRoutineSig [0x{s.rva:X}..0x{s.end:X}) {s.file}"
                )

    # D. Write intervals vs standalone read-only byte checks
    write_keys = {(w.rva, w.file) for w in valid_w}
    ro_checks = [bc for bc in contracts.byte_checks if (bc.rva, bc.file) not in write_keys and len(bc.expected) > 0]
    for w in valid_w:
        for bc in ro_checks:
            bc_end = bc.rva + len(bc.expected)
            if w.rva < bc_end and bc.rva < w.end:
                errors.append(
                    f"Write [0x{w.rva:X}..0x{w.end:X}) {w.file}({w.label}) "
                    f"overlaps read-only check [0x{bc.rva:X}..0x{bc_end:X}) {bc.file}({bc.label})"
                )

    if errors:
        return False, "; ".join(errors[:5]) + (f" (+{len(errors) - 5} more)" if len(errors) > 5 else "")
    return True, (
        f"0 collisions across {len(contracts.write_intervals)} write sites "
        f"({len(inline_hooks)} inline hooks) and {len(contracts.routine_sigs)} TsmRoutineSig entries"
    )


def check_3_text_bytes(pe: PeFile, contracts: ExtractedContracts) -> Tuple[bool, str]:
    errors: List[str] = []
    checked = 0

    for bc in contracts.byte_checks:
        actual = pe.read_rva(bc.rva, len(bc.expected))
        checked += 1
        if actual != bc.expected:
            got = actual.hex() if actual else "<OOB>"
            errors.append(f"{bc.file} {bc.label} @0x{bc.rva:X}: expected {bc.expected.hex()}, got {got}")

    for sig in contracts.routine_sigs:
        actual = pe.read_rva(sig.rva, len(sig.expected))
        checked += 1
        if actual != sig.expected:
            got = actual.hex() if actual else "<OOB>"
            errors.append(f"{sig.file} TsmRoutineSig @0x{sig.rva:X}: expected {sig.expected.hex()}, got {got}")

    for cc in contracts.call_checks:
        insn = pe.read_rva(cc.site_rva, 5)
        checked += 1
        if not insn or insn[0] != 0xE8:
            got = insn.hex() if insn else "<OOB>"
            errors.append(f"{cc.file} {cc.label} @0x{cc.site_rva:X}: not E8 call ({got})")
            continue
        rel32 = struct.unpack_from("<i", insn, 1)[0]
        target = cc.site_rva + 5 + rel32
        if target != cc.target_rva:
            errors.append(f"{cc.file} {cc.label} @0x{cc.site_rva:X}: targets 0x{target:X}, expected 0x{cc.target_rva:X}")

    for pr in contracts.pick_ray_checks:
        actual = pe.read_rva(pr.site_rva, len(pr.expected))
        checked += 1
        if actual != pr.expected:
            got = actual.hex() if actual else "<OOB>"
            errors.append(f"{pr.file} pickRay @0x{pr.site_rva:X}: expected {pr.expected.hex()}, got {got}")

    for ic in contracts.iat_call_checks:
        checked += 1
        _verify_ff15_iat(pe, ic.site_rva, ic.target_rva, f"{ic.file} {ic.label}", errors)

    for site_rva, rel in contracts.presentation_sites:
        call_rva = site_rva - 6
        insn = pe.read_rva(call_rva, 6)
        checked += 1
        if not insn or insn[:2] != b"\xFF\x15":
            got = insn.hex() if insn else "<OOB>"
            errors.append(f"{rel} presentationSite @0x{site_rva:X}: preceding bytes not FF 15 ({got})")
            continue
        disp = struct.unpack_from("<i", insn, 2)[0]
        iat_slot = site_rva + disp
        sym = pe.iat_map.get(iat_slot, "")
        if "rand" not in sym.lower():
            errors.append(f"{rel} presentationSite @0x{site_rva:X}: IAT 0x{iat_slot:X} is '{sym}' (not rand/C3DRandom)")

    if errors:
        return False, "; ".join(errors[:5]) + (f" (+{len(errors) - 5} more)" if len(errors) > 5 else "")
    return True, f"All {checked} .text byte/call/signature/presentation sites matched (0 mismatches)"


KNOWN_NON_IAT_8B_RVAS: Set[int] = {
    0x8B708C,  # RVA of .rdata string/table literal in engine_rva.h
    0x8BB3C8,  # SND_CLICK_NAME_RVA ("click" / config key string in .rdata)
}


def check_4_iat_resolution(pe: PeFile, contracts: ExtractedContracts) -> Tuple[bool, str]:
    iat_begin = pe.iat_rva
    iat_end = pe.iat_rva + pe.iat_size
    errors: List[str] = []

    non_iat_rvas = sorted({(rva, f, ln) for rva, f, ln in contracts.iat_literals
                           if not (iat_begin <= rva < iat_end) and rva not in KNOWN_NON_IAT_8B_RVAS})
    for rva, f, ln in non_iat_rvas:
        errors.append(f"Unexpected 0x8Bxxxx literal outside IAT [0x{iat_begin:X}..0x{iat_end:X}): 0x{rva:X} at {f}:{ln}")

    unique_rvas = sorted({rva for rva, _, _ in contracts.iat_literals if iat_begin <= rva < iat_end})
    for rva in unique_rvas:
        if rva not in pe.iat_map:
            errors.append(f"Unresolved IAT RVA 0x{rva:X}")

    expected_substrings = {
        0x8B1010: "C3DVECTOR3",
        0x8B1118: "C3DVec3Normalize",
        0x8B1518: "CreateManagedTexture",
        0x8B1528: "CalculateFinal",
        0x8B1538: "CalculateView",
        0x8B15D0: "GetNode",
        0x8B1858: "GetPosition",
        0x8B18F0: "Collision@C3D_PANEL2D",
        0x8B18F8: "DrawFrom9Patch",
        0x8B1900: "Draw@C3D_PANEL2D",
        0x8B1908: "GetMouseSolid",
        0x8B1910: "PrintAllTexts",
        0x8B1920: "PrintLeftUnicode",
        0x8B1950: "GetMouseLeftPressAOff",
        0x8B1958: "GetMouseLeftPress",
        0x8B1998: "EndDraw@C3D_PANEL2D",
        0x8B19B8: "BeginDraw@C3D_PANEL2D",
        0x8B1AD0: "C3DRandom_Int",
        0x8B1AD8: "C3DRandom_Float",
        0x8B1C20: "PlaySound3D",
        0x8B1CA8: "GetClipHeightOffsetRealWithZoom",
        0x8B1D58: "GetValueExist",
        0x8B1D60: "GetValueString",
        0x8B2008: "GetFragment",
        0x8B2058: "GetZDistance",
        0x8B22B0: "EditorAddCursorTreesSoviet",
        0x8B22E8: "PlaySound2D",
        0x8B2598: "ChangeValuesTo",
        0x8B25A0: "RestoreOriginalValues",
        0x8B25A8: "SaveOriginalValues",
        0x8B2830: "EnterCriticalSection",
        0x8B2858: "CreateEventW",
        0x8B30E8: "rand",
    }
    for rva in unique_rvas:
        if rva not in expected_substrings:
            errors.append(f"Undeclared IAT slot 0x{rva:X} (sym='{pe.iat_map.get(rva, '')}') in expected_substrings")
    for rva, sub in expected_substrings.items():
        sym = pe.iat_map.get(rva, "")
        if sub not in sym:
            errors.append(f"IAT 0x{rva:X} expected '{sub}', got '{sym}'")

    if errors:
        return False, "; ".join(errors[:5]) + (f" (+{len(errors) - 5} more)" if len(errors) > 5 else "")
    return True, f"All {len(unique_rvas)} unique IAT 0x8Bxxxx slots in [0x{iat_begin:X}..0x{iat_end:X}) verified ({len(expected_substrings)} exact symbols)"


def _verify_ff15_iat(pe: PeFile, site_rva: int, expected_iat: int, desc: str, errs: List[str]) -> None:
    b = pe.read_rva(site_rva, 6)
    if not b or b[:2] != b"\xFF\x15":
        errs.append(f"{desc} @0x{site_rva:X}: not FF 15")
        return
    disp = struct.unpack_from("<i", b, 2)[0]
    target = site_rva + 6 + disp
    if target != expected_iat:
        errs.append(f"{desc} @0x{site_rva:X}: IAT 0x{target:X} != 0x{expected_iat:X}")


def _verify_rip_target(pe: PeFile, insn_rva: int, insn_len: int, prefix: bytes,
                       expected_rva: int, desc: str, errs: List[str]) -> None:
    b = pe.read_rva(insn_rva, insn_len)
    if not b or not b.startswith(prefix):
        errs.append(f"{desc} @0x{insn_rva:X}: prefix mismatch")
        return
    disp = struct.unpack_from("<i", b, len(prefix))[0]
    target = insn_rva + insn_len + disp
    if target != expected_rva:
        errs.append(f"{desc} @0x{insn_rva:X}: RIP target 0x{target:X} != 0x{expected_rva:X}")


def check_5_register_contracts(pe: PeFile) -> Tuple[bool, str]:
    errs: List[str] = []

    # 1. BUY_PRESS_SITE (0x852B02)..0x85302E + 0x422E60
    _verify_ff15_iat(pe, 0x852B02, 0x8B1958, "BUY_PRESS_SITE", errs)
    exact_bytes = [
        (0x852B10, bytes.fromhex("488B4C246880792000"), "BUY window mov rcx,[rsp+0x68]; cmp byte ptr [rcx+0x20],0"),
        (0x852B1F, bytes.fromhex("84DB"),               "BUY test bl,bl"),
        (0x852B39, bytes.fromhex("488B7C2470"),         "BUY type Capture mov rdi,[rsp+0x70]"),
        (0x852C6A, bytes.fromhex("440F2F8774860000"),   "BUY avail comiss xmm8,[rdi+0x8674]"),
        (0x852D07, bytes.fromhex("66450F2F96A8050000"), "BUY dollars comisd xmm10,[r14+0x5A8]"),
        (0x852D19, bytes.fromhex("66450F2F8EB0050000"), "BUY rubles comisd xmm9,[r14+0x5B0]"),
        (0x852DF1, bytes.fromhex("4C6344247C"),         "BUY container movsxd r8,[rsp+0x7C]"),
        (0x852EE5, bytes.fromhex("488B5C2470"),         "BUY type mov rbx,[rsp+0x70]"),
        (0x852F3F, bytes.fromhex("488D8B087B0000E855EA7DFF"), "BUY plane props lea rcx,[rbx+0x7B08]; call 0x0319A0"),
        (0x852F76, bytes.fromhex("4038B3D0860000"),     "BUY rail electric cmp byte ptr [rbx+0x86D0],sil"),
        (0x852FB8, bytes.fromhex("488B8310970000482B8308970000"), "BUY tags 0x9708/0x9710"),
        (0x852FD2, bytes.fromhex("488D15E32C0F00488D0D40BF1C00E86B49A2FF"), "BUY tram tag lea rdx,[tram]; lea rcx,[WORLD_RVA]; call 0x277950"),
        (0x85302E, bytes.fromhex("E82DFEBCFF"),         "BUY call 0x422E60"),
        (0x422E6F, bytes.fromhex("80793C00"),           "0x422E60 cmp [rcx+0x3C],0"),
        (0x422E7F, bytes.fromhex("4881C1B0FD0000"),     "0x422E60 add rcx,0xFDB0"),
        (0x422E98, bytes.fromhex("488D8BB0FD0000"),     "0x422E60 lea rcx,[rbx+0xFDB0]"),
        (0x422EB1, bytes.fromhex("488B8BA8FD0000"),     "0x422E60 mov rcx,[rbx+0xFDA8]"),
        (0x422EBB, bytes.fromhex("48C744242000000000"), "0x422E60 mov qword ptr [rsp+0x20],0"),
        (0x86F083, bytes.fromhex("4C8B4758"),           "CIT cur building mov r8,[rdi+0x58]"),
        (0x86F0A6, bytes.fromhex("4C8B8F90060000"),     "CIT dest building mov r9,[rdi+0x690]"),
        (0x86F0F5, bytes.fromhex("4C3B4F20"),           "CIT dest vs home cmp r9,[rdi+0x20]"),
        (0x27795A, bytes.fromhex("4C8B81A8280100488BDA4C2B81A0280100"), "FindTagId world+0x128A0/0x128A8"),
        (0x2779A0, bytes.fromhex("4D6BC044"),           "FindTagId stride 0x44"),
    ]
    for rva, exp, desc in exact_bytes:
        got = pe.read_rva(rva, len(exp))
        if got != exp:
            errs.append(f"{desc} @0x{rva:X}: got {got.hex() if got else '<OOB>'}, expected {exp.hex()}")
    _verify_ff15_iat(pe, 0x422EC4, 0x8B22E8, "0x422E60 PlaySound2D", errs)
    if pe.read_cstr_rva(0x945CBC) != "tram":
        errs.append(f"0x945CBC sound tag literal != 'tram' (got {pe.read_cstr_rva(0x945CBC)!r})")
    if pe.read_cstr_rva(0x945CF0) != "purchase_vehicle":
        errs.append(f"0x945CF0 sound key literal != 'purchase_vehicle' (got {pe.read_cstr_rva(0x945CF0)!r})")

    # 2. DEPOT_MARK_RETURN (0x2CF213) + prolog 0x2CE7F0 (rbp - rsp == 0x100)
    prolog = pe.read_rva(0x2CE7F0, 26)
    if not prolog or prolog[:12] != bytes.fromhex("488BC4554154415541564157") or prolog[12:15] != b"\x48\x8D\xA8" or prolog[19:22] != b"\x48\x81\xEC":
        errs.append("0x2CE7F0 prolog frame mismatch")
    else:
        rbp_disp = struct.unpack_from("<i", prolog, 15)[0]
        sub_imm = struct.unpack_from("<I", prolog, 22)[0]
        # 5 pushes (0x28) + sub rsp, sub_imm => rsp = rax - 0x28 - sub_imm
        if rbp_disp - (-0x28 - sub_imm) != 0x100:
            errs.append(f"0x2CE7F0 rbp-rsp frame delta != 0x100 (rbp={rbp_disp}, sub=0x{sub_imm:X})")
    call_2cf20e = pe.read_rva(0x2CF20E, 5)
    if not call_2cf20e or call_2cf20e[0] != 0xE8 or (0x2CF20E + 5 + struct.unpack_from("<i", call_2cf20e, 1)[0]) != 0x42FC30:
        errs.append("0x2CF20E does not call 0x42FC30")
    if pe.read_rva(0x2CF216, 4) != bytes.fromhex("488B5DA8"):
        errs.append("0x2CF216 is not mov rbx,[rbp-0x58]")

    # 3. CountPopulation (0x339943..0x339BE3)
    pop_checks = [
        (0x339943, bytes.fromhex("4D2B9E60270100"), "CountPopulation CITIZENS_OFFSET 0x12760"),
        (0x339981, bytes.fromhex("448B921C070000"), "CountPopulation +0x71C"),
        (0x339991, bytes.fromhex("399A48070000"),   "CountPopulation +0x748"),
        (0x339999, bytes.fromhex("4C8B82C8000000"), "CountPopulation +0xC8"),
        (0x3399AF, bytes.fromhex("488B8818030000"), "CountPopulation +0x318"),
        (0x3399BB, bytes.fromhex("3899F80A0000"),   "CountPopulation +0xAF8"),
        (0x3399C7, bytes.fromhex("385A2D"),         "CountPopulation +0x2D"),
        (0x339BDC, bytes.fromhex("41FF86C0C70000"), "CountPopulation POPULATION_OFFSET 0xC7C0"),
    ]
    for rva, exp, desc in pop_checks:
        if pe.read_rva(rva, len(exp)) != exp:
            errs.append(f"{desc} @0x{rva:X} mismatch")

    # 4. FillingrStart (0x274416..0x274454)
    _verify_rip_target(pe, 0x274416, 7, bytes.fromhex("488B1D"), 0x9DDC80, "FillingrStart FILLING_OBJ_RVA", errs)
    b_lea = pe.read_rva(0x27443D, 7)
    if not b_lea or b_lea[:3] != bytes.fromhex("4C8D0D"):
        errs.append("FillingrStart @0x27443D not lea r9,[rip+disp]")
    else:
        str_rva = 0x27443D + 7 + struct.unpack_from("<i", b_lea, 3)[0]
        if pe.read_wstr_rva(str_rva) != "FillingrStart":
            errs.append(f"FillingrStart @0x27443D string mismatch at 0x{str_rva:X}")
    _verify_ff15_iat(pe, 0x27444A, 0x8B2858, "FillingrStart CreateEventW", errs)
    if pe.read_rva(0x274450, 4) != bytes.fromhex("48894360"):
        errs.append("FillingrStart @0x274450 is not mov [rbx+0x60],rax")

    if errs:
        return False, "; ".join(errs[:5]) + (f" (+{len(errs) - 5} more)" if len(errs) > 5 else "")
    return True, "All 4 register/stack contracts (BUY_PRESS, DEPOT_MARK, CountPopulation, FillingrStart) verified"


def check_6_citizen_init_coverage(pe: PeFile, contracts: ExtractedContracts) -> Tuple[bool, str]:
    text_sec = pe.section_by_name(".text")
    if not text_sec:
        return False, "Missing .text section"
    target_rva = 0x865C00
    expected_patched = {0x31BAA7, 0x4CCF16, 0x7A1AEE, 0x7F9CE3, 0x865AD2}
    sim_allowlist = {0x8766A6, 0x8766B2}

    missing_in_src = expected_patched - contracts.session_citizen_init_sites
    if missing_in_src:
        return False, f"session_patches.cpp missing citizen init sites: {[hex(x) for x in sorted(missing_in_src)]}"

    raw = pe.data[text_sec.raw_offset:text_sec.raw_offset + min(text_sec.virtual_size, text_sec.raw_size)]
    va = text_sec.virtual_address
    callers: List[int] = []
    for i in range(len(raw) - 4):
        op = raw[i]
        if op == 0xE8 or op == 0xE9:
            site_rva = va + i
            rel32 = struct.unpack_from("<i", raw, i + 1)[0]
            if site_rva + 5 + rel32 == target_rva:
                callers.append(site_rva)

    allowed = contracts.session_citizen_init_sites | sim_allowlist
    unexpected = [c for c in callers if c not in allowed]
    if not callers:
        return False, f"Found 0 callers of 0x{target_rva:X} in .text (unexpected)"
    if unexpected:
        return False, f"Uncovered callers of 0x{target_rva:X}: {[hex(x) for x in unexpected]}"

    return True, (
        f"All {len(callers)} callers of CITIZEN_INIT_RVA (0x{target_rva:X}) accounted for "
        f"({len(contracts.session_citizen_init_sites)} patched + {len(sim_allowlist)} sim)"
    )


# ---------------------------------------------------------------------------
# Entry Point
# ---------------------------------------------------------------------------

def find_soviet64_exe(explicit: Optional[str]) -> Optional[Path]:
    if explicit:
        p = Path(explicit)
        return p if p.is_file() else None
    env_p = os.environ.get("SOVIET64_EXE", "").strip()
    if env_p:
        p = Path(env_p)
        return p if p.is_file() else None
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent
    candidates = [
        Path(r"C:\Program Files (x86)\Steam\steamapps\common\SovietRepublic\SOVIET64.exe"),
        repo_root / "local" / "test-host" / "SOVIET64.exe",
        repo_root / "local" / "test-client" / "SOVIET64.exe",
    ]
    for c in candidates:
        if c.is_file():
            return c
    return None


def main(argv: Optional[List[str]] = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    parser = argparse.ArgumentParser(description="Verify RepublicMP binary, IAT, hook, and register contracts.")
    parser.add_argument("--exe", help="Path to SOVIET64.exe (or set SOVIET64_EXE env var)")
    parser.add_argument("--require-exe", action="store_true", help="Fail if SOVIET64.exe is not found")
    args = parser.parse_args(argv)

    tesmio_dir = Path(__file__).resolve().parent
    contracts = extract_all_contracts(tesmio_dir)

    all_ok = True
    ok2, msg2 = check_2_collisions(contracts)
    print(f"[{'PASS' if ok2 else 'FAIL'}] Check 2 (Cross-Module Collisions): {msg2}")
    if not ok2:
        all_ok = False

    explicit_exe = (args.exe or os.environ.get("SOVIET64_EXE", "").strip())
    exe_path = find_soviet64_exe(args.exe)
    if exe_path is None:
        if explicit_exe:
            print(f"[FAIL] Explicit SOVIET64.exe path not found: {explicit_exe}")
            return 1
        if args.require_exe:
            print("[FAIL] SOVIET64.exe not found (--require-exe set).")
            return 1
        print("[SKIP] SOVIET64.exe not found; skipped PE checks 1, 3, 4, 5, 6 (Check 2 still enforced).")
        return 0 if all_ok else 1

    print(f"[INFO] Verifying against PE: {exe_path}")
    pe = PeFile(exe_path)

    checks = [
        ("Check 1 (PE Version)", lambda: check_1_pe_version(pe, contracts)),
        ("Check 3 (.text Byte Contracts)", lambda: check_3_text_bytes(pe, contracts)),
        ("Check 4 (IAT 0x8Bxxxx Resolution)", lambda: check_4_iat_resolution(pe, contracts)),
        ("Check 5 (Register & Stack Contracts)", lambda: check_5_register_contracts(pe)),
        ("Check 6 (0x865C00 Call Coverage)", lambda: check_6_citizen_init_coverage(pe, contracts)),
    ]
    for name, fn in checks:
        ok, msg = fn()
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {msg}")
        if not ok:
            all_ok = False

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
