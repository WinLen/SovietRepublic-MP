---
trigger: always_on
description: Enforces Uncle Bob's ub-mode Clean Code discipline (github.com/unclebob/ub-mode) across RepublicMP.
---

# ub-mode Clean Code Discipline (RepublicMP)

Every code change in this repository MUST follow **Uncle Bob's `ub-mode`** (`https://github.com/unclebob/ub-mode`):
1. **First Make it Work**:
   - **Laziness Protocol**: One decision lives in one place (`core/engine_rva.h`, `core/safe_mem.h`, `lockstep/world_refs.h`). Ship the smallest edit that solves the problem.
   - **Foundational Thinking**: Get the data shape right before writing the logic. Avoid unnecessary shared mutable state; if shared state is required, guard it via `TsmScopedCsLock`.
   - **TDD**: Write/extend behavioral tests in `TesmioLoader/RepublicMP/tests/test_main.cpp` and verify via `run_checks.bat`.
   - **Subtract Before You Add**: Take complexity and duplication out before adding new features.
   - **Boundary Discipline & Type System Discipline**: Validate raw engine memory and network packets at the boundary (`SafeReadPod`, `SafeMemcpyPod`, `IsPointerLikeX64`). Inside, keep business logic in pure functions. Never silence compiler/analyzer warnings.
   - **Make Operations Idempotent**: Running hooks, save/restore, or workshop filters twice must end in the same valid state.
2. **Then Make it Right**:
   - **Fix Root Causes**: A guard that only hides a crash is not a fix. Understand the engine RVA/struct invariant and fix the root cause.
   - **Sequence Work into Verifiable Units**: Finish one small unit, verify with `run_checks.bat`, then start the next.
   - **Crapper & Dryer Gate (`TesmioLoader/ub_check.py`)**:
     - Keep structural duplication `< 0.80` (`--dry-threshold 0.80`).
     - Split overly complex functions into focused single-responsibility helpers to keep Cyclomatic Complexity (`CC`) and `CRAP` low.
     - Delete dead `static` functions and unused symbols immediately.
3. **Practice**:
   - **Keep a Log**: Record each action and the reason it was taken, and report the log upon completion.
   - **Worktree Cleanup**: Remove temporary build artifacts (`.obj`, `.nativecodeanalysis.xml`) outside `build/` and `obj/`.
