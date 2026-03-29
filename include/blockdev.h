#ifndef TG11_BLOCKDEV_H
#define TG11_BLOCKDEV_H

#define BLOCKDEV_MAX_DRIVES 4

struct block_device
{
	int present;
	unsigned int sector_count;
	int (*read_sector)(unsigned int lba, unsigned char *buffer512);
	int (*write_sector)(unsigned int lba, const unsigned char *buffer512);
	int (*read_sectors)(unsigned int lba, int count, unsigned char *buffer);
	int (*write_sectors)(unsigned int lba, int count, const unsigned char *buffer);
};

void blockdev_init(void);
int  blockdev_count(void);               /* how many drives found (0-4) */
struct block_device *blockdev_get(int index); /* 0..3 ATA drive index */
struct block_device *blockdev_get_primary(void);   /* alias for get(0) */
struct block_device *blockdev_get_secondary(void); /* alias for get(1) */

#endif
