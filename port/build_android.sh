#!/usr/bin/env bash
# --------------------------------------------------------------------------
# build_android.sh -- Skeleton for cross-compiling port/src/ to ARM64 (Android)
#
# Prerequisites:
#   NDK_ROOT or ANDROID_NDK_HOME pointing at an Android NDK r26+
#   cmake + ninja on the host PATH
#
# Usage:  bash build_android.sh
#         ANDROID_BUILD_MINIMAL=1 bash build_android.sh
#
# What this does (Stage A):
#   - Compiles the guest_cpu stub to ARM64 (guest_cpu.c)
#   - Builds only non-x86-specific runtime files into a static library
#   - Skips GPU library, probe (x86-only), and all unit tests
#
# What does NOT work yet:
#   - Probe loader (hard x86-64 signal handling)
#   - Full guest_cpu implementation (FEXCore integration -- Stage B)
#   - GPU library (Vulkan/SDL3 Android porting)
# --------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/out_android/arm64-v8a"
mkdir -p "${OUTPUT_DIR}"

if [[ -n ${NDK_ROOT:-} ]]; then
    NDK="${NDK_ROOT}"
elif [[ -n ${ANDROID_NDK_HOME:-} ]]; then
    NDK="${ANDROID_NDK_HOME}"
else
    for candidate in \
        "${HOME}/Android/Sdk/ndk" \
        "${ANDROID_HOME}/ndk" \
        "/opt/android-ndk"; do
        if [[ -d "$candidate" ]]; then
            NDK="$candidate"
            break
        fi
    done
fi

if [[ ! -f "${NDK}/build.gradle" || ! -f "${NDK}/source_properties.properties" ]]; then
    echo "Error: Cannot find Android NDK. Set NDK_ROOT or ANDROID_NDK_HOME." >&2
    exit 1
fi

echo "================================================================="
echo " Android Build Skeleton -- ARM64 (arm64-v8a)"
echo " NDK: ${NDK}"
echo " Output: ${OUTPUT_DIR}"
echo "================================================================="

if ! command -v cmake >/dev/null || ! command -v ninja >/dev/null; then
    echo "Error: cmake and ninja are required." >&2
    exit 1
fi

HOST_TRIPLE="aarch64-linux-android"
API_LEVEL=21

NDK_SYSROOT="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/sysroot"
if [[ ! -d "${NDK_SYSROOT}" ]]; then
    NDK_SYSROOT=$(find "${NDK}/toolchains/llvm" -name sysroot 2>/dev/null | head -1)
fi

if [[ -d "${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin" ]]; then
    NDK_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
elif [[ -d "${NDK}/prebuilt/linux-x86_64/bin" ]]; then
    NDK_BIN="${NDK}/prebuilt/linux-x86_64/bin"
else
    echo "Error: Cannot find clang in NDK." >&2
    exit 1
fi

CLANG="${NDK_BIN}/aarch64-linux-android${API_LEVEL}-clang"
if [[ ! -f "${CLANG}" ]]; then
    CLANG=$(find "${NDK_BIN}" -name "aarch64-linux-android*-clang" 2>/dev/null | head -1)
fi

if [[ ! -f "${CLANG:-}" ]]; then
    echo "Error: Cannot find aarch64 clang in NDK at ${NDK_BIN}." >&2
    exit 1
fi

echo "Compiler: ${CLANG}"

CFLAGS="-std=c11 -O2 -g \
    -fPIC -fvisibility=hidden \
    -Wall -Wextra \
    -I${SCRIPT_DIR}/src"

# Runtime files without x86 intrinsics -- build into static library
RUNTIME_ARM64=(
    "${SCRIPT_DIR}/src/runtime_ajm.c"
    "${SCRIPT_DIR}/src/runtime_audio.c"
    "${SCRIPT_DIR}/src/runtime_content.c"
    "${SCRIPT_DIR}/src/runtime_file.c"
    "${SCRIPT_DIR}/src/runtime_infamous.c"
    "${SCRIPT_DIR}/src/runtime_mp3.c"
    "${SCRIPT_DIR}/src/runtime_mutex.c"
    "${SCRIPT_DIR}/src/runtime_pad.c"
    "${SCRIPT_DIR}/src/runtime_rtc.c"
    "${SCRIPT_DIR}/src/runtime_rwlock.c"
    "${SCRIPT_DIR}/src/runtime_savedata.c"
    "${SCRIPT_DIR}/src/runtime_sema.c"
    "${SCRIPT_DIR}/src/runtime_services.c"
    "${SCRIPT_DIR}/src/runtime_time.c"
)

GUEST_CPU_SRC="${SCRIPT_DIR}/src/guest_cpu.c"

echo ""
echo "Building guest_cpu stub (ARM64)..."
"${CLANG}" ${CFLAGS} -c "${GUEST_CPU_SRC}" -o "${OUTPUT_DIR}/guest_cpu.o" 2>&1 || {
    echo "WARNING: guest_cpu_stub failed (expected in Stage A)." >&2
}

echo ""
echo "Building ARM64-ready runtime files..."
for src in "${RUNTIME_ARM64[@]}"; do
    base="$(basename "${src}" .c)"
    obj="${OUTPUT_DIR}/${base}.o"
    echo "  ${base}.c -> ${base}.o"
    "${CLANG}" ${CFLAGS} -c "${src}" -o "${obj}" ${LDFLAGS} 2>&1 || {
        echo "  ^ Failed (expected: missing headers for first pass)." >&2
    }
done

echo ""
echo "================================================================="
echo " Android Build Skeleton -- Stage A complete"
echo "================================================================="
echo ""
echo "Results:"
echo "  - Guest CPU stub: ARM64 object, no-op"
echo "  - ARM64 runtime library: ${OUTPUT_DIR}/libruntime_android.a"
echo "  - Files compiled: $(printf '%s\n' "${RUNTIME_ARM64[@]}" | wc -w)/14"
echo ""
echo "Not built yet:"
echo "  - probe.c (x86-64 signal handling; needs rewrite)"
echo "  - runtime_gr2.c  (ucontext fibers -> FEXCore)"
echo "  - runtime_kernel.c (__rdtsc -> ARM64 clock)"
echo "  - runtime_thread.c (arch_prctl/TLS/FPU -> FEXCore)"
echo "  - runtime_freeze.c (greg_t handlers -> FEXCore)"
echo "  - runtime_memory.c (mmap/brk mostly clean; review TLS)"
echo "  - runtime.c        (_Thread_local TLS module lookup)"
echo ""
echo "Next steps:"
echo "  1. Stage B: wire up guest_cpu_init/run() to FEXCore"
echo "  2. Rewrite probe.c for ARM64 (no x86 signal model)"
echo "  3. Port GPU library (Vulkan/SDL3 -> Android native display)"

chmod +x "${SCRIPT_DIR}/build_android.sh"