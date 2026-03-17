#ifndef TG11_ATA_H
#define TG11_ATA_H

int ata_init(void);
int ata_is_present(void);
unsigned int ata_get_sector_count_low(void);

int ata_read_sector28(unsigned int lba, unsigned char *buffer512);
int ata_write_sector28(unsigned int lba, const unsigned char *buffer512);

#endif
