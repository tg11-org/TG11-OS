#include "ata.h"
#include "arch.h"

#define ATA_IO_BASE      0x1F0
#define ATA_CTRL_BASE    0x3F6

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

static int ata_present = 0;
static unsigned int ata_sector_count_low = 0;

static unsigned char ata_status(void)
{
	return arch_inb(ATA_IO_BASE + ATA_REG_STATUS);
}

static unsigned char ata_altstatus(void)
{
	return arch_inb(ATA_CTRL_BASE + ATA_REG_ALTSTATUS);
}

static void ata_400ns_delay(void)
{
	ata_altstatus();
	ata_altstatus();
	ata_altstatus();
	ata_altstatus();
}

static int ata_wait_not_bsy(unsigned int spin)
{
	while (spin--)
	{
		if ((ata_status() & ATA_SR_BSY) == 0) return 0;
	}
	return -1;
}

static int ata_wait_drq_or_err(unsigned int spin)
{
	unsigned char st;
	while (spin--)
	{
		st = ata_status();
		if (st & ATA_SR_ERR) return -1;
		if (st & ATA_SR_DF)  return -1;
		if ((st & ATA_SR_BSY) == 0 && (st & ATA_SR_DRQ)) return 0;
	}
	return -1;
}

static void ata_select_drive_lba(unsigned int lba)
{
	arch_outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, (unsigned char)(0xE0 | ((lba >> 24) & 0x0F)));
	ata_400ns_delay();
}

int ata_init(void)
{
	unsigned int i;
	unsigned short w;

	ata_present = 0;
	ata_sector_count_low = 0;

	arch_outb(ATA_CTRL_BASE + ATA_REG_CONTROL, 0x04);
	ata_400ns_delay();
	arch_outb(ATA_CTRL_BASE + ATA_REG_CONTROL, 0x00);

	if (ata_wait_not_bsy(1000000) != 0) return -1;

	ata_select_drive_lba(0);
	arch_outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 0);
	arch_outb(ATA_IO_BASE + ATA_REG_LBA0, 0);
	arch_outb(ATA_IO_BASE + ATA_REG_LBA1, 0);
	arch_outb(ATA_IO_BASE + ATA_REG_LBA2, 0);
	arch_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

	if (ata_status() == 0) return -1;
	if (ata_wait_drq_or_err(1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = arch_inw(ATA_IO_BASE + ATA_REG_DATA);
		if (i == 60) ata_sector_count_low = w;
		if (i == 61) ata_sector_count_low |= ((unsigned int)w << 16);
	}

	ata_present = 1;
	return 0;
}

int ata_is_present(void)
{
	return ata_present;
}

unsigned int ata_get_sector_count_low(void)
{
	return ata_sector_count_low;
}

int ata_read_sector28(unsigned int lba, unsigned char *buffer512)
{
	unsigned int i;
	unsigned short w;

	if (!ata_present) return -1;
	if (lba > 0x0FFFFFFF) return -1;

	if (ata_wait_not_bsy(1000000) != 0) return -1;

	ata_select_drive_lba(lba);
	arch_outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 1);
	arch_outb(ATA_IO_BASE + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

	if (ata_wait_drq_or_err(1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = arch_inw(ATA_IO_BASE + ATA_REG_DATA);
		buffer512[i * 2] = (unsigned char)(w & 0xFF);
		buffer512[i * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
	}

	ata_400ns_delay();
	return 0;
}

int ata_write_sector28(unsigned int lba, const unsigned char *buffer512)
{
	unsigned int i;
	unsigned short w;

	if (!ata_present) return -1;
	if (lba > 0x0FFFFFFF) return -1;

	if (ata_wait_not_bsy(1000000) != 0) return -1;

	ata_select_drive_lba(lba);
	arch_outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 1);
	arch_outb(ATA_IO_BASE + ATA_REG_LBA0, (unsigned char)(lba & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_LBA1, (unsigned char)((lba >> 8) & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_LBA2, (unsigned char)((lba >> 16) & 0xFF));
	arch_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

	if (ata_wait_drq_or_err(1000000) != 0) return -1;

	for (i = 0; i < 256; i++)
	{
		w = (unsigned short)buffer512[i * 2] | ((unsigned short)buffer512[i * 2 + 1] << 8);
		arch_outw(ATA_IO_BASE + ATA_REG_DATA, w);
	}

	arch_outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
	if (ata_wait_not_bsy(1000000) != 0) return -1;
	return 0;
}
