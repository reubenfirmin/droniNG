/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared F446 PiOS limits/COM declarations, without RE1 hardware initialization. */
#include "../../brainre1/board-info/pios_board.h"
#include "imported_profile.h"
#undef VREF_PLUS
#define VREF_PLUS IMPORTED_ADC_VREF
