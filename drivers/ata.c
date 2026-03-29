#include "ata.h"
#include "arch.h"

#define ATA_IO_BASE      0x1F0
#define ATA_CTRL_BASE    0x3F6
#define ATA_IO_BASE_SEC  0x170
#define ATA_CTRL_BASE_SEC 0x376

#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_SECCOUNT0   0x02
#define ATA_REG_LBA0        0x03
#define ATA_REG_LBA1        0x04
#define ATA_REG_LBA2        0x05
#define ATA_REG_HDDEVSEL    0x06
#define ATA_REG_COMMAND     0x07
#define ATA_REG_STATUS      0x07

#define ATA_REG_ALTSTATUS   0x00
#define ATA_REG_CONTROL     0x00

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

static int ata_drive_present[ATA_MAX_DRIVES] = {0, 0, 0, 0};
static unsigned int ata_drive_sectors[ATA_MAX_DRIVES] = {0, 0, 0, 0};

static unsigned short ata_io_base_for_drive(int drive)
{
	return (drive < 2) ? ATA_IO_BASE : ATA_IO_BASE_SEC;
}

static unsigned short ata_ctrl_base_for_drive(int drive)
{
	return (drive < 2) ? ATA_CTRL_BASE : ATA_CTRL_BASE_SEC;
}

static int ata_is_slave_drive(int drive)
{
	return (drive & 1) != 0;
}

static unsigned char ata_status_drive(int drive)
{
	return arch_inb(ata_io_base_for_drive(drive) + ATA_REG_STATUS);
}

static unsigned char ata_altstatus_drive(int drive)
{
	return arch_inb(ata_ctrl_base_for_drive(drive) + ATA_REG_ALTSTATUS);
}

static void ata_400ns_delay_drive(int drive)
{
	ata_altstatus_drive(drive);
	ata_altstatus_drive(drive);
	ata_altstatus_drive(drive);
	ata_altstatus_drive(drive);
}

static int ata_wait_not_bsy_drive(int drive, unsigned int spin)
{
	while (spin--)
	{
		if ((ata_status_drive(drive) & ATA_SR_BSY) == 0) return 0;
	}
	return -1;
}

static int ata_wait_drq_or_err_drive(int drive, unsigned int spin)
{
	unsigned char st;
	while (spin--)
	{
		st = ata_status_drive(drive);
		if (st & ATA_SR_ERR) return -1;
		if (st & ATA_SR_DF)  return -1;
		if ((st & ATA_SR_BSY) == 0 && (st & ATA_SR_DRQ)) return 0;
	}
	return -1;
}

static void ata_select(int drive, unsigned int lba)
{
	unsigned short io_base = ata_io_base_for_drive(drive);
	unsigned char sel = (unsigned char)((ata_is_slave_drive(drive) ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
	arch_outb(io_base + ATA_REG_HDDEVSEL, sel);
	ata_400ns_delay_drive(drive);
}

int ata_init_drive(int drive)
{
	unsigned int i;
	unsigned short w;
	unsigned char st;
	unsigned short io_base = ata_io_base_for_drive(drive);
	unsigned short ctrl_base = ata_ctrl_base_for_drive(drive);

	if (drive < 0 || drive >= ATA_MAX_DRIVES) return -1;
	ata_drive_present[drive] = 0;
	ata_drive_sectors[drive] = 0;

	/* Soft reset resets the whole channel; only do it when probing channel master. */
	if (!ata_is_slave_drive(drive))
	{
		arch_outb(ctrl_base + ATA_REG_CONTROL, 0x04);
		ata_400ns_delay_drive(drive);
		arch_outb(ctrl_base + ATA_REG_CONTROL, 0x00);
	}

	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;

	ata_select(drive, 0);

	/* A floating bus reads 0xFF; a missing slave often gives 0x00 */
	st = ata_status_drive(drive);
	if (st == 0xFF || st == 0x00) return -1;

	arch_outb(io_base + ATA_REG_SECCOUNT0, 0);
	arch_outb(io_base + ATA_REG_LBA0, 0);
	arch_outb(io_base + ATA_REG_LBA1, 0);
	arch_outb(io_base + ATA_REG_LBA2, 0);
	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

	st = ata_status_drive(drive);
	if (st == 0) return -1;
	if (ata_wait_drq_or_err_drive(drive, 1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = arch_inw(io_base + ATA_REG_DATA);
		if (i == 60) ata_drive_sectors[drive] = w;
		if (i == 61) ata_drive_sectors[drive] |= ((unsigned int)w << 16);
	}

	ata_drive_present[drive] = 1;
	return 0;
}

int ata_init(void)
{
	return ata_init_drive(0);
}

int ata_is_present_drive(int drive)
{
	if (drive < 0 || drive >= ATA_MAX_DRIVES) return 0;
	return ata_drive_present[drive];
}

int ata_is_present(void)
{
	return ata_is_present_drive(0);
}

unsigned int ata_get_sector_count_drive(int drive)
{
	if (drive < 0 || drive >= ATA_MAX_DRIVES) return 0;
	return ata_drive_sectors[drive];
}

unsigned int ata_get_sector_count_low(void)
{
	return ata_get_sector_count_drive(0);
}

int ata_read_sector28_drive(int drive, unsigned int lba, unsigned char *buffer512)
{
	unsigned int i;
	unsigned short w;
	unsigned short io_base = ata_io_base_for_drive(drive);

	if (drive < 0 || drive >= ATA_MAX_DRIVES) return -1;
	if (!ata_drive_present[drive]) return -1;
	if (lba > 0x0FFFFFFF) return -1;

	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;

	ata_select(drive, lba);
	arch_outb(io_base + ATA_REG_SECCOUNT0, 1);
	arch_outb(io_base + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(io_base + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(io_base + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

	if (ata_wait_drq_or_err_drive(drive, 1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = arch_inw(io_base + ATA_REG_DATA);
		buffer512[i * 2] = (unsigned char)(w & 0xFF);
		buffer512[i * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
	}

	ata_400ns_delay_drive(drive);
	return 0;
}

int ata_read_sector28(unsigned int lba, unsigned char *buffer512)
{
	return ata_read_sector28_drive(0, lba, buffer512);
}

int ata_write_sector28_drive(int drive, unsigned int lba, const unsigned char *buffer512)
{
	unsigned int i;
	unsigned short w;
	unsigned short io_base = ata_io_base_for_drive(drive);

	if (drive < 0 || drive >= ATA_MAX_DRIVES) return -1;
	if (!ata_drive_present[drive]) return -1;
	if (lba > 0x0FFFFFFF) return -1;

	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;

	ata_select(drive, lba);
	arch_outb(io_base + ATA_REG_SECCOUNT0, 1);
	arch_outb(io_base + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(io_base + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(io_base + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

	if (ata_wait_drq_or_err_drive(drive, 1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = (unsigned short)buffer512[i * 2] | ((unsigned short)buffer512[i * 2 + 1] << 8);
		arch_outw(io_base + ATA_REG_DATA, w);
	}

	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;
	return 0;
}

int ata_write_sector28(unsigned int lba, const unsigned char *buffer512)
{
	return ata_write_sector28_drive(0, lba, buffer512);
}

/* Multi-sector read: reads N consecutive sectors starting from LBA.
   Buffer must be at least (count * 512) bytes. */
int ata_read_sectors_drive(int drive, unsigned int lba, int count, unsigned char *buffer)
{
	unsigned int i;
	unsigned int sector;
	unsigned short w;
	unsigned short io_base = ata_io_base_for_drive(drive);
	unsigned int word_offset;

	if (drive < 0 || drive >= ATA_MAX_DRIVES) return -1;
	if (!ata_drive_present[drive]) return -1;
	if (lba > 0x0FFFFFFF) return -1;
	if (count <= 0 || count > 255) return -1;

	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;

	ata_select(drive, lba);
	arch_outb(io_base + ATA_REG_SECCOUNT0, (unsigned char)count);
	arch_outb(io_base + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(io_base + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(io_base + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

	/* Read all sectors */
	word_offset = 0;
	for (sector = 0; sector < (unsigned int)count; sector++)
	{
		if (ata_wait_drq_or_err_drive(drive, 1000000) != 0) return -1;

		/* Read 256 words (512 bytes) per sector */
		for (i = 0; i < 256; i++)
		{
			w = arch_inw(io_base + ATA_REG_DATA);
			buffer[word_offset * 2] = (unsigned char)(w & 0xFF);
			buffer[word_offset * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
			word_offset++;
		}
	}

	ata_400ns_delay_drive(drive);
	return 0;
}

/* Multi-sector write: writes N consecutive sectors starting from LBA.
   Buffer must be at least (count * 512) bytes. */
int ata_write_sectors_drive(int drive, unsigned int lba, int count, const unsigned char *buffer)
{
	unsigned int i;
	unsigned int sector;
	unsigned short w;
	unsigned short io_base = ata_io_base_for_drive(drive);
	unsigned int word_offset;

	if (drive < 0 || drive >= ATA_MAX_DRIVES) return -1;
	if (!ata_drive_present[drive]) return -1;
	if (lba > 0x0FFFFFFF) return -1;
	if (count <= 0 || count > 255) return -1;

	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;

	ata_select(drive, lba);
	arch_outb(io_base + ATA_REG_SECCOUNT0, (unsigned char)count);
	arch_outb(io_base + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(io_base + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(io_base + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

	/* Write all sectors */
	word_offset = 0;
	for (sector = 0; sector < (unsigned int)count; sector++)
	{
		if (ata_wait_drq_or_err_drive(drive, 1000000) != 0) return -1;

		/* Write 256 words (512 bytes) per sector */
		for (i = 0; i < 256; i++)
		{
			w = (unsigned short)buffer[word_offset * 2] | ((unsigned short)buffer[word_offset * 2 + 1] << 8);
			arch_outw(io_base + ATA_REG_DATA, w);
			word_offset++;
		}
	}

	arch_outb(io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
	if (ata_wait_not_bsy_drive(drive, 1000000) != 0) return -1;
	return 0;
}
