/*
 * Output-rate definitions derived from pios_servo.h.
 * Copyright (C) 2010 The OpenPilot Team, http://www.openpilot.org
 * Copyright (C) 2015 Tau Labs, http://taulabs.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#ifndef PIOS_SERVO_RATE_H
#define PIOS_SERVO_RATE_H
#include <stdbool.h>
#include <stdint.h>

enum pios_servo_shot_type {
    SHOT_ONESHOT = 0,
    SHOT_DSHOT300 = 65532,
    SHOT_DSHOT600 = 65533,
    SHOT_DSHOT1200 = 65534
};

static inline bool PIOS_Servo_OutputRatesSupported(const uint16_t *rates, int banks)
{
#if defined(PIOS_SERVO_DISABLE_DSHOT)
    for (int bank = 0; bank < banks; ++bank)
        if (rates[bank] >= SHOT_DSHOT300 && rates[bank] <= SHOT_DSHOT1200)
            return false;
#else
    (void)rates; (void)banks;
#endif
    return true;
}
#endif
