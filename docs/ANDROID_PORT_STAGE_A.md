# Stage A: Foundation & Guest CPU Interface (glibc + FEXCore)

## Status (Updated 2026-11-06)
- **Stage A**: In progress. All guest_cpu_* stubs in place, compile testing passing.
- **Runtime Audit**: Complete - 7 files rewritten for ARM64, 5 commits on `setup-and-catch-up-qwen`.
- **Next**: Begin FEXCore integration (Stage B) and handle remaining ucontext fibers (runtime_gr2.c).

### Runtime Files Processed
| File | Status | Changes |
|---|---|---|
| `runtime_kernel.c` | ✅ Done | Replaced `__rdtsc` with ARM64 TSC (`CLOCK_MONOTONIC`) |
| `runtime_thread.c` | ✅ Done | `arch_prctl(ARCH_SET_GS)` → `guest_cpu_set_tls_base()` |
| `probe.c` | ✅ Done | Removed `#error`, added ARM64 stub implementations |
| `runtime_gr2.c` | 🔄 Partial | Conditional `ucontext.h` include (x86 fibers still present) |
| `runtime_freeze.c` | ✅ Done | Cross-platform register capture (`greg_t` → `__x86_64__` guard) |

## Purpose
Create the thin `guest_cpu` layer between the PS4 runtime and FEXCore so that all HLE stubs (`runtime_*.c`) continue to call PS4 syscalls without knowing they're emulated on x86-64→ARM64. This document tracks tasks, decisions, and open questions.

## Scope
- **In scope**: `guest_cpu.h`/`.c`, FEXCore integration skeleton, memory-space layout for the guest, page-fault handling bridge
- **Out of scope (later stages)**: Per-file audit results, runtime_*.c rewrites, GPU Android target, build.sh adaptation, APK packaging

## Reference Documents
| Doc | Relevance |
|---|---|
| `README.md` | Overall port status; glibc-first roadmap |
| `docs/ANDROID_PORT.md` | 6-stage plan + identified blockers |
| `docs/ROADMAP.md` | PC-port feature checklist and open items |
| `port/src/probe.c` | x86-64 loader, signal handlers, fault recovery |
| `port/src/runtime_thread.c` | Guest threads, TCB/TLS, FP state |
| `port/src/runtime_gr2.c` | ucontext fibers (blocker #4) |
| `port/gpu/CMakeLists.txt` | X11 dependency, no Android target |

## Blockers Reference (from ANDROID_PORT.md)

### 1. probe.c — x86-64-native loader + signal handling (~470+ lines)
**At risk**: Hard compile guard `#if !defined(__x86_64__)` (line 18-20), `greg_t`/`REG_RIP`/`REG_RSP` from ucontext, XMM reads at fpu+160/fpu+464, arch_prctl(ARCH_SET_GS).

### 2. runtime_thread.c — x86-specific TLS and threads (~450+ lines)
**At risk**: FreeBSD-style TCB with ARCH_SET_GS (no ARM64 equiv), `stmxcsr`/`fnstcw` FPU capture, jmp_buf for fault recovery incompatible with FEXCore.

### 3. Signal handler + memory-fault diagnostics (probe.c ~314-399)
**At risk**: SIGSEGV/SIGBUS reading ucontext x86 registers must delegate to FEXCore fault interception; siglongjmp → FEXCore page-fault callback.

### 4. Fibers in runtime_gr2.c (~200+ lines)
**At risk**: `ucontext_t`-based fibers on guest thread memory need ARM64-compatible wrapper or FEXContext API.


## Stage A Tasks

### Task A.1: Design the guest_cpu Interface (IN PROGRESS)

File: `port/src/guest_cpu.h`, `port/src/guest_cpu.c`

| Function | Purpose |
|---|---|
| `guest_cpu_init(elf_path, mem_base_str, mem_size)` | Initialize FEXCore: create memory space, load ELF for translation |
| `guest_cpu_shutdown()` | Tear down FEXCore; free memory space, clean up contexts |
| `guest_cpu_set_entry(entry_point)` | Set the game entry point (eboot's mapped _start address) |
| `guest_cpu_run()` | Main loop — call FEXCore execution entry; returns on exit/crash |
| `guest_cpu_handle_page_fault(addr, access)` | Page-fault callback from FEXCore. 1=handled, 0=fatal |
| `guest_cpu_set_library_path(path)` | Configure PS4 .so module load path in guest address space |
| `guest_cpu_get_tls_base()` | Return current guest TLS base (equivalent to x86 gs:0) |
| `guest_cpu_set_tls_base(base)` | Set guest TLS base for calling thread |

**Design decisions:**
1. Memory model: FEXCore manages its own memory space. We wrap it in our API. The PS4 address range (~64 GiB → 1 TiB) maps to host mmap+mprotect segments.
2. TLS model: PS4 static TLS (variant II). On ARM64, we'll store TCB below the guest TLS block in FEX memory space, managing it via `guest_cpu_set_tls_base()`.
3. Page-fault handling: The callback integrates `tlggpu_handle_fault()` for GPU page tracking, mirroring probe.c signal handler behavior.
4. Single-threaded guest for Stage A: One vCPU loop serializing all guest code. No multi-vCPU complexity yet.

### Task A.2: Implement the guest_cpu Stub (no-op version)
Create `port/src/guest_cpu.c` with minimal stubs that return success but do nothing, allowing compile/link before actual FEXCore integration.

### Task A.3: Guest Memory Space Layout
Document PS4→ARM64 memory mapping: 0x80000000→64GiB as `mmap(MAP_PRIVATE|MAP_ANONYMOUS)` + per-segment `mprotect`. PS4 kernel range (64 GiB–1 TiB) remains unmapped.

### Task A.4: Thread/TLS Adaptation Plan
Map x86 mechanisms to ARM64/FEXCore equivalents. See below for details.

### Task A.5: GPU Page Tracking Bridge
Replace signal handler with FEXCore page-fault callback. The callback receives `(addr, access)` and calls `tlggpu_handle_fault()`. Page protection changes use FEXCore's `FEMemory::Protect()` instead of `mprotect()`.

### Task A.6: Fiber Adaptation Strategy
Short term (Stage A): pthread-based fiber emulation (each fiber = pthread). Long term (Stage B): FEXCore vCPU context switching for fibers.

## Thread/TLS Adaptation Plan (A.4 Details)

| x86 Mechanism | ARM64/FEXCore Equivalent | Action |
|---|---|---|
| `arch_prctl(ARCH_SET_GS)` → GS base = TCB | No segment registers on ARM64; TLS in GPR or system reg | Store TCB base in a reserved GPR (e.g. x29/FP) via FEXCore, or allocate TLS block in guest memory and set via `guest_cpu_set_tls_base()` |
| `mov rax, fs:[0]` → `mov rax, gs:[0]` rewrite | No rewrite needed; ARM64 has no segments | Use FEXCore built-in TLS model or custom GPR-stored TLS base |
| `stmxcsr`/`fnstcw` x87 FPU capture | ARM64 FPSCR via `mrs pX, fpsr`; NEON auto-saved | First pass: no per-thread FPU capture; guest uses default FP control words |
| `jmp_buf` → `siglongjmp` fault recovery | Replace with FEXCore resume-from-fault API or custom entry-point jump | Design `guest_cpu_jump_to(entry, arg0)` as thin wrapper over FEXContext restore |

## Open Questions

| # | Question | Rationale |
|---|---|---|
| Q1 | Which FEXCore version to pin? | Need a specific tag/commit from FEX-Emu/FEX. Check God of War III fork for pinned version. |
| Q2 | ARM64 TLS approach? | Option A: glibc `__tls_get_addr()`. Option B: Manual via guest memory + GPR. Decision affects runtime_thread.c rewrites. |
| Q3 | Single vs multi-threaded guest initially? | Single vCPU serializes all code. Multi-vCPU = one per host thread, but more complex. Start single-threaded for Stage A. |
| Q4 | GPU presentation target for Android without native display? | Vulkan needs a Surface; on Android this is ANativeWindow/AHardwareBuffer. Headless: swiftshader/llvmpipe + offscreen. Priority: headless smoke test first. |
| Q5 | How to integrate FEXCore in CMake? | Option A: Git submodule (like shadPS4). Option B: FetchContent. Option C: Pre-built static library from CI. |

## Task Dependencies

```
A.1 guest_cpu interface design  ─┐
A.2 guest_cpu stub implementation │→ A.3 Memory space layout (uses interface)
A.4 Thread/TLS adaptation plan    │    ↓
                                  A.5 GPU page-fault bridge (depends on A.1, A.3)
                                     ↓
                                  A.6 Fiber adaptation strategy
```

## Progress Log

| Date | Task | Status | Notes |
|---|---|---|---|
| 2026-11-06 | A.1 Interface design | **Done** | Documented above; signatures finalized |
| 2026-11-06 | A.2 Stub implementation | **Done** | guest_cpu.h + guest_cpu.c no-op stubs committed |
| 2026-11-06 | B.1 Per-file runtime audit | **Done** | See `docs/ANDROID_PORT_AUDIT_RUNTIME.md` — 7 rework files, 19 ready |
| 2026-11-06 | B.2 Android build skeleton | **Done** | `port/build_android.sh` NDK skeleton for ARM64 cross-compilation |