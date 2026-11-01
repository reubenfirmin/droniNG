# SPDX-License-Identifier: GPL-3.0-or-later
PAD_TLFW_FW_DESC := YES
SRC := $(filter-out pios_usb_board_data.c pios_usbhook.c pios_hmc5983_i2c.c pios_usb_hid_istr.c %/pios_usb_hid_istr.c $(addprefix $(USBFSLIB)/src/,$(USBFSLIB_SRC)),$(SRC))
SRC += pios_board_info.c pios_ms5611.c
CDEFS += -DBOARD_TYPE=$(BOARD_TYPE) -DBOARD_REVISION=$(BOARD_REVISION) -DHW_TYPE=$(HW_TYPE)
CDEFS += -DBOOTLOADER_VERSION=0 -DFW_BANK_BASE=$(FW_BANK_BASE) -DFW_BANK_SIZE=$(FW_BANK_SIZE) -DFW_DESC_SIZE=$(FW_DESC_SIZE)
CDEFS += -DCORTEX_USE_FPU=TRUE -UCORTEX_VTOR_INIT -DCORTEX_VTOR_INIT=0x08000000
LDFLAGS := $(filter-out -T% memory.ld,$(LDFLAGS))
LDFLAGS += -T $(OUTDIR)/memory.ld -T $(OUTDIR)/sections.ld
