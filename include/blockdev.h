#ifndef TG11_BLOCKDEV_H
#define TG11_BLOCKDEV_H

struct block_device
{
	int present;
	unsigned int sector_count;
	int (*read_sector)(unsigned int lba, unsigned char *buffer512);
	int (*write_sector)(unsigned int lba, const unsigned char *buffer512);
};

void blockdev_init(void);
struct block_device *blockdev_get_primary(void);

#endif
