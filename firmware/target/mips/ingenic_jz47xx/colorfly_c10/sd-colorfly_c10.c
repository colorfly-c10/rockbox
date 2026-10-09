/***************************************************************************
 * Colorfly C10 internal-storage write policy
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "ata-sd-target.h"
#include "disk.h"
#include "string.h"

static unsigned char c10_emmc_mbr[512] CACHEALIGN_ATTR;
static sector_t c10_emmc_part4_start;
static sector_t c10_emmc_part4_end;

/* eMMC p1-p3 contain the stock boot chain and system. Cache a validated p4
 * interval during card initialization and reject every write outside it.
 * Both filesystem and USB writes reach the physical-sector API, so the USB
 * LUN offset alone cannot protect the stock areas. */
int c10_sd_validate_card(int drive, sector_t blocks,
                        int (*read_mbr)(int, unsigned char *))
{
    if (drive != 0)
        return 0;
    c10_emmc_part4_start = 0;
    c10_emmc_part4_end = 0;
    memset(c10_emmc_mbr, 0, sizeof(c10_emmc_mbr));
    int retval = read_mbr(drive, c10_emmc_mbr);
    if (retval)
        return retval;

    const unsigned int entry = 446 + 3 * 16;
    const unsigned int type = c10_emmc_mbr[entry + 4];
    const sector_t start =
        (sector_t)c10_emmc_mbr[entry + 8] |
        ((sector_t)c10_emmc_mbr[entry + 9] << 8) |
        ((sector_t)c10_emmc_mbr[entry + 10] << 16) |
        ((sector_t)c10_emmc_mbr[entry + 11] << 24);
    const sector_t size =
        (sector_t)c10_emmc_mbr[entry + 12] |
        ((sector_t)c10_emmc_mbr[entry + 13] << 8) |
        ((sector_t)c10_emmc_mbr[entry + 14] << 16) |
        ((sector_t)c10_emmc_mbr[entry + 15] << 24);

    if (c10_emmc_mbr[510] != 0x55 || c10_emmc_mbr[511] != 0xaa ||
        (type != PARTITION_TYPE_FAT32 &&
         type != PARTITION_TYPE_FAT32_LBA) ||
        start == 0 || size == 0 || start >= blocks ||
        size > blocks - start)
        return -1;

    c10_emmc_part4_start = start;
    c10_emmc_part4_end = start + size;
    return 0;
}

bool c10_sd_write_allowed(int drive, sector_t start, int count)
{
    return drive != 0 ||
        (count > 0 && c10_emmc_part4_start != 0 &&
         start >= c10_emmc_part4_start && start < c10_emmc_part4_end &&
         (sector_t)count <= c10_emmc_part4_end - start);
}
