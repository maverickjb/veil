#include "board.h"
#include "veil.h"

/*
 * HDMI/mailbox FB is left for bring-up on hardware. uart10 is the console.
 * BOARD_MBOX_BASE is in board.h when you port the raspi3 mailbox path.
 */

int board_skip_testdisk(void)
{
    return 0;
}

int fb_init(void)
{
    return -1;
}

void fb_putc(char c)
{
    (void)c;
}

int fb_map_guest_ramfb(const uint8_t *be_cfg, uint32_t len)
{
    (void)be_cfg;
    (void)len;
    return -1;
}
