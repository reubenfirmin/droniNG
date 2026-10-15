/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PIOS_VIDEO_SYNC_GUARD_H
#define PIOS_VIDEO_SYNC_GUARD_H
#include <stdbool.h>
#include <stdint.h>

/* PAL/NTSC fields are at least 16 ms apart. Bound noise interrupts and retry
 * after a cooling period from the OSD task, even when no redraw is signalled. */
struct pios_video_sync_guard {
    volatile uint32_t last_field, masked_at;
    volatile uint8_t spurious;
    volatile bool masked;
};
static inline bool PIOS_Video_SyncAccept(struct pios_video_sync_guard *guard, uint32_t now)
{
    if (guard->masked) return false;
    if ((uint32_t)(now - guard->last_field) < 10000) {
        if (++guard->spurious >= 10) { guard->masked_at = now; guard->masked = true; }
        return false;
    }
    guard->last_field = now; guard->spurious = 0;
    return true;
}
static inline bool PIOS_Video_SyncRecover(struct pios_video_sync_guard *guard, uint32_t now)
{
    if (!guard->masked || (uint32_t)(now - guard->masked_at) < 400000) return false;
    guard->spurious = 0; guard->last_field = now - 10000; guard->masked = false;
    return true;
}
#endif
