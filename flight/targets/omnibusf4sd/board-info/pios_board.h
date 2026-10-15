/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../../revolution/board-info/pios_board.h"
#include "imported_profile.h"
#undef VREF_PLUS
#define VREF_PLUS IMPORTED_ADC_VREF
