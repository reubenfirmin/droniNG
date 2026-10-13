/* SPDX-License-Identifier: GPL-3.0-or-later */
#if defined(IMPORTED_PROFILE_RADIX)
#include "../../radix/board-info/board_hw_defs.c"
#elif defined(IMPORTED_PROFILE_OMNIBUSF4SD)
#include "../../omnibusf4sd/board-info/board_hw_defs.c"
#else
#error No reviewed imported hardware profile
#endif
