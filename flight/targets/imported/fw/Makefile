# SPDX-License-Identifier: GPL-3.0-or-later
# Shared native F4 build with explicitly reviewed compile-time profiles.
HOST_PYTHON ?= python3
PAD_TLFW_FW_DESC := YES
SRC += pios_board_info.c
CDEFS += -DBOARD_TYPE=$(BOARD_TYPE) -DBOARD_REVISION=$(BOARD_REVISION) -DHW_TYPE=$(HW_TYPE)
CDEFS += -DBOOTLOADER_VERSION=$(BOOTLOADER_VERSION) -DFW_BANK_BASE=$(FW_BANK_BASE)
CDEFS += -DFW_BANK_SIZE=$(FW_BANK_SIZE) -DFW_DESC_SIZE=$(FW_DESC_SIZE)
CDEFS += -DEE_BANK_BASE=$(EE_BANK_BASE) -DEE_BANK_SIZE=$(EE_BANK_SIZE)
EXTRAINCDIRS += $(OUTDIR) $(ROOT_DIR)/flight/targets/imported/fw
vpath pios_board.c $(ROOT_DIR)/flight/targets/imported/fw
IMPORTED_MEMORY_SCRIPT = $(OUTDIR)/imported_memory.ld
IMPORTED_LINKER_SCRIPT = $(ROOT_DIR)/flight/targets/imported/fw/sections.ld
ifeq ($(IMPORTED_PROFILE),radix)
CDEFS += -DIMPORTED_PROFILE_RADIX
include $(ROOT_DIR)/flight/targets/brainre1/fw/Makefile
PROFILE_SOURCE := brainfpv
else ifeq ($(IMPORTED_PROFILE),omnibusf4sd)
CDEFS += -DIMPORTED_PROFILE_OMNIBUSF4SD
include $(ROOT_DIR)/flight/targets/revolution/fw/Makefile
PROFILE_SOURCE := betaflight_3_4
else
$(error No reviewed imported board backend)
endif

# ROM DFU can leave address zero mapped to system memory. Route interrupts to
# the physical flash vectors for both a ROM jump and a normal power-on reset.
CDEFS += -UCORTEX_VTOR_INIT '-DCORTEX_VTOR_INIT=$(FW_BANK_BASE)'
$(OUTDIR)/chcore_v7m.o: $(ROOT_DIR)/flight/targets/imported/fw/Makefile

PROFILE_INPUTS := $(ROOT_DIR)/shared/firmware/profiles/$(IMPORTED_PROFILE).json
PROFILE_INPUTS += $(ROOT_DIR)/make/scripts/import-board.py
PROFILE_INPUTS += $(ROOT_DIR)/make/scripts/radix-dry-run.py $(ROOT_DIR)/make/scripts/betaflight-desk-check.py
PROFILE_INPUTS += $(ROOT_DIR)/make/scripts/omnibusf4sd-import.py
PROFILE_INPUTS += $(ROOT_DIR)/flight/Betaflight/$(IMPORTED_PROFILE).lock.json $(ROOT_DIR)/flight/Betaflight/$(IMPORTED_PROFILE)-source-manifest.json
PROFILE_INPUTS += $(shell find $(ROOT_DIR)/flight/Betaflight/upstream/$(PROFILE_SOURCE) -type f)
$(OUTDIR)/imported_profile.h $(IMPORTED_MEMORY_SCRIPT) $(OUTDIR)/import-report.json &: $(PROFILE_INPUTS)
	$(HOST_PYTHON) $(ROOT_DIR)/make/scripts/import-board.py --profile $(IMPORTED_PROFILE) --output $(OUTDIR)
$(ALLOBJ) $(MODOBJ): $(OUTDIR)/imported_profile.h
$(OUTDIR)/$(TARGET).elf: $(OUTDIR)/imported_profile.h $(IMPORTED_MEMORY_SCRIPT) $(IMPORTED_LINKER_SCRIPT)
