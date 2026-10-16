# Copyright (C) 2023-2026 Eldrick Sidney <bedinacks@gmail.com>
# All rights reserved.

CC     = gcc
LD     = ld
OBJCOPY = objcopy

MUSL_GCC = toolchain/musl/bin/musl-gcc
MUSL_INC = toolchain/musl/include

CFLAGS = \
    -ffreestanding \
    -fno-stack-protector \
    -fno-stack-check \
    -fno-lto \
    -fno-PIE \
    -fno-PIC \
    -m64 \
    -march=x86-64 \
    -mno-80387 \
    -mno-mmx \
    -mno-red-zone \
    -mcmodel=kernel \
    -std=c11 \
    -Wall \
    -Wextra \
    -MMD -MP \
    -Isrc/kernel \
    -Ithird_party/libvterm/include \
    -c

LDFLAGS = \
    -T config/linker.ld \
    -static \
    -nostdlib \
    -no-pie \
    -z max-page-size=0x1000 \
    -z noexecstack \
    -m elf_x86_64

NASM   = nasm
ASMFLAGS = -f elf64

KSRCS_C   = $(shell find src/kernel -name '*.c' ! -path 'src/kernel/vdso/*')
KSRCS_ASM = $(shell find src/kernel -name '*.asm')
KOBJS     = $(patsubst src/kernel/%.c, build/%.o, $(KSRCS_C)) \
            $(patsubst src/kernel/%.asm, build/%.o, $(KSRCS_ASM))

# ── third_party/libvterm (vendored terminal emulator, MIT) ──────────────
# Built with its own flags rather than $(CFLAGS):
#   -fno-builtin  so gcc cannot synthesise calls to a libc we do not link
#   -Isrc/kernel/compat  so its <stdlib.h>/<string.h>/<stdio.h> resolve to
#                 the kernel shims that forward to kheap
#   -std=c99            upstream expects C99
# Only src/*.c is compiled; bin/ and t/ contain upstream tools/tests.
LVTERM_DIR  = third_party/libvterm
LVTERM_SRCS = $(shell find $(LVTERM_DIR)/src -maxdepth 1 -name '*.c')
LVTERM_OBJS = $(patsubst $(LVTERM_DIR)/%.c, build/$(LVTERM_DIR)/%.o, $(LVTERM_SRCS))
LVTERM_INC  = -I$(LVTERM_DIR)/include -I$(LVTERM_DIR)/src -Isrc/kernel/compat \
              -include src/kernel/compat/vterm_libc.h
build/$(LVTERM_DIR)/%.o: $(LVTERM_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) -ffreestanding -fno-builtin -fno-stack-protector -fno-stack-check \
	    -fno-lto -fno-PIE -fno-PIC -m64 -march=x86-64 -mno-red-zone \
	    -mcmodel=kernel -std=c99 -O2 $(LVTERM_INC) -c $< -o $@

# The libc shims themselves must be built without builtin recognition, or gcc
# can rewrite their copy/zero loops into calls to memset/memcpy and recurse.
build/compat/vterm_libc.o: src/kernel/compat/vterm_libc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $< -o $@

all: eigen.iso

build/%.o: src/kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

# vterm_glue.c peeks at the vendored VTermState struct (mode.cursor, which
# the public API does not expose), so it needs the library's private header
# directory on its include path as well.
build/drivers/char/vterm_glue.o: src/kernel/drivers/char/vterm_glue.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Ithird_party/libvterm/src $< -o $@

build/%.o: src/kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) $(ASMFLAGS) $< -o $@

# ── Lattice TUI toolkit (L0 surface, L1 compositor, L2 reactive) ─────────
# Freestanding core shared by the kernel and the host test harness, one
# folder per layer under src/libs/lattice (core/, l0/, l1/, l2/).
# Only the four core files build into the kernel (tests/ is host-only:
# it uses stdio/assert). Built with -fno-builtin like the vterm shims so
# gcc cannot synthesise libc calls we do not link.
LATTICE_DIR   = src/libs/lattice
LATTICE_SRCS  = $(LATTICE_DIR)/core/arena.c \
                $(LATTICE_DIR)/l0/surface.c \
                $(LATTICE_DIR)/l2/reactive.c \
                $(LATTICE_DIR)/l1/compositor.c
LATTICE_OBJS  = $(patsubst $(LATTICE_DIR)/%.c, build/lattice/%.o, $(LATTICE_SRCS))
LATTICE_INC   = -Isrc/libs -Isrc/libs/lattice
build/lattice/%.o: $(LATTICE_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $(LATTICE_INC) -c $< -o $@

# lattice_demo.c needs the lattice headers on its include path.
build/tui/lattice_demo.o: src/kernel/tui/lattice_demo.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-builtin $(LATTICE_INC) -c $< -o $@

# Host proof harness: builds the same core with the system gcc and runs it.
lattice-test:
	gcc -std=c11 -Wall -Wextra $(LATTICE_INC) \
	    $(LATTICE_SRCS) $(LATTICE_DIR)/tests/test_main.c -o /tmp/lattice_test
	/tmp/lattice_test
	@echo "[LATTICE] freestanding check (kernel flags, syntax+codegen only):"
	@for f in $(LATTICE_SRCS) src/kernel/tui/lattice_demo.c; do \
	    $(CC) $(CFLAGS) -fno-builtin $(LATTICE_INC) -Isrc/kernel -c $$f -o /tmp/lat_kcheck.o || exit 1; \
	    echo "  OK $$f"; \
	done

build/kernel.elf: $(KOBJS) $(LVTERM_OBJS) $(LATTICE_OBJS)
	$(LD) $(LDFLAGS) -o $@ $^

initramfs_root/bin/init: src/user/init.c
	@mkdir -p initramfs_root/bin
	gcc -ffreestanding -nostdlib -static -fno-stack-protector -m64 -O2 $< -o $@

initramfs_root/bin/crashme: src/user/crashme.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall -o $@ $<

initramfs_root/bin/fbtest: src/user/fbtest/main.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall -o $@ $<

initramfs_root/bin/shmtest: src/user/shmtest/main.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall -o $@ $<

initramfs_root/bin/sndtest: src/user/sndtest/main.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall -o $@ $< -lm

# ffitest: exercises the real libffi (vendored in third_party/) with the
# exact dispatch pattern libwayland uses — pointer + sint32 + uint32 +
# pointer args through ffi_prep_cif/ffi_call.
FFI_BUILD = third_party/libffi/x86_64-pc-linux-musl
initramfs_root/bin/ffitest: src/user/ffitest/main.c $(FFI_BUILD)/.libs/libffi.a
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall -I $(FFI_BUILD)/include -o $@ $< $(FFI_BUILD)/.libs/libffi.a

$(FFI_BUILD)/.libs/libffi.a:
	make -C third_party/libffi/x86_64-pc-linux-musl CC=/tmp/muslcc

# ── libwayland (vendored third_party/libwayland) ─────────────────────────
# The protocol headers are generated by wayland-scanner. It is a build-time
# tool, so the host's copy is used: no need to cross-build the scanner or
# its libxml2 dependency for a program that only ever emits C source.
WL_DIR  = third_party/libwayland
WL_SRCS = wayland-util connection wayland-protocol wayland-os \
          wayland-server wayland-client wayland-shm event-loop
WL_OBJS = $(patsubst %,$(WL_DIR)/build/%.o,$(WL_SRCS))
WL_INC  = -I$(WL_DIR) -I$(WL_DIR)/src -I$(FFI_BUILD)/include

$(WL_DIR)/src/wayland-server-protocol.h $(WL_DIR)/src/wayland-client-protocol.h $(WL_DIR)/src/wayland-protocol.c &: $(WL_DIR)/protocol/wayland.xml
	/usr/bin/wayland-scanner server-header $(WL_DIR)/protocol/wayland.xml $(WL_DIR)/src/wayland-server-protocol.h
	/usr/bin/wayland-scanner client-header $(WL_DIR)/protocol/wayland.xml $(WL_DIR)/src/wayland-client-protocol.h
	/usr/bin/wayland-scanner private-code  $(WL_DIR)/protocol/wayland.xml $(WL_DIR)/src/wayland-protocol.c

$(WL_DIR)/build/%.o: $(WL_DIR)/src/%.c $(WL_DIR)/src/wayland-server-protocol.h
	@mkdir -p $(WL_DIR)/build
	$(MUSL_GCC) -static -O2 -Wall $(WL_INC) -c $< -o $@

$(WL_DIR)/libwayland.a: $(WL_OBJS)
	ar cr $@ $(WL_OBJS)
	ranlib $@

initramfs_root/bin/wltest: src/user/wltest/main.c $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(WL_INC) -o $@ src/user/wltest/main.c \
	    $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a

# A real client: connects, binds the registry globals, and (next step) posts
# a surface with pixels in it.
initramfs_root/bin/wlclient: src/user/wlclient/main.c $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(WL_INC) -o $@ src/user/wlclient/main.c \
	    $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a

# The compositor: wl_compositor + wl_surface that blits shm buffers to /dev/fb0.
initramfs_root/bin/compositor: src/user/compositor/main.c $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(WL_INC) -o $@ src/user/compositor/main.c \
	    $(WL_DIR)/libwayland.a $(FFI_BUILD)/.libs/libffi.a

BASIS_INC = -Isrc/user/basis/include

initramfs_root/usr/basisd/sbin/basis-core: src/user/basis/core/main.c
	@mkdir -p initramfs_root/usr/basisd/sbin
	$(MUSL_GCC) -static -O2 -Wall $(BASIS_INC) -o $@ $<

initramfs_root/usr/basisd/sbin/basis-svcmgr: src/user/basis/svcmgr/main.c src/user/basis/svcmgr/load.c src/user/basis/svcmgr/dag.c src/user/basis/svcmgr/run.c src/user/basis/svcmgr/state.c src/user/basis/lib/toml.c
	@mkdir -p initramfs_root/usr/basisd/sbin
	$(MUSL_GCC) -static -O2 -Wall $(BASIS_INC) -o $@ $^

initramfs_root/usr/basisd/bin/basisctl: src/user/basis/ctl/main.c src/user/basis/ctl/common.c src/user/basis/ctl/status.c src/user/basis/ctl/mutate.c src/user/basis/ctl/validate.c src/user/basis/lib/toml.c src/user/basis/lib/state.c
	@mkdir -p initramfs_root/usr/basisd/bin
	$(MUSL_GCC) -static -O2 -Wall $(BASIS_INC) -o $@ $^

initramfs_root/usr/basisd/bin/basis-log: src/user/basis/log/main.c
	@mkdir -p initramfs_root/usr/basisd/bin
	$(MUSL_GCC) -static -O2 -Wall $(BASIS_INC) -o $@ $<

initramfs_root/usr/basisd/bin/basis-ntpsync: src/user/basis/ntp/main.c
	@mkdir -p initramfs_root/usr/basisd/bin
	$(MUSL_GCC) -static -O2 -Wall $(BASIS_INC) -o $@ $<

# ── lattice CLI (whiptail-style userspace tool) ──────────────────────────
# Static musl binary using the same L0/L1/L2 core as the kernel demo.
LATTICE_CLI_SRCS = src/libs/lattice/cli/main.c \
                   src/libs/lattice/cli/term.c \
                   src/libs/lattice/cli/dialogs.c
LATTICE_CLI_INC  = -Isrc/libs -Isrc/libs/lattice -Isrc/libs/lattice/cli
initramfs_root/bin/lattice: $(LATTICE_CLI_SRCS) $(LATTICE_SRCS)
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(LATTICE_CLI_INC) -o $@ $^

# Host build of the CLI for piped-input testing (same sources, system gcc).
lattice-cli-test: lattice-test
	gcc -std=c11 -Wall -Wextra $(LATTICE_CLI_INC) \
	    $(LATTICE_CLI_SRCS) $(LATTICE_SRCS) -o /tmp/lattice-cli
	@echo "[LATTICE] host CLI checks:"
	@printf '\n' | /tmp/lattice-cli --menu "Pick one" 12 40 3 \
	    alpha "First item" beta "Second item" bravo "Third" \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  menu-enter rc=$$? result=$$(cat /tmp/cli_res.txt)"
	@printf '\033[B\n' | /tmp/lattice-cli --menu "Pick one" 12 40 3 \
	    alpha "First item" beta "Second item" bravo "Third" \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  menu-down-enter rc=$$? result=$$(cat /tmp/cli_res.txt)"
	@printf '\033' | /tmp/lattice-cli --menu "Pick one" 12 40 3 \
	    alpha "First item" beta "Second item" \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  menu-esc rc=$$? result=$$(cat /tmp/cli_res.txt)"
	@printf '\n' | /tmp/lattice-cli --yesno "Reboot now?" 8 40 \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  yesno-enter rc=$$?"
	@printf 'n' | /tmp/lattice-cli --yesno "Reboot now?" 8 40 \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  yesno-n rc=$$?"
	@printf 'hello\n' | /tmp/lattice-cli --inputbox "Name:" 9 40 "jo" \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  inputbox rc=$$? result=$$(cat /tmp/cli_res.txt)"
	@printf '\n' | /tmp/lattice-cli --msgbox "Hello world" 8 40 \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  msgbox rc=$$?"
	@printf 'q' | /tmp/lattice-cli --textbox Makefile 16 60 \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  textbox rc=$$?"
	@printf '10\n50\nXXX\nHalfway\nXXX\n100\n' | \
	    /tmp/lattice-cli --gauge "Working..." 8 50 0 \
	    >/tmp/cli_ui.txt 2>/tmp/cli_res.txt; \
	    echo "  gauge-pipe rc=$$?"
	@echo "  ui bytes: $$(wc -c < /tmp/cli_ui.txt), alt-screen: $$(grep -c '1049h' /tmp/cli_ui.txt || true)"

# ── eigen-recovery (diagnostic console) ────────────────────────────────
# Static musl binary on the lattice L0/L1/L2 core + shared CLI terminal
# backend. onfail is a shell wrapper, packed via the find rule.
RECOVERY_SRCS = src/user/recovery/main.c \
                src/user/recovery/data/data.c \
                src/user/recovery/ui/ui.c \
                src/user/recovery/screens/home.c \
                src/user/recovery/screens/log.c \
                src/user/recovery/screens/regs.c \
                src/user/recovery/screens/exceptions.c \
                src/user/recovery/screens/storage.c \
                src/user/recovery/screens/pci.c \
                src/user/recovery/screens/hex.c \
                src/user/recovery/screens/shellview.c \
                src/user/recovery/overlays/overlays.c \
                src/user/recovery/actions/actions.c \
                src/user/recovery/shell/shell.c
RECOVERY_INC  = -Isrc/libs -Isrc/libs/lattice -Isrc/libs/lattice/cli -Isrc/user/recovery
initramfs_root/bin/eigen-recovery: $(RECOVERY_SRCS) $(LATTICE_SRCS) src/libs/lattice/cli/term.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(RECOVERY_INC) -o $@ $^

# Host build + scripted flows (piped keys; EOF ends the app, rc must be 0).
# NOTE: cells go out cursor-addressed, so text asserts strip CSI first.
recovery-test:
	gcc -std=c11 -Wall -Wextra $(RECOVERY_INC) \
	    $(RECOVERY_SRCS) src/libs/lattice/cli/term.c $(LATTICE_SRCS) \
	    -o /tmp/eigen-recovery
	@echo "[RECOVERY] scripted flows:"
	@printf '2\0331\033' | /tmp/eigen-recovery --reason "test degraded" \
	    >/tmp/rec_ui.txt 2>&1; echo "  nav-log-home-quit rc=$$?"
	@sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/rec_ui.txt > /tmp/rec_txt.txt
	@grep -c "REGISTERS" /tmp/rec_txt.txt | xargs echo "  regs frames:"
	@grep -c "DEGRADED" /tmp/rec_txt.txt | xargs echo "  health frames:"
	@printf '7hi\n\0331\033' | COLUMNS=100 LINES=30 /tmp/eigen-recovery \
	    >/tmp/rec_ui2.txt 2>&1; echo "  shell-type-quit rc=$$?"
	@printf '/stor\n\033' | /tmp/eigen-recovery "panic: test" \
	    >/tmp/rec_ui3.txt 2>&1; echo "  palette-storage-quit rc=$$?"
	@sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/rec_ui3.txt > /tmp/rec_txt3.txt
	@grep -c "CRITICAL" /tmp/rec_txt3.txt | xargs echo "  critical frames:"
	@grep -c "STORAGE" /tmp/rec_txt3.txt | xargs echo "  palette-land frames:"
	@printf '6g0x100000\n\033a\0331\033' | /tmp/eigen-recovery \
	    >/tmp/rec_ui4.txt 2>&1; echo "  hex-goto-annotate-quit rc=$$?"
	@sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/rec_ui4.txt > /tmp/rec_txt4.txt
	@grep -c "MEM @" /tmp/rec_txt4.txt | xargs echo "  hex frames:"

# ── basisui (Basis service console) ────────────────────────────────────
# Static musl binary on the lattice L0/L1/L2 core + shared CLI terminal
# backend. Shells out to basisctl for mutations.
BASISUI_SRCS = src/user/basis/ui/main.c \
               src/user/basis/ui/data.c \
               src/user/basis/ui/ui.c
BASISUI_INC  = -Isrc/libs -Isrc/libs/lattice -Isrc/libs/lattice/cli -Isrc/user/basis/ui
initramfs_root/bin/basisui: $(BASISUI_SRCS) $(LATTICE_SRCS) src/libs/lattice/cli/term.c
	@mkdir -p initramfs_root/bin
	$(MUSL_GCC) -static -O2 -Wall $(BASISUI_INC) -o $@ $^

# Host build + fixture flows (fake /tmp/basis tree; EOF ends app, rc 0).
basisui-test:
	gcc -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE $(BASISUI_INC) \
	    $(BASISUI_SRCS) src/libs/lattice/cli/term.c $(LATTICE_SRCS) \
	    -o /tmp/basisui
	@mkdir -p /tmp/basis/log
	@printf 'SVC multi-user/motd online 100 gen=3 exit=0\nSVC network/dhcp offline 90 gen=1 exit=1 link timeout\nSVC system/identity online 95 gen=2 exit=0\n' > /tmp/basis/state
	@printf 'motd line one\nmotd line two\n' > /tmp/basis/log/multi-user_motd.log
	@echo "[BASISUI] fixture flows:"
	@printf 'l\033\033' | /tmp/basisui >/tmp/bui.txt 2>&1; echo "  open-log-quit rc=$$?"
	@sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' /tmp/bui.txt > /tmp/bui_txt.txt
	@grep -c "multi-user/motd" /tmp/bui_txt.txt | xargs echo "  table frames:"
	@grep -c "motd line one" /tmp/bui_txt.txt | xargs echo "  log frames:"
	@printf 'v\033\033' | PATH="/tmp/empty-bin:$$PATH" /tmp/basisui >/tmp/bui2.txt 2>&1; echo "  validate-quit rc=$$?"
	@printf '?\033' | /tmp/basisui >/tmp/bui3.txt 2>&1; echo "  help-quit rc=$$?"

# ── fastfetch (system info; static musl, all optional deps off) ────────
# Built once via cmake into tools/fastfetch-2.43.0/build-eigen; the binary
# is copied into the initramfs. Re-run the cmake configure in that dir if
# you change options (see FF_FLAGS below).
FF_DIR   = tools/fastfetch-2.43.0
FF_BUILD = $(FF_DIR)/build-eigen
FF_FLAGS = -DCMAKE_C_COMPILER=$(abspath $(MUSL_GCC)) \
           -DCMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES="$(abspath toolchain/musl/include);/usr/lib/gcc/x86_64-pc-linux-gnu/15/include" \
           -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF \
           -DENABLE_VULKAN=OFF -DENABLE_WAYLAND=OFF -DENABLE_XCB_RANDR=OFF \
           -DENABLE_XRANDR=OFF -DENABLE_DRM=OFF -DENABLE_DRM_AMDGPU=OFF \
           -DENABLE_GIO=OFF -DENABLE_DCONF=OFF -DENABLE_DBUS=OFF \
           -DENABLE_XFCONF=OFF -DENABLE_SQLITE3=OFF -DENABLE_RPM=OFF \
           -DENABLE_IMAGEMAGICK7=OFF -DENABLE_IMAGEMAGICK6=OFF \
           -DENABLE_CHAFA=OFF -DENABLE_ZLIB=OFF -DENABLE_EGL=OFF \
           -DENABLE_GLX=OFF -DENABLE_OPENCL=OFF -DENABLE_ELF=OFF \
           -DENABLE_PULSE=OFF -DCMAKE_C_FLAGS="-static" \
           -DCMAKE_EXE_LINKER_FLAGS="-static"
initramfs_root/bin/fastfetch: $(FF_BUILD)/fastfetch
	@mkdir -p initramfs_root/bin
	@cp $< $@
$(FF_BUILD)/fastfetch:
	cmake --build $(FF_BUILD) -j$(shell nproc) || \
	    (cmake -S $(FF_DIR) -B $(FF_BUILD) $(FF_FLAGS) && \
	     cmake --build $(FF_BUILD) -j$(shell nproc))

iso_root/boot/initramfs.tar: initramfs_root/bin/init initramfs_root/bin/crashme initramfs_root/bin/fbtest initramfs_root/bin/shmtest initramfs_root/bin/sndtest initramfs_root/bin/ffitest initramfs_root/bin/wltest initramfs_root/bin/compositor initramfs_root/bin/wlclient initramfs_root/usr/basisd/sbin/basis-core initramfs_root/usr/basisd/sbin/basis-svcmgr initramfs_root/usr/basisd/bin/basisctl initramfs_root/usr/basisd/bin/basis-log initramfs_root/usr/basisd/bin/basis-ntpsync initramfs_root/bin/lattice initramfs_root/bin/eigen-recovery initramfs_root/bin/basisui initramfs_root/bin/fastfetch $(shell find initramfs_root -type f 2>/dev/null)
	@mkdir -p iso_root/boot
	tar --format=ustar -cf $@ -C initramfs_root .

eigen.iso: build/kernel.elf iso_root/boot/initramfs.tar
	@cp build/kernel.elf iso_root/boot/kernel.elf
	@xorriso -as mkisofs -R -r -J \
	    -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot \
	    -boot-load-size 4 \
	    -boot-info-table \
	    -hfsplus \
	    -apm-block-size 2048 \
	    --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part \
	    --efi-boot-image \
	    --protective-msdos-label \
	    iso_root -o $@
	@limine bios-install $@
	@echo "[BUILD] eigen.iso ready."

run: eigen.iso stop
	qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
	    -cdrom eigen.iso -netdev user,id=n0 -device e1000,netdev=n0 \
	    -display gtk,zoom-to-fit=on \
	    -serial stdio -no-reboot -no-shutdown

run-uefi: eigen.iso stop
	@cp /usr/share/edk2/OvmfX64/OVMF_VARS.fd /tmp/OVMF_VARS.fd
	qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
	    -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/OvmfX64/OVMF_CODE.fd \
	    -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
    -cdrom eigen.iso -netdev user,id=n0 -device e1000,netdev=n0 \
    -display gtk,zoom-to-fit=on \
    -serial stdio -no-reboot -no-shutdown

# Headful via VNC: framebuffer + guest keyboard/mouse in a viewer window,
# serial stays on the launching tty (host Ctrl-C never reaches QEMU).
run-vnc: eigen.iso stop
	(sleep 1; vncviewer :5 &) ; \
	qemu-system-x86_64 -enable-kvm -cpu host -smp 1 -m 2G \
	    -cdrom eigen.iso -netdev user,id=n0 -device e1000,netdev=n0 \
	    -serial stdio -display none -vnc :5 -no-reboot -no-shutdown

clean:
	rm -rf build/ eigen.iso iso_root/boot/kernel.elf

# Stop stale QEMU instances (by PID) so a fresh run never hits a busy port.
stop:
	@for pid in $$(pidof qemu-system-x86_64 2>/dev/null || true); do \
	    echo "[QEMU] Stopping stale instance $$pid"; \
	    kill $$pid 2>/dev/null || true; \
	done; \
	sleep 1; \
	for pid in $$(pidof qemu-system-x86_64 2>/dev/null || true); do \
	    kill -9 $$pid 2>/dev/null || true; \
	done; true

-include $(shell find build -name '*.d' 2>/dev/null)

.PHONY: all run run-uefi run-vnc stop clean lattice-test lattice-cli-test recovery-test basisui-test
