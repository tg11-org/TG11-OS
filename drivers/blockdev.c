#include "blockdev.h"
#include "ata.h"

/* ------------------------------------------------------------------ */
/* Static wrappers: each drive gets its own read/write function pair   */
/* so the block_device function pointers have no extra state.          */
/* ------------------------------------------------------------------ */

static int drive0_read(unsigned int lba, unsigned char *buf)
{
	return ata_read_sector28_drive(0, lba, buf);
}
static int drive0_write(unsigned int lba, const unsigned char *buf)
{
	return ata_write_sector28_drive(0, lba, buf);
}
static int drive1_read(unsigned int lba, unsigned char *buf)
{
	return ata_read_sector28_drive(1, lba, buf);
}
static int drive1_write(unsigned int lba, const unsigned char *buf)
{
	return ata_write_sector28_drive(1, lba, buf);
}

static struct block_device drives[2];

void blockdev_init(void)
{
	int i;
	for (i = 0; i < 2; i++)
	{
		drives[i].present = 0;
		drives[i].sector_count = 0;
		drives[i].read_sector = (void *)0;
		drives[i].write_sector = (void *)0;
	}

	/* Probe master first (does the channel reset), then slave */
	if (ata_init_drive(0) == 0 && ata_is_present_drive(0))
	{
		drives[0].present = 1;
		drives[0].sector_count = ata_get_sector_count_drive(0);
		drives[0].read_sector = drive0_read;
		drives[0].write_sector = drive0_write;
	}

	if (ata_init_drive(1) == 0 && ata_is_present_drive(1))
	{
		drives[1].present = 1;
		drives[1].sector_count = ata_get_sector_count_drive(1);
		drives[1].read_sector = drive1_read;
		drives[1].write_sector = drive1_write;
	}
}

int blockdev_count(void)
{
	int n = 0;
	if (drives[0].present) n++;
	if (drives[1].present) n++;
	return n;
}

struct block_device *blockdev_get(int index)
{
	if (index < 0 || index > 1) return (void *)0;
	return &drives[index];
}

struct block_device *blockdev_get_primary(void)
{
	return &drives[0];
}

struct block_device *blockdev_get_secondary(void)
{
	return &drives[1];
}
