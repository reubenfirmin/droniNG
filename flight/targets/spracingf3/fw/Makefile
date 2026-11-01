# SPDX-License-Identifier: GPL-3.0-or-later
# Reuse the F303 build machinery with a separate original-board backend.
IMPORTED_F3_CONFIG := $(ROOT_DIR)/flight/targets/spracingf3/fw/native.mk
EXTRAINCDIRS += $(OUTDIR)
include $(ROOT_DIR)/flight/targets/sprf3e/fw/Makefile
PROFILE_INPUTS := $(ROOT_DIR)/make/scripts/spracingf3-import.py
PROFILE_INPUTS += $(ROOT_DIR)/make/scripts/spracingf3-dry-run.py $(ROOT_DIR)/make/scripts/import-board.py
PROFILE_INPUTS += $(ROOT_DIR)/shared/firmware/profiles/spracingf3.json
PROFILE_INPUTS += $(ROOT_DIR)/flight/Betaflight/spracingf3.lock.json $(ROOT_DIR)/flight/Betaflight/spracingf3-source-manifest.json
PROFILE_INPUTS += $(shell find $(ROOT_DIR)/flight/Betaflight/upstream/betaflight_3_1_5 -type f)
$(OUTDIR)/imported_profile.h $(OUTDIR)/memory.ld $(OUTDIR)/sections.ld $(OUTDIR)/import-report.json &: $(PROFILE_INPUTS) $(ROOT_DIR)/flight/PiOS/STM32F30x/sections_chibios.ld
	python3 $(ROOT_DIR)/make/scripts/import-board.py --profile spracingf3 --output $(OUTDIR)
$(ALLOBJ) $(MODOBJ): $(OUTDIR)/imported_profile.h
$(OUTDIR)/$(TARGET).elf: $(OUTDIR)/memory.ld $(OUTDIR)/sections.ld
