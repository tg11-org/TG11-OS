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
static int drive0_read_sectors(unsigned int lba, int count, unsigned char *buf)
{
	return ata_read_sectors_drive(0, lba, count, buf);
}
static int drive0_write_sectors(unsigned int lba, int count, const unsigned char *buf)
{
	return ata_write_sectors_drive(0, lba, count, buf);
}
static int drive1_read(unsigned int lba, unsigned char *buf)
{
	return ata_read_sector28_drive(1, lba, buf);
}
static int drive1_write(unsigned int lba, const unsigned char *buf)
{
	return ata_write_sector28_drive(1, lba, buf);
}
static int drive1_read_sectors(unsigned int lba, int count, unsigned char *buf)
{
	return ata_read_sectors_drive(1, lba, count, buf);
}
static int drive1_write_sectors(unsigned int lba, int count, const unsigned char *buf)
{
	return ata_write_sectors_drive(1, lba, count, buf);
}
static int drive2_read(unsigned int lba, unsigned char *buf)
{
	return ata_read_sector28_drive(2, lba, buf);
}
static int drive2_write(unsigned int lba, const unsigned char *buf)
{
	return ata_write_sector28_drive(2, lba, buf);
}
static int drive2_read_sectors(unsigned int lba, int count, unsigned char *buf)
{
	return ata_read_sectors_drive(2, lba, count, buf);
}
static int drive2_write_sectors(unsigned int lba, int count, const unsigned char *buf)
{
	return ata_write_sectors_drive(2, lba, count, buf);
}
static int drive3_read(unsigned int lba, unsigned char *buf)
{
	return ata_read_sector28_drive(3, lba, buf);
}
static int drive3_write(unsigned int lba, const unsigned char *buf)
{
	return ata_write_sector28_drive(3, lba, buf);
}
static int drive3_read_sectors(unsigned int lba, int count, unsigned char *buf)
{
	return ata_read_sectors_drive(3, lba, count, buf);
}
static int drive3_write_sectors(unsigned int lba, int count, const unsigned char *buf)
{
	return ata_write_sectors_drive(3, lba, count, buf);
}

static struct block_device drives[BLOCKDEV_MAX_DRIVES];

static int (*const drive_read_wrappers[BLOCKDEV_MAX_DRIVES])(unsigned int, unsigned char *) = {
	drive0_read, drive1_read, drive2_read, drive3_read
};
static int (*const drive_write_wrappers[BLOCKDEV_MAX_DRIVES])(unsigned int, const unsigned char *) = {
	drive0_write, drive1_write, drive2_write, drive3_write
};
static int (*const drive_read_sectors_wrappers[BLOCKDEV_MAX_DRIVES])(unsigned int, int, unsigned char *) = {
	drive0_read_sectors, drive1_read_sectors, drive2_read_sectors, drive3_read_sectors
};
static int (*const drive_write_sectors_wrappers[BLOCKDEV_MAX_DRIVES])(unsigned int, int, const unsigned char *) = {
	drive0_write_sectors, drive1_write_sectors, drive2_write_sectors, drive3_write_sectors
};

void blockdev_init(void)
{
	int i;
	for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
	{
		drives[i].present = 0;
		drives[i].sector_count = 0;
		drives[i].read_sector = (void *)0;
		drives[i].write_sector = (void *)0;
		drives[i].read_sectors = (void *)0;
		drives[i].write_sectors = (void *)0;
	}

	/* Probe masters first (channel reset), then slaves on each channel. */
	for (i = 0; i < BLOCKDEV_MAX_DRIVES; i += 2)
	{
		if (ata_init_drive(i) == 0 && ata_is_present_drive(i))
		{
			drives[i].present = 1;
			drives[i].sector_count = ata_get_sector_count_drive(i);
			drives[i].read_sector = drive_read_wrappers[i];
			drives[i].write_sector = drive_write_wrappers[i];
			drives[i].read_sectors = drive_read_sectors_wrappers[i];
			drives[i].write_sectors = drive_write_sectors_wrappers[i];
		}
	}

	for (i = 1; i < BLOCKDEV_MAX_DRIVES; i += 2)
	{
		if (ata_init_drive(i) == 0 && ata_is_present_drive(i))
		{
			drives[i].present = 1;
			drives[i].sector_count = ata_get_sector_count_drive(i);
			drives[i].read_sector = drive_read_wrappers[i];
			drives[i].write_sector = drive_write_wrappers[i];
			drives[i].read_sectors = drive_read_sectors_wrappers[i];
			drives[i].write_sectors = drive_write_sectors_wrappers[i];
		}
	}
}

int blockdev_count(void)
{
	int n = 0;
	int i;
	for (i = 0; i < BLOCKDEV_MAX_DRIVES; i++)
	{
		if (drives[i].present) n++;
	}
	return n;
}

struct block_device *blockdev_get(int index)
{
	if (index < 0 || index >= BLOCKDEV_MAX_DRIVES) return (void *)0;
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
