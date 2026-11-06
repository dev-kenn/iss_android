# Stage B: Per-File Runtime Audit + Android Build Skeleton

## 1. Per-File Runtime Audit

This table maps every `port/src/runtime_*.c` to its PS4 syscalls and ARM64 blockers.
Goal: clear plan for rewriting each file when FEXCore guest CPU is available.

### Files with x86-specific code (require rework)

| # | File | x86-specific code | Blockers | Rewrite strategy |
|---|------|-------------------|----------|------------------|
| 1 | `runtime.c` | `_Thread_local` TLS block array; module TLS lookup via `__tls_get_addr()` equivalent | **Medium**: `_Thread_local` on ARM64 needs custom guest model | Replace with explicit `guest_cpu_get_module_tls()` calls; keep `_Thread_local` in guest's view only |
| 2 | `runtime_freeze.c` | `greg_t` + `ucontext_t` + `REG_RIP/RSP/RBP` for thread state capture via signal handlers | **High**: ARM64 signal frame has different reg layout; SIGURG approach needs rewrite for FEXCore | Replace with FEXCore page-fault context capture; read registers via guest CPU state |
| 3 | `runtime_gr2.c` | `ucontext_t` + `getcontext/makecontext/swapcontext` for fibers + `__thread` TLS | **Critical**: Fibers on game's own memory broken by translator | Pthread-based fiber emulation short-term; FEXCore vCPU per-fiber long-term |
| 4 | `runtime_kernel.c` | `#include x86intrin.h` + `__rdtsc()` (3 sites) for TSC reads | **High**: `__rdtsc` is x86-only; ARM64 has no TSC instruction | Replace with `clock_gettime`-scaled simulation at 3.5 GHz |
| 5 | `runtime_thread.c` | `arch_prctl(ARCH_SET_GS)` + `stmxcsr/fnstcw` FPU capture + `jmp_buf/siglongjmp` fault recovery | **Critical**: Three blockers — TLS model, FPU state, fault recovery | Full rewrite: ARM64 guest TLS via GPR/store; skip per-thread FPU capture; replace siglongjmp with FEXCore resume-from-fault |
| 6 | `cpu_check.c` | `__get_cpuid()` from `<cpuid.h>` for CPUID instruction reads | **High**: CPUID is x86-specific; ARM64 uses MIDR registers | Replace with ARM64 feature detection or skip via FEXCore-reported capabilities |
| 7 | `probe.c` | Hard compile guard, signal handlers reading ucontext_t gregs, XMM/FPU regs, arch_prctl(ARCH_SET_GS), red-zone clobber | **Critical**: Entire loader is x86-64 specific | See Stage A plan for detailed rewrite strategy |
| 8 | `isa_emulate.c` / `isa_rewrite.c` | ISA emulation tables using x86 instruction encoding + zydis decoding | **Medium**: Wire up to FEXCore; no code rewrite needed | Link with FEXCore which handles ISA emulation automatically |

### Files with Zero Blockers (ARM64-ready as-is)

These 18 files need **no changes** for ARM64 — they use only POSIX libc, pthreads, or external libraries:
- `runtime_ajm.c` (audio decoder stubs)
- `runtime_audio.c` (SDL/libavcodec audio playback)
- `runtime_content.c` (POSIX file + env vars)
- `runtime_file.c` (POSIX I/O wrappers)
- `runtime_infamous.c` ← **Model of clean code** — all HLE exports, no platform deps
- `runtime_gr2_audio3d.c` (audio3d stubs)
- `runtime_gr2_font.c` (FreeType font rendering)
- `runtime_gr2_json.c` (JSON string parsing)
- `runtime_gr2_np.c` (NpManager offline stubs)
- `runtime_mp3.c` (libmp3lame/libavcodec)
- `runtime_mutex.c` (pthread-backed mutexes)
- `runtime_pad.c` (SDL3 input)
- `runtime_rtc.c` (clock-based RTC stubs)
- `runtime_rwlock.c` (pthread_rwlock_t)
- `runtime_savedata.c` (POSIX file I/O + crypto hashes)
- `runtime_sema.c` (pthread condition variables)
- `runtime_services.c` (NP/system services)
- `runtime_time.c` (POSIX clocks)

### Summary: Blocker Severity (Updated 2026-11-06)

| Severity | Count | Files | Status |
|----------|-------|-------|--------|
| **Critical** | 3 | probe.c, runtime_thread.c, runtime_gr2.c | 2/3 Done; runtime_gr2.c still has x86 fibers |
| **High** | 4 | cpu_check.c, runtime_freeze.c, runtime_kernel.c, (runtime_memory*) | 2/4 Done; cpu_check.c pending |
| **Medium** | 3 | runtime.c (TLS), isa_emulate.c, isa_rewrite.c | Wire up to FEXCore later |
| **None** | 18+ | All files listed above | No changes needed |

Total: ~26+ files audited. 7 require active rework; 19+ are ARM64-ready or need only wiring.