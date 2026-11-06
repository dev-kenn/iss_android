/* Guest CPU abstraction layer — Stage A stub implementation.
 *
 * All functions compile and link but are no-ops or return defaults.
 * Actual FEXCore integration replaces this file in a later stage.
 *
 * This allows the rest of port/src/ to be compiled with the new header
 * before the guest CPU interface is fully wired up.
 */

#include "guest_cpu.h"
#include <stdio.h>
#include <string.h>

/* Internal state — empty during Stage A stub; populated in Stage B. */
static int initialized = 0;
static uintptr_t guest_entry = 0;
static char library_path[4096];
static uint64_t mem_base_addr = 0;
static uint64_t mem_size_total = 0;

int guest_cpu_init(const char *elf_path, const char *mem_base, uint64_t mem_size) {
    (void)elf_path;
    (void)mem_base;
    (void)mem_size;
    initialized = 1;
    mem_size_total = mem_size;
    /* Stage A stub: no FEXCore memory space created yet.
     * In Stage B, this will allocate the PS4 guest address range
     * (~64 GiB → 1 TiB) via mmap(MAP_PRIVATE|MAP_ANONYMOUS) and
     * configure FEXCore's memory space accordingly. */
    fprintf(stderr, "guest_cpu_init: stub mode (not connected to FEXCore yet)\n");
    return 0;
}

void guest_cpu_shutdown(void) {
    initialized = 0;
    guest_entry = 0;
    memset(library_path, 0, sizeof(library_path));
    mem_base_addr = 0;
    mem_size_total = 0;
    /* Stage A stub: no FEXCore cleanup to perform. */
    fprintf(stderr, "guest_cpu_shutdown: stub mode\n");
}

void guest_cpu_set_entry(uintptr_t entry_point) {
    guest_entry = entry_point;
    /* Stage A stub: entry point stored but not connected to FEXCore. */
}

int guest_cpu_run(void) {
    if (!initialized) {
        fprintf(stderr, "guest_cpu_run: called without guest_cpu_init\n");
        return -1;
    }
    /* Stage A stub: no guest execution loop. Return fatal until FEXCore is connected. */
    fprintf(stderr, "guest_cpu_run: stub mode — no FEXCore connection yet (entry at 0x%lx)\n",
            (unsigned long)guest_entry);
    return -1;
}

int guest_cpu_handle_page_fault(uintptr_t addr, uint32_t access) {
    (void)addr;
    (void)access;
    /* Stage A stub: no page tracking yet. Treat all faults as unhandled. */
    fprintf(stderr, "guest_cpu_handle_page_fault: stub mode — fault at 0x%lx, access=%u\n",
            (unsigned long)addr, access);
    return 0; /* fatal: page fault not handled without GPU bridge */
}

void guest_cpu_set_library_path(const char *path) {
    if (path) {
        strncpy(library_path, path, sizeof(library_path) - 1);
        library_path[sizeof(library_path) - 1] = '\0';
    } else {
        memset(library_path, 0, sizeof(library_path));
    }
}

uintptr_t guest_cpu_get_tls_base(void) {
    /* Stage A stub: return zero. In Stage B, this will read from the
     * FEXCore context or a reserved GPR holding the TLS base pointer. */
    return 0;
}

void guest_cpu_set_tls_base(uintptr_t base) {
    (void)base;
    /* Stage A stub: no TLS management yet. In Stage B, this will store
     * the TCB base in a reserved GPR or FEXCore TLS slot. */
}
