#ifndef TG11_ATA_H
#define TG11_ATA_H

/* Primary-master (drive 0) — original API, unchanged */
int ata_init(void);
int ata_is_present(void);
unsigned int ata_get_sector_count_low(void);
int ata_read_sector28(unsigned int lba, unsigned char *buffer512);
int ata_write_sector28(unsigned int lba, const unsigned char *buffer512);

/* Per-drive API: drive 0 = primary master, drive 1 = primary slave */
int ata_init_drive(int drive);
int ata_is_present_drive(int drive);
unsigned int ata_get_sector_count_drive(int drive);
int ata_read_sector28_drive(int drive, unsigned int lba, unsigned char *buffer512);
int ata_write_sector28_drive(int drive, unsigned int lba, const unsigned char *buffer512);

#endif
