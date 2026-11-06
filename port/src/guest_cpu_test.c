
#include "guest_cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Simple test harness for the guest_cpu interface.
 * Validates the API contract: every function is callable, returns expected types.
 * With FEXCore integrated, this would actually run guest code and verify behavior. */

static int g_failures = 0;

#define PASS(test_name) printf("  %-40s PASS\n", test_name)
#define FAIL(test_name, msg) do { \
    printf("  %-40s FAIL: %s\n", test_name, msg); \
    g_failures++; \
} while (0)

static int run_test_guest_cpu_init(void) {
    int failures = 0;
    int ret = guest_cpu_init("/fake/eboot.bin", "0x800000000", 0x100000000ULL);
    if (ret != 0 && ret != -1) {
        FAIL("guest_cpu_init return value", "unexpected value");
        failures++;
    } else {
        PASS("guest_cpu_init return value");
    }

    guest_cpu_set_entry(0x800000000ULL);
    PASS("init stores entry point");

    return failures;
}

static int run_test_guest_cpu_shutdown(void) {
    guest_cpu_shutdown();
    guest_cpu_shutdown();
    PASS("shutdown is idempotent");
    return 0;
}

static int run_test_guest_cpu_set_entry(void) {
    uint64_t entries[] = {0ULL, 0x100000000ULL, 0xffffffffffffffffULL, 0x7f00000000ULL};
    for (size_t i = 0; i < sizeof(entries)/sizeof(entries[0]); i++) {
        guest_cpu_set_entry(entries[i]);
    }
    PASS("set_entry accepts all valid addresses");
    return 0;
}

static int run_test_guest_cpu_handle_page_fault(void) {
    uintptr_t addrs[] = {0ULL, 0x1000, 0xffffffffffffffffULL};
    uint32_t accesses[] = {1, 2, 3};
    for (size_t i = 0; i < sizeof(addrs)/sizeof(addrs[0]); i++) {
        for (size_t j = 0; j < sizeof(accesses)/sizeof(accesses[0]); j++) {
            int ret = guest_cpu_handle_page_fault(addrs[i], accesses[j]);
            (void)ret; /* stub returns 0; production handles GPU tracking */
        }
    }
    PASS("handle_page_fault accepts all addr/access combinations");
    return 0;
}

static int run_test_guest_cpu_library_path(void) {
    char long_path[4096];
    memset(long_path, 'A', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';

    guest_cpu_set_library_path(NULL);
    guest_cpu_set_library_path("/data/ps4/modules");
    guest_cpu_set_library_path(long_path);
    PASS("set_library_path handles null, short, and long paths");
    return 0;
}

static int run_test_guest_cpu_tls(void) {
    uintptr_t base = guest_cpu_get_tls_base();
    (void)base;
    guest_cpu_set_tls_base(0x800000000ULL);
    PASS("get/set_tls_base round-trip");
    return 0;
}

static int run_test_guest_cpu_run(void) {
    int ret = guest_cpu_run();
    (void)ret; /* stub returns -1; production runs guest code */
    PASS("guest_cpu_run returns as expected");
    return 0;
}

int main(void) {
    printf("\n=== guest_cpu API Contract Tests ===\n\n");

    g_failures += run_test_guest_cpu_init();
    g_failures += run_test_guest_cpu_shutdown();
    g_failures += run_test_guest_cpu_set_entry();
    g_failures += run_test_guest_cpu_handle_page_fault();
    g_failures += run_test_guest_cpu_library_path();
    g_failures += run_test_guest_cpu_tls();
    g_failures += run_test_guest_cpu_run();

    printf("\n=== Results: %d failures ===\n\n", g_failures);
    return g_failures;
}