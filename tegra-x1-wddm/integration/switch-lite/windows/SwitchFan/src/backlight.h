/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SWITCH_BACKLIGHT_H
#define SWITCH_BACKLIGHT_H

#define BACKLIGHT_MIN 8u
#define BACKLIGHT_MAX 255u

static ULONG SwBacklightClamp(ULONG Level)
{
    return Level < BACKLIGHT_MIN ? BACKLIGHT_MIN :
           Level > BACKLIGHT_MAX ? BACKLIGHT_MAX : Level;
}

static ULONG SwBacklightDuty(ULONG Level, ULONG DisplayState)
{
    if (DisplayState == 0) return 0;
    if (DisplayState == 2) return SwBacklightClamp(Level / 3);
    return SwBacklightClamp(Level);
}

/* Keep the clock divider from the firmware; only enable/duty change. */
static ULONG SwBacklightCsr(ULONG Current, ULONG Duty)
{
    return (Current & ~0x7FFF0000u) | 0x80000000u | (Duty << 16);
}

#endif
