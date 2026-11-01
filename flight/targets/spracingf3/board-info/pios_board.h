/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Common F303 limits and COM declarations; no Evo hardware tables. */
#include "../../sprf3e/board-info/pios_board.h"
#include "imported_profile.h"
#undef PIOS_USB_ENABLED
#define PIOS_USB_ENABLED 0
#undef PIOS_COM_TELEM_USB
#undef PIOS_COM_VCP
#define PIOS_COM_VCP 0
