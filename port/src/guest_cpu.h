#ifndef GUEST_CPU_H
#define GUEST_CPU_H

#include <stdint.h>
#include <stddef.h>

/* Guest CPU abstraction layer.
 *
 * Thin wrapper around FEXCore (x86-64 → ARM64 dynamic translator).
 * All HLE runtime code in port/src/ calls guest_cpu_* functions only;
 * they never see FEXCore types directly.
 *
 * Stage A: stub interface + no-op implementation for compile/link testing.
 * Actual FEXCore integration happens in Stage B (separate branch/work).
 */

/* Initialize the guest CPU environment.
 *
 * elf_path  — path to the PS4 eboot.bin (or equivalent executable image)
 * mem_base  — base address of the guest memory space (as string for later validation)
 * mem_size  — total size of the guest addressable memory region
 *
 * Returns:
 *   0 on success, -1 on failure (check stderr for details)
 *
 * Sets up FEXCore memory space and initializes the ELF loader.
 */
int guest_cpu_init(const char *elf_path, const char *mem_base, uint64_t mem_size);

/* Tear down the guest CPU environment and release all resources.
 * Safe to call when no guest code is executing (after guest_cpu_run returns).
 */
void guest_cpu_shutdown(void);

/* Set the entry point address for the guest main thread.
 * The address must be within the guest memory space mapped by guest_cpu_init().
 * Called before guest_cpu_run() to configure where execution begins.
 */
void guest_cpu_set_entry(uintptr_t entry_point);

/* Execute guest code from the configured entry point.
 *
 * This is the main run loop. It will call into the translated code,
 * handle page faults via guest_cpu_handle_page_fault(), and continue
 * until the guest exits or a fatal fault occurs.
 *
 * Returns:
 *   0 on clean exit
 *  >0 on non-fatal event (e.g., game restart requested)
 *   -1 on fatal error (check stderr for details)
 */
int guest_cpu_run(void);

/* Page-fault handler callback invoked by FEXCore.
 *
 * Called when a guest memory access fails due to page protection.
 * Used by GPU page tracking: the GPU writes to mapped pages, then
 * those pages are read-protected; on subsequent reads, this callback
 * flushes pending GPU writes before allowing the access.
 *
 * Parameters:
 *   addr     — guest virtual address that caused the fault
 *   access   — FEXCore page fault access type (1=read, 2=write, 3=exec)
 *
 * Returns:
 *   1 = handled successfully (page protection updated; resume guest)
 *   0 = fatal (game should terminate)
 */
int guest_cpu_handle_page_fault(uintptr_t addr, uint32_t access);

/* Configure the path prefix for PS4 library modules (.so files).
 * The loader resolves module names against this prefix when loading
 * started modules via sceKernelLoadStartModule().
 */
void guest_cpu_set_library_path(const char *path);

/* Get the current guest TLS base pointer.
 *
 * Equivalent to reading x86's gs:0 on the host side. On ARM64/FEXCore,
 * this is managed internally and exposed through this accessor.
 *
 * Returns the TLS base address for the current guest thread context.
 */
uintptr_t guest_cpu_get_tls_base(void);

/* Set the guest TLS base pointer for the calling thread.
 *
 * Called during guest thread creation to assign a TCB in guest memory.
 * The base must be within the guest address space and aligned to 16 bytes.
 */
void guest_cpu_set_tls_base(uintptr_t base);

#endif /* GUEST_CPU_H */
