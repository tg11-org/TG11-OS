#include "blockdev.h"
#include "ata.h"

static struct block_device primary;

void blockdev_init(void)
{
	if (ata_init() == 0 && ata_is_present())
	{
		primary.present = 1;
		primary.sector_count = ata_get_sector_count_low();
		primary.read_sector = ata_read_sector28;
		primary.write_sector = ata_write_sector28;
	}
	else
	{
		primary.present = 0;
		primary.sector_count = 0;
		primary.read_sector = (void *)0;
		primary.write_sector = (void *)0;
	}
}

struct block_device *blockdev_get_primary(void)
{
	return &primary;
}
