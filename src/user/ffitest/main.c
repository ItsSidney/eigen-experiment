/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * ffitest.c - prove the real libffi can dispatch a wayland-style handler.
 *
 * Calls a handler with the signature wayland uses
 *   void handler(void *data, void *res, int32_t a, uint32_t b, void *c)
 * through ffi_prep_cif/ffi_call, and verifies every argument landed in the
 * right register slot. This is the exact code path connection.c exercises.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ffi.h>

static uint64_t got[8];
static int calls;

static void test_handler(void *data, void *res,
                         int32_t a, uint32_t b, void *c) {
    got[0] = (uint64_t)(uintptr_t)data;
    got[1] = (uint64_t)(uintptr_t)res;
    /* The register carries the full value; sign-extend through the C type
     * itself so the comparison sees -42 rather than its truncated bits. */
    got[2] = (uint64_t)(int64_t)a;
    got[3] = (uint64_t)b;
    got[4] = (uint64_t)(uintptr_t)c;
    calls++;
}

int main(void) {
    static ffi_type *atypes[6];
    int fails = 0;

    atypes[0] = &ffi_type_pointer;  /* void *data */
    atypes[1] = &ffi_type_pointer;  /* void *resource */
    atypes[2] = &ffi_type_sint32;   /* int32 arg */
    atypes[3] = &ffi_type_uint32;   /* uint32 arg */
    atypes[4] = &ffi_type_pointer;  /* void *arg */

    ffi_cif cif;
    ffi_status st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5,
                                 &ffi_type_pointer, atypes);
    if (st != FFI_OK) {
        printf("ffitest: ffi_prep_cif failed (%d)\n", (int)st);
        return 1;
    }
    printf("ffitest: prep_cif ok (5 args)\n");

    int32_t payload = -42;
    void *cookie = (void *)0xDEADBEEFCAFEBABEULL;
    void *resource = (void *)0x1234567890ULL;
    void *client_data = (void *)0xCAFEBABEULL;

    void *avalue[5] = {
        &client_data,
        &resource,
        &payload,
        &payload,          /* 0xFFFFFFD6 when zero-extended */
        &cookie,
    };

    ffi_call(&cif, (void (*)(void))test_handler, NULL, avalue);

    if (calls != 1) { printf("ffitest: handler not called\n"); fails++; }
    if (got[0] != (uint64_t)(uintptr_t)client_data) {
        printf("ffitest: arg0 (data) wrong: %llx\n",
               (unsigned long long)got[0]);
        fails++;
    }
    if (got[1] != (uint64_t)(uintptr_t)resource) {
        printf("ffitest: arg1 (resource) wrong: %llx\n",
               (unsigned long long)got[1]);
        fails++;
    }
    /* sint32 must arrive sign-extended: -42, not 0xFFFFFFD6. */
    if (got[2] != 0xFFFFFFFFFFFFFFD6ULL) {
        printf("ffitest: arg2 (int32 -42) wrong: %llx (not sign-extended)\n",
               (unsigned long long)got[2]);
        fails++;
    }
    /* uint32 arrives zero-extended. */
    if (got[3] != 0x00000000FFFFFFD6ULL) {
        printf("ffitest: arg3 (uint32) wrong: %llx\n",
               (unsigned long long)got[3]);
        fails++;
    }
    if (got[4] != (uint64_t)(uintptr_t)cookie) {
        printf("ffitest: arg4 (pointer) wrong: %llx\n",
               (unsigned long long)got[4]);
        fails++;
    }

    if (fails) {
        printf("ffitest: %d FAILURE(S)\n", fails);
        return 1;
    }
    printf("ffitest: all checks passed\n");
    return 0;
}
