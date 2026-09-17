/*
 * Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
 * All rights reserved.
 *
 * elf.c - 64-bit ELF executable parser and demand-segment loader.
 */

#include <stdint.h>
#include <stddef.h>
#include "elf.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kheap.h"
#include "fs/vfs/vfs.h"
#include "include/serial.h"

int elf_validate(const Elf64_Ehdr *hdr) {
    if (!hdr) return 0;
    if (hdr->e_ident[0] != 0x7F ||
        hdr->e_ident[1] != 'E'  ||
        hdr->e_ident[2] != 'L'  ||
        hdr->e_ident[3] != 'F') {
        return 0;
    }
    if (hdr->e_ident[4] != ELF_CLASS_64) return 0;
    if (hdr->e_ident[5] != ELF_DATA_2LSB) return 0;
    if (hdr->e_machine != ELF_EM_X86_64) return 0;
    return 1;
}

int elf_load_file(struct vfs_node *file, pml4_t *pml4, struct elf_load_info *info) {
    if (!file || !pml4 || !info) return -1;

    Elf64_Ehdr ehdr;
    if (file->read(file, 0, sizeof(Elf64_Ehdr), &ehdr) != sizeof(Elf64_Ehdr)) {
        serial_puts("[ELF] Failed to read ELF header!\n");
        return -1;
    }

    if (!elf_validate(&ehdr)) {
        serial_puts("[ELF] Invalid ELF64 header or wrong architecture!\n");
        return -1;
    }

    size_t ph_total_size = ehdr.e_phnum * sizeof(Elf64_Phdr);
    Elf64_Phdr *phdrs = (Elf64_Phdr *)kmalloc(ph_total_size);
    if (!phdrs) {
        serial_puts("[ELF] Out of kernel memory reading program headers!\n");
        return -1;
    }

    if (file->read(file, ehdr.e_phoff, ph_total_size, phdrs) != (int64_t)ph_total_size) {
        serial_puts("[ELF] Failed to read program headers!\n");
        kfree(phdrs);
        return -1;
    }

    info->entry_point = ehdr.e_entry;
    info->phnum = ehdr.e_phnum;
    info->phent = ehdr.e_phentsize;
    info->phdr_vaddr = 0;
    info->brk_base = 0;

    uint64_t max_vaddr = 0;

    /* Load each PT_LOAD segment into user address space */
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr *ph = &phdrs[i];

        if (ph->p_type == PT_PHDR) {
            info->phdr_vaddr = ph->p_vaddr;
        }

        if (ph->p_type != PT_LOAD) continue;

        uint64_t start_vaddr = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_vaddr = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (end_vaddr > max_vaddr) {
            max_vaddr = end_vaddr;
        }

        uint64_t flags = PAGE_PRESENT | PAGE_USER;
        if (ph->p_flags & PF_W) flags |= PAGE_WRITABLE;

        /* Allocate and map pages */
        for (uint64_t v = start_vaddr; v < end_vaddr; v += PAGE_SIZE) {
            uint64_t existing = vmm_virt_to_phys(pml4, v);
            if (!existing) {
                void *phys = pmm_alloc_page();
                if (!phys) {
                    serial_puts("[ELF] Out of physical memory loading segment!\n");
                    kfree(phdrs);
                    return -1;
                }
                vmm_map_page(pml4, v, (uint64_t)phys, flags);
            }
        }

        /* Read segment data into memory */
        if (ph->p_filesz > 0) {
            uint8_t *seg_buf = (uint8_t *)kmalloc(ph->p_filesz);
            if (!seg_buf) {
                kfree(phdrs);
                return -1;
            }

            file->read(file, ph->p_offset, ph->p_filesz, seg_buf);

            /* Copy into mapped pages */
            uint64_t copied = 0;
            while (copied < ph->p_filesz) {
                uint64_t cur_vaddr = ph->p_vaddr + copied;
                uint64_t page_offset = cur_vaddr & (PAGE_SIZE - 1);
                uint64_t chunk = PAGE_SIZE - page_offset;
                if (chunk > (ph->p_filesz - copied)) {
                    chunk = ph->p_filesz - copied;
                }

                uint64_t phys = vmm_virt_to_phys(pml4, cur_vaddr);
                /* Access physical page through HHDM (0xFFFF800000000000) */
                uint8_t *page_dest = (uint8_t *)(0xFFFF800000000000ULL + (phys & PAGE_ADDR_MASK) + page_offset);

                for (uint64_t b = 0; b < chunk; b++) {
                    page_dest[b] = seg_buf[copied + b];
                }

                copied += chunk;
            }

            kfree(seg_buf);
        }

        /* Zero out BSS section (where p_memsz > p_filesz) */
        if (ph->p_memsz > ph->p_filesz) {
            uint64_t bss_start = ph->p_vaddr + ph->p_filesz;
            uint64_t bss_len = ph->p_memsz - ph->p_filesz;
            uint64_t cleared = 0;

            while (cleared < bss_len) {
                uint64_t cur_vaddr = bss_start + cleared;
                uint64_t page_offset = cur_vaddr & (PAGE_SIZE - 1);
                uint64_t chunk = PAGE_SIZE - page_offset;
                if (chunk > (bss_len - cleared)) {
                    chunk = bss_len - cleared;
                }

                uint64_t phys = vmm_virt_to_phys(pml4, cur_vaddr);
                uint8_t *page_dest = (uint8_t *)(0xFFFF800000000000ULL + (phys & PAGE_ADDR_MASK) + page_offset);

                for (uint64_t b = 0; b < chunk; b++) {
                    page_dest[b] = 0;
                }

                cleared += chunk;
            }
        }
    }

    if (info->phdr_vaddr == 0) {
        /* If no explicit PT_PHDR segment, find the LOAD segment containing e_phoff */
        for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
            Elf64_Phdr *ph = &phdrs[i];
            if (ph->p_type == PT_LOAD && ehdr.e_phoff >= ph->p_offset && ehdr.e_phoff < ph->p_offset + ph->p_filesz) {
                info->phdr_vaddr = ph->p_vaddr + (ehdr.e_phoff - ph->p_offset);
                break;
            }
        }
    }

    info->brk_base = max_vaddr;
    kfree(phdrs);
    return 0;
}
