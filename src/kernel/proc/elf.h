/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * elf.h - 64-bit Executable and Linkable Format (ELF64) loader definitions.
 */

#ifndef KERNEL_ELF_H
#define KERNEL_ELF_H

#include <stdint.h>
#include <stddef.h>
#include "mm/vmm.h"
#include "fs/vfs/vfs.h"

#define ELF_MAGIC       0x464C457F /* "\x7fELF" in little-endian */
#define ELF_CLASS_64    2
#define ELF_DATA_2LSB   1          /* Little endian */
#define ELF_EM_X86_64   0x3E

/* Segment types */
#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6
#define PT_TLS          7

/* Segment flags */
#define PF_X            0x1 /* Executable */
#define PF_W            0x2 /* Writable */
#define PF_R            0x4 /* Readable */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) Elf64_Phdr;

/* Auxiliary vector entries required by Musl libc.
 * T_PROP values (except NULL/IGNORE) are pushed value-first so the final
 * low->high order on the stack reads: a_type (=TAG) then a_val. */
#define AT_NULL         0
#define AT_IGNORE       1
#define AT_EXECFD       2
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
#define AT_NOTELF       10
#define AT_UID          11
#define AT_EUID         12
#define AT_GID          13
#define AT_EGID         14
#define AT_HWCAP        16
#define AT_CLKTCK       17
#define AT_SECURE       23
#define AT_RANDOM       25
#define AT_HWCAP2       26
#define AT_EXECFN       31
#define AT_SYSINFO      32   /* vDSO entry point (legacy) */
#define AT_SYSINFO_EHDR 33   /* vDSO ELF image base */

struct elf_load_info {
    uint64_t entry_point;
    uint64_t phdr_vaddr;
    uint16_t phnum;
    uint16_t phent;
    uint64_t brk_base;
};

int elf_validate(const Elf64_Ehdr *hdr);
int elf_load_file(struct vfs_node *file, pml4_t *pml4, struct elf_load_info *info);

#endif /* KERNEL_ELF_H */
