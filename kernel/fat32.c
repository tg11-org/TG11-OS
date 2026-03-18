#include "fat32.h"

#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_LFN       0x0F
#define FAT32_EOC            0x0FFFFFF8UL

struct fat32_state
{
	int mounted;
	struct block_device *dev;
	unsigned int start_lba;
	unsigned short bytes_per_sector;
	unsigned char sectors_per_cluster;
	unsigned short reserved_sectors;
	unsigned char fat_count;
	unsigned int sectors_per_fat;
	unsigned int root_cluster;
	unsigned int fat_start_lba;
	unsigned int data_start_lba;
};

static struct fat32_state fs;

static unsigned short rd16(const unsigned char *p)
{
	return (unsigned short)p[0] | ((unsigned short)p[1] << 8);
}

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] |
	       ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[3] << 24);
}

static char upcase(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
	return c;
}

static int str_case_eq(const char *a, const char *b)
{
	while (*a != '\0' && *b != '\0')
	{
		if (upcase(*a) != upcase(*b)) return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

static int boot_is_fat32(const unsigned char *sector)
{
	if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;
	if (sector[82] == 'F' && sector[83] == 'A' && sector[84] == 'T' && sector[85] == '3' && sector[86] == '2') return 1;
	return 0;
}

static unsigned int cluster_to_lba(unsigned int cluster)
{
	return fs.data_start_lba + (cluster - 2U) * fs.sectors_per_cluster;
}

static int fat32_next_cluster(unsigned int cluster, unsigned int *next)
{
	unsigned char sec[512];
	unsigned int fat_offset;
	unsigned int fat_sector;
	unsigned int ent_off;
	unsigned int value;

	fat_offset = cluster * 4U;
	fat_sector = fs.fat_start_lba + (fat_offset / fs.bytes_per_sector);
	ent_off = fat_offset % fs.bytes_per_sector;

	if (fs.dev->read_sector(fat_sector, sec) != 0) return -1;
	if (ent_off > (unsigned int)fs.bytes_per_sector - 4U) return -1;

	value = rd32(&sec[ent_off]) & 0x0FFFFFFFUL;
	*next = value;
	return 0;
}

static int fat32_set_cluster_value(unsigned int cluster, unsigned int value)
{
	unsigned int fat_offset;
	unsigned int fat_sector;
	unsigned int ent_off;
	unsigned char sec[512];
	unsigned int fat_index;

	fat_offset = cluster * 4U;
	ent_off = fat_offset % fs.bytes_per_sector;
	if (ent_off > (unsigned int)fs.bytes_per_sector - 4U) return -1;

	for (fat_index = 0; fat_index < fs.fat_count; fat_index++)
	{
		fat_sector = fs.fat_start_lba + fat_index * fs.sectors_per_fat + (fat_offset / fs.bytes_per_sector);
		if (fs.dev->read_sector(fat_sector, sec) != 0) return -1;
		sec[ent_off + 0] = (unsigned char)(value & 0xFF);
		sec[ent_off + 1] = (unsigned char)((value >> 8) & 0xFF);
		sec[ent_off + 2] = (unsigned char)((value >> 16) & 0xFF);
		sec[ent_off + 3] = (unsigned char)((sec[ent_off + 3] & 0xF0) | ((value >> 24) & 0x0F));
		if (fs.dev->write_sector(fat_sector, sec) != 0) return -1;
	}

	return 0;
}

static int fat32_name_to_83(const char *name, unsigned char out83[11])
{
	unsigned long i;
	unsigned long base_len = 0;
	unsigned long ext_len = 0;
	int in_ext = 0;

	for (i = 0; i < 11; i++) out83[i] = ' ';
	if (name == (void *)0 || name[0] == '\0') return -1;

	for (i = 0; name[i] != '\0'; i++)
	{
		char c = upcase(name[i]);
		if (c == '/' || c == '\\') return -1;
		if (c == '.')
		{
			if (in_ext) return -1;
			in_ext = 1;
			continue;
		}

		if (!in_ext)
		{
			if (base_len >= 8) return -1;
			out83[base_len++] = (unsigned char)c;
		}
		else
		{
			if (ext_len >= 3) return -1;
			out83[8 + ext_len++] = (unsigned char)c;
		}
	}

	if (base_len == 0) return -1;
	return 0;
}

static int fat32_find_root_entry(const char *name, unsigned int *out_entry_lba, unsigned int *out_entry_off, unsigned char *out_entry)
{
	unsigned char name83[11];
	unsigned int cluster;
	unsigned int s;
	unsigned int off;
	unsigned int next;
	unsigned char sec[512];

	if (fat32_name_to_83(name, name83) != 0) return -1;

	cluster = fs.root_cluster;
	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			unsigned int lba = cluster_to_lba(cluster) + s;
			if (fs.dev->read_sector(lba, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				unsigned char first = sec[off + 0];
				unsigned char attr = sec[off + 11];
				unsigned int i;
				int match = 1;

				if (first == 0x00) return -1;
				if (first == 0xE5) continue;
				if (attr == FAT32_ATTR_LFN) continue;

				for (i = 0; i < 11; i++)
				{
					if (sec[off + i] != name83[i]) { match = 0; break; }
				}

				if (match)
				{
					if (out_entry_lba) *out_entry_lba = lba;
					if (out_entry_off) *out_entry_off = off;
					if (out_entry)
					{
						for (i = 0; i < 32; i++) out_entry[i] = sec[off + i];
					}
					return 0;
				}
			}
		}

		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	return -1;
}

static int fat32_find_free_root_entry(unsigned int *out_entry_lba, unsigned int *out_entry_off)
{
	unsigned int cluster;
	unsigned int s;
	unsigned int off;
	unsigned int next;
	unsigned char sec[512];

	cluster = fs.root_cluster;
	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			unsigned int lba = cluster_to_lba(cluster) + s;
			if (fs.dev->read_sector(lba, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				if (sec[off + 0] == 0x00 || sec[off + 0] == 0xE5)
				{
					*out_entry_lba = lba;
					*out_entry_off = off;
					return 0;
				}
			}
		}

		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	return -1;
}

static int fat32_free_chain(unsigned int first_cluster)
{
	unsigned int cur = first_cluster;
	while (cur >= 2 && cur < FAT32_EOC)
	{
		unsigned int next;
		if (fat32_next_cluster(cur, &next) != 0) return -1;
		if (fat32_set_cluster_value(cur, 0) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cur = next;
	}
	return 0;
}

static int fat32_alloc_cluster(unsigned int *out_cluster)
{
	unsigned int total_clusters;
	unsigned int c;

	total_clusters = ((fs.dev->sector_count - fs.data_start_lba) / fs.sectors_per_cluster) + 2U;
	for (c = 2; c < total_clusters; c++)
	{
		unsigned int val;
		if (fat32_next_cluster(c, &val) != 0) return -1;
		if (val == 0)
		{
			unsigned char zero[512];
			unsigned int s;
			unsigned int lba;

			if (fat32_set_cluster_value(c, 0x0FFFFFFF) != 0) return -1;
			for (s = 0; s < 512; s++) zero[s] = 0;
			for (s = 0; s < fs.sectors_per_cluster; s++)
			{
				lba = cluster_to_lba(c) + s;
				if (fs.dev->write_sector(lba, zero) != 0) return -1;
			}
			*out_cluster = c;
			return 0;
		}
	}

	return -1;
}

static void fat83_to_name(const unsigned char *entry, char *out, unsigned long out_sz)
{
	unsigned long i = 0;
	unsigned long j;
	unsigned long end_base = 8;
	unsigned long end_ext = 3;

	while (end_base > 0 && entry[end_base - 1] == ' ') end_base--;
	while (end_ext > 0 && entry[8 + end_ext - 1] == ' ') end_ext--;

	for (j = 0; j < end_base && i + 1 < out_sz; j++) out[i++] = (char)entry[j];
	if (end_ext > 0 && i + 2 < out_sz)
	{
		out[i++] = '.';
		for (j = 0; j < end_ext && i + 1 < out_sz; j++) out[i++] = (char)entry[8 + j];
	}
	out[i] = '\0';
}

int fat32_mount(struct block_device *dev)
{
	unsigned char sec[512];
	unsigned int start_lba = 0;
	unsigned int part_lba;

	fs.mounted = 0;
	if (dev == (void *)0 || !dev->present || dev->read_sector == (void *)0) return -1;
	if (dev->read_sector(0, sec) != 0) return -1;

	if (!boot_is_fat32(sec))
	{
		if (sec[510] != 0x55 || sec[511] != 0xAA) return -1;
		part_lba = rd32(&sec[446 + 8]);
		if (part_lba == 0) return -1;
		if (dev->read_sector(part_lba, sec) != 0) return -1;
		if (!boot_is_fat32(sec)) return -1;
		start_lba = part_lba;
	}

	fs.dev = dev;
	fs.start_lba = start_lba;
	fs.bytes_per_sector = rd16(&sec[11]);
	fs.sectors_per_cluster = sec[13];
	fs.reserved_sectors = rd16(&sec[14]);
	fs.fat_count = sec[16];
	fs.sectors_per_fat = rd32(&sec[36]);
	fs.root_cluster = rd32(&sec[44]);

	if (fs.bytes_per_sector != 512) return -1;
	if (fs.sectors_per_cluster == 0 || fs.fat_count == 0 || fs.sectors_per_fat == 0 || fs.root_cluster < 2) return -1;

	fs.fat_start_lba = fs.start_lba + fs.reserved_sectors;
	fs.data_start_lba = fs.fat_start_lba + (unsigned int)fs.fat_count * fs.sectors_per_fat;
	fs.mounted = 1;
	return 0;
}

int fat32_is_mounted(void)
{
	return fs.mounted;
}

void fat32_unmount(void)
{
	fs.mounted = 0;
	fs.dev = (void *)0;
	fs.start_lba = 0;
	fs.bytes_per_sector = 0;
	fs.sectors_per_cluster = 0;
	fs.reserved_sectors = 0;
	fs.fat_count = 0;
	fs.sectors_per_fat = 0;
	fs.root_cluster = 0;
	fs.fat_start_lba = 0;
	fs.data_start_lba = 0;
}

int fat32_ls_root(char names[][40], int max_entries, int *out_count)
{
	unsigned int cluster;
	int count = 0;
	unsigned char sec[512];
	unsigned int s;
	unsigned int off;
	unsigned int next;

	if (!fs.mounted || max_entries <= 0 || out_count == (void *)0) return -1;

	cluster = fs.root_cluster;
	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			if (fs.dev->read_sector(cluster_to_lba(cluster) + s, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				unsigned char first = sec[off + 0];
				unsigned char attr = sec[off + 11];
				if (first == 0x00)
				{
					*out_count = count;
					return 0;
				}
				if (first == 0xE5) continue;
				if (attr == FAT32_ATTR_LFN) continue;
				fat83_to_name(&sec[off], names[count], 40);
				if (attr & FAT32_ATTR_DIRECTORY)
				{
					unsigned long i = 0;
					while (names[count][i] != '\0' && i < 38) i++;
					names[count][i++] = '/';
					names[count][i] = '\0';
				}
				count++;
				if (count >= max_entries)
				{
					*out_count = count;
					return 0;
				}
			}
		}
		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	*out_count = count;
	return 0;
}

int fat32_read_file_root(const char *name, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size)
{
	unsigned int cluster;
	unsigned char sec[512];
	unsigned int s;
	unsigned int off;
	unsigned int file_cluster = 0;
	unsigned long file_size = 0;
	unsigned int attr = 0;

	if (!fs.mounted || name == (void *)0 || out_data == (void *)0 || out_size == (void *)0) return -1;

	cluster = fs.root_cluster;
	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			char tmp[40];
			if (fs.dev->read_sector(cluster_to_lba(cluster) + s, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				unsigned char first = sec[off + 0];
				unsigned char at = sec[off + 11];
				if (first == 0x00) goto file_search_done;
				if (first == 0xE5) continue;
				if (at == FAT32_ATTR_LFN) continue;
				fat83_to_name(&sec[off], tmp, sizeof(tmp));
				if (str_case_eq(tmp, name))
				{
					unsigned int hi = rd16(&sec[off + 20]);
					unsigned int lo = rd16(&sec[off + 26]);
					file_cluster = (hi << 16) | lo;
					file_size = rd32(&sec[off + 28]);
					attr = at;
					goto file_search_done;
				}
			}
		}
		{
			unsigned int next;
			if (fat32_next_cluster(cluster, &next) != 0) return -1;
			if (next >= FAT32_EOC) break;
			cluster = next;
		}
	}

file_search_done:
	if (file_cluster < 2 || (attr & FAT32_ATTR_DIRECTORY)) return -1;

	{
		unsigned long written = 0;
		unsigned int cur = file_cluster;
		while (cur >= 2 && cur < FAT32_EOC && written < file_size)
		{
			for (s = 0; s < fs.sectors_per_cluster && written < file_size; s++)
			{
				unsigned long i;
				if (fs.dev->read_sector(cluster_to_lba(cur) + s, sec) != 0) return -1;
				for (i = 0; i < 512 && written < file_size; i++)
				{
					if (written < out_capacity) out_data[written] = sec[i];
					written++;
				}
			}
			{
				unsigned int next;
				if (fat32_next_cluster(cur, &next) != 0) return -1;
				if (next >= FAT32_EOC) break;
				cur = next;
			}
		}
		*out_size = (file_size < out_capacity) ? file_size : out_capacity;
		return 0;
	}
}

int fat32_touch_file_root(const char *name)
{
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char sec[512];
	unsigned char name83[11];
	unsigned int i;

	if (!fs.mounted || name == (void *)0) return -1;
	if (fat32_find_root_entry(name, (void *)0, (void *)0, (void *)0) == 0) return 0;
	if (fat32_name_to_83(name, name83) != 0) return -1;
	if (fat32_find_free_root_entry(&entry_lba, &entry_off) != 0) return -1;
	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;

	for (i = 0; i < 32; i++) sec[entry_off + i] = 0;
	for (i = 0; i < 11; i++) sec[entry_off + i] = name83[i];
	sec[entry_off + 11] = 0x20;
	sec[entry_off + 26] = 0;
	sec[entry_off + 27] = 0;
	sec[entry_off + 28] = 0;
	sec[entry_off + 29] = 0;
	sec[entry_off + 30] = 0;
	sec[entry_off + 31] = 0;

	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;
	return 0;
}

int fat32_write_file_root(const char *name, const unsigned char *data, unsigned long size)
{
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char entry[32];
	unsigned int cur_cluster;
	unsigned int first_cluster = 0;
	unsigned int prev_cluster = 0;
	unsigned int needed_clusters;
	unsigned int allocated = 0;
	unsigned long written = 0;
	unsigned char sec[512];
	unsigned int s;

	if (!fs.mounted || name == (void *)0 || data == (void *)0) return -1;
	if (fat32_find_root_entry(name, &entry_lba, &entry_off, entry) != 0)
	{
		if (fat32_touch_file_root(name) != 0) return -1;
		if (fat32_find_root_entry(name, &entry_lba, &entry_off, entry) != 0) return -1;
	}

	if (entry[11] & FAT32_ATTR_DIRECTORY) return -1;

	{
		unsigned int old_hi = rd16(&entry[20]);
		unsigned int old_lo = rd16(&entry[26]);
		unsigned int old_first = (old_hi << 16) | old_lo;
		if (old_first >= 2 && fat32_free_chain(old_first) != 0) return -1;
	}

	needed_clusters = (size == 0) ? 0 : (unsigned int)((size + (fs.sectors_per_cluster * 512UL) - 1) / (fs.sectors_per_cluster * 512UL));

	for (allocated = 0; allocated < needed_clusters; allocated++)
	{
		if (fat32_alloc_cluster(&cur_cluster) != 0)
		{
			if (first_cluster >= 2) fat32_free_chain(first_cluster);
			return -1;
		}

		if (first_cluster == 0) first_cluster = cur_cluster;
		if (prev_cluster >= 2)
		{
			if (fat32_set_cluster_value(prev_cluster, cur_cluster) != 0)
			{
				if (first_cluster >= 2) fat32_free_chain(first_cluster);
				return -1;
			}
		}
		prev_cluster = cur_cluster;
	}

	if (prev_cluster >= 2)
	{
		if (fat32_set_cluster_value(prev_cluster, 0x0FFFFFFF) != 0)
		{
			if (first_cluster >= 2) fat32_free_chain(first_cluster);
			return -1;
		}
	}

	cur_cluster = first_cluster;
	while (cur_cluster >= 2 && written < size)
	{
		for (s = 0; s < fs.sectors_per_cluster && written < size; s++)
		{
			unsigned int i;
			unsigned int lba = cluster_to_lba(cur_cluster) + s;
			for (i = 0; i < 512; i++)
			{
				if (written < size) sec[i] = data[written++];
				else sec[i] = 0;
			}
			if (fs.dev->write_sector(lba, sec) != 0) return -1;
		}
		if (written >= size) break;
		if (fat32_next_cluster(cur_cluster, &cur_cluster) != 0) return -1;
		if (cur_cluster >= FAT32_EOC) break;
	}

	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;
	sec[entry_off + 11] = 0x20;
	sec[entry_off + 20] = (unsigned char)((first_cluster >> 16) & 0xFF);
	sec[entry_off + 21] = (unsigned char)((first_cluster >> 24) & 0xFF);
	sec[entry_off + 26] = (unsigned char)(first_cluster & 0xFF);
	sec[entry_off + 27] = (unsigned char)((first_cluster >> 8) & 0xFF);
	sec[entry_off + 28] = (unsigned char)(size & 0xFF);
	sec[entry_off + 29] = (unsigned char)((size >> 8) & 0xFF);
	sec[entry_off + 30] = (unsigned char)((size >> 16) & 0xFF);
	sec[entry_off + 31] = (unsigned char)((size >> 24) & 0xFF);
	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;

	return 0;
}

int fat32_delete_file_root(const char *name)
{
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char entry[32];
	unsigned int first_cluster;
	unsigned char sec[512];

	if (!fs.mounted || name == (void *)0) return -1;
	if (fat32_find_root_entry(name, &entry_lba, &entry_off, entry) != 0) return -1;
	if (entry[11] & FAT32_ATTR_DIRECTORY) return -1;

	first_cluster = ((unsigned int)rd16(&entry[20]) << 16) | rd16(&entry[26]);
	if (first_cluster >= 2 && fat32_free_chain(first_cluster) != 0) return -1;

	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;
	sec[entry_off + 0] = 0xE5;
	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;

	return 0;
}

static const char *path_next_part(const char *path, char *out, unsigned long out_sz)
{
	unsigned long i = 0;

	while (*path == '/' || *path == '\\') path++;
	if (*path == '\0')
	{
		if (out_sz > 0) out[0] = '\0';
		return path;
	}

	while (*path != '\0' && *path != '/' && *path != '\\')
	{
		if (i + 1 >= out_sz) return (void *)0;
		out[i++] = *path++;
	}
	out[i] = '\0';
	return path;
}

static unsigned int entry_first_cluster(const unsigned char *entry)
{
	return ((unsigned int)rd16(&entry[20]) << 16) | rd16(&entry[26]);
}

static void entry_set_first_cluster(unsigned char *entry, unsigned int cluster)
{
	entry[20] = (unsigned char)((cluster >> 16) & 0xFF);
	entry[21] = (unsigned char)((cluster >> 24) & 0xFF);
	entry[26] = (unsigned char)(cluster & 0xFF);
	entry[27] = (unsigned char)((cluster >> 8) & 0xFF);
}

static int entry_name_is_dot_or_dotdot(const unsigned char *entry)
{
	if (entry[0] == '.') return 1;
	if (entry[0] == '.' && entry[1] == '.') return 1;
	if (entry[0] == '.' && entry[1] == ' ' && entry[2] == ' ') return 1;
	if (entry[0] == '.' && entry[1] == '.' && entry[2] == ' ') return 1;
	return 0;
}

static int fat32_find_entry_in_dir(unsigned int dir_cluster, const unsigned char name83[11], unsigned int *out_entry_lba, unsigned int *out_entry_off, unsigned char *out_entry)
{
	unsigned int cluster = dir_cluster;
	unsigned int s;
	unsigned int off;
	unsigned int next;
	unsigned char sec[512];

	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			unsigned int lba = cluster_to_lba(cluster) + s;
			if (fs.dev->read_sector(lba, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				unsigned char first = sec[off + 0];
				unsigned char attr = sec[off + 11];
				unsigned int i;
				int match = 1;

				if (first == 0x00) return -1;
				if (first == 0xE5) continue;
				if (attr == FAT32_ATTR_LFN) continue;

				for (i = 0; i < 11; i++)
				{
					if (sec[off + i] != name83[i]) { match = 0; break; }
				}

				if (match)
				{
					if (out_entry_lba) *out_entry_lba = lba;
					if (out_entry_off) *out_entry_off = off;
					if (out_entry)
					{
						for (i = 0; i < 32; i++) out_entry[i] = sec[off + i];
					}
					return 0;
				}
			}
		}

		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	return -1;
}

static int fat32_find_free_entry_in_dir(unsigned int dir_cluster, unsigned int *out_entry_lba, unsigned int *out_entry_off)
{
	unsigned int cluster = dir_cluster;
	unsigned int s;
	unsigned int off;
	unsigned int next;
	unsigned char sec[512];

	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			unsigned int lba = cluster_to_lba(cluster) + s;
			if (fs.dev->read_sector(lba, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				if (sec[off + 0] == 0x00 || sec[off + 0] == 0xE5)
				{
					*out_entry_lba = lba;
					*out_entry_off = off;
					return 0;
				}
			}
		}

		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	return -1;
}

static int fat32_get_parent_cluster(unsigned int dir_cluster, unsigned int *out_parent)
{
	unsigned char dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
	unsigned char entry[32];

	if (dir_cluster == fs.root_cluster)
	{
		*out_parent = fs.root_cluster;
		return 0;
	}

	if (fat32_find_entry_in_dir(dir_cluster, dotdot, (void *)0, (void *)0, entry) != 0)
	{
		*out_parent = fs.root_cluster;
		return 0;
	}

	*out_parent = entry_first_cluster(entry);
	if (*out_parent < 2) *out_parent = fs.root_cluster;
	return 0;
}

static int fat32_resolve_dir_cluster(const char *path, unsigned int *out_cluster)
{
	char part[16];
	const char *p;
	unsigned int cur;

	if (!fs.mounted || out_cluster == (void *)0) return -1;
	if (path == (void *)0 || path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
	{
		*out_cluster = fs.root_cluster;
		return 0;
	}

	cur = fs.root_cluster;
	p = path;
	for (;;)
	{
		unsigned char name83[11];
		unsigned char entry[32];
		const char *np = path_next_part(p, part, sizeof(part));
		if (np == (void *)0) return -1;
		if (part[0] == '\0') break;

		if (part[0] == '.' && part[1] == '\0')
		{
			p = np;
			continue;
		}
		if (part[0] == '.' && part[1] == '.' && part[2] == '\0')
		{
			unsigned int parent;
			if (fat32_get_parent_cluster(cur, &parent) != 0) return -1;
			cur = parent;
			p = np;
			continue;
		}

		if (fat32_name_to_83(part, name83) != 0) return -1;
		if (fat32_find_entry_in_dir(cur, name83, (void *)0, (void *)0, entry) != 0) return -1;
		if (!(entry[11] & FAT32_ATTR_DIRECTORY)) return -1;
		cur = entry_first_cluster(entry);
		if (cur < 2) return -1;
		p = np;
	}

	*out_cluster = cur;
	return 0;
}

static int fat32_resolve_parent_and_leaf(const char *path, unsigned int *out_parent_cluster, unsigned char out_name83[11])
{
	char part[16];
	char last[16];
	const char *p;
	unsigned int cur;

	if (!fs.mounted || path == (void *)0 || out_parent_cluster == (void *)0 || out_name83 == (void *)0) return -1;

	last[0] = '\0';
	cur = fs.root_cluster;
	p = path;

	for (;;)
	{
		const char *np = path_next_part(p, part, sizeof(part));
		if (np == (void *)0) return -1;
		if (part[0] == '\0') break;
		{
			char check[16];
			const char *peek = path_next_part(np, check, sizeof(check));
			if (peek == (void *)0) return -1;
			if (check[0] == '\0')
			{
				unsigned long i;
				for (i = 0; i < sizeof(last); i++) last[i] = part[i];
				break;
			}
		}

		if (part[0] == '.' && part[1] == '\0')
		{
			p = np;
			continue;
		}
		if (part[0] == '.' && part[1] == '.' && part[2] == '\0')
		{
			unsigned int parent;
			if (fat32_get_parent_cluster(cur, &parent) != 0) return -1;
			cur = parent;
			p = np;
			continue;
		}
		else
		{
			unsigned char name83[11];
			unsigned char entry[32];
			if (fat32_name_to_83(part, name83) != 0) return -1;
			if (fat32_find_entry_in_dir(cur, name83, (void *)0, (void *)0, entry) != 0) return -1;
			if (!(entry[11] & FAT32_ATTR_DIRECTORY)) return -1;
			cur = entry_first_cluster(entry);
			if (cur < 2) return -1;
			p = np;
		}
	}

	if (last[0] == '\0') return -1;
	if ((last[0] == '.' && last[1] == '\0') || (last[0] == '.' && last[1] == '.' && last[2] == '\0')) return -1;
	if (fat32_name_to_83(last, out_name83) != 0) return -1;
	*out_parent_cluster = cur;
	return 0;
}

static int fat32_ls_dir_cluster(unsigned int dir_cluster, char names[][40], int max_entries, int *out_count)
{
	unsigned int cluster = dir_cluster;
	int count = 0;
	unsigned char sec[512];
	unsigned int s;
	unsigned int off;
	unsigned int next;

	if (!fs.mounted || max_entries <= 0 || out_count == (void *)0) return -1;

	while (cluster >= 2 && cluster < FAT32_EOC)
	{
		for (s = 0; s < fs.sectors_per_cluster; s++)
		{
			if (fs.dev->read_sector(cluster_to_lba(cluster) + s, sec) != 0) return -1;
			for (off = 0; off < 512; off += 32)
			{
				unsigned char first = sec[off + 0];
				unsigned char attr = sec[off + 11];
				if (first == 0x00)
				{
					*out_count = count;
					return 0;
				}
				if (first == 0xE5) continue;
				if (attr == FAT32_ATTR_LFN) continue;
				if (entry_name_is_dot_or_dotdot(&sec[off])) continue;

				fat83_to_name(&sec[off], names[count], 40);
				if (attr & FAT32_ATTR_DIRECTORY)
				{
					unsigned long i = 0;
					while (names[count][i] != '\0' && i < 38) i++;
					names[count][i++] = '/';
					names[count][i] = '\0';
				}
				count++;
				if (count >= max_entries)
				{
					*out_count = count;
					return 0;
				}
			}
		}

		if (fat32_next_cluster(cluster, &next) != 0) return -1;
		if (next >= FAT32_EOC) break;
		cluster = next;
	}

	*out_count = count;
	return 0;
}

static int fat32_read_file_entry(unsigned char entry[32], unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size)
{
	unsigned int file_cluster = entry_first_cluster(entry);
	unsigned long file_size = rd32(&entry[28]);
	unsigned char sec[512];
	unsigned int s;
	unsigned long written = 0;
	unsigned int cur;

	if (entry[11] & FAT32_ATTR_DIRECTORY) return -1;
	if (file_cluster < 2)
	{
		*out_size = 0;
		return 0;
	}

	cur = file_cluster;
	while (cur >= 2 && cur < FAT32_EOC && written < file_size)
	{
		for (s = 0; s < fs.sectors_per_cluster && written < file_size; s++)
		{
			unsigned long i;
			if (fs.dev->read_sector(cluster_to_lba(cur) + s, sec) != 0) return -1;
			for (i = 0; i < 512 && written < file_size; i++)
			{
				if (written < out_capacity) out_data[written] = sec[i];
				written++;
			}
		}
		if (written >= file_size) break;
		if (fat32_next_cluster(cur, &cur) != 0) return -1;
		if (cur >= FAT32_EOC) break;
	}

	*out_size = (file_size < out_capacity) ? file_size : out_capacity;
	return 0;
}

int fat32_ls_path(const char *path, char names[][40], int max_entries, int *out_count)
{
	unsigned int dir_cluster;
	if (fat32_resolve_dir_cluster(path, &dir_cluster) != 0) return -1;
	return fat32_ls_dir_cluster(dir_cluster, names, max_entries, out_count);
}

int fat32_read_file_path(const char *path, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size)
{
	unsigned int parent;
	unsigned char name83[11];
	unsigned char entry[32];

	if (!fs.mounted || path == (void *)0 || out_data == (void *)0 || out_size == (void *)0) return -1;
	if (fat32_resolve_parent_and_leaf(path, &parent, name83) != 0) return -1;
	if (fat32_find_entry_in_dir(parent, name83, (void *)0, (void *)0, entry) != 0) return -1;
	return fat32_read_file_entry(entry, out_data, out_capacity, out_size);
}

int fat32_touch_file_path(const char *path)
{
	unsigned int parent;
	unsigned char name83[11];
	unsigned char entry[32];
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned int i;
	unsigned char sec[512];

	if (!fs.mounted || path == (void *)0) return -1;
	if (fat32_resolve_parent_and_leaf(path, &parent, name83) != 0) return -1;
	if (fat32_find_entry_in_dir(parent, name83, (void *)0, (void *)0, entry) == 0)
	{
		if (entry[11] & FAT32_ATTR_DIRECTORY) return -1;
		return 0;
	}
	if (fat32_find_free_entry_in_dir(parent, &entry_lba, &entry_off) != 0) return -1;
	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;

	for (i = 0; i < 32; i++) sec[entry_off + i] = 0;
	for (i = 0; i < 11; i++) sec[entry_off + i] = name83[i];
	sec[entry_off + 11] = 0x20;
	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;
	return 0;
}

int fat32_write_file_path(const char *path, const unsigned char *data, unsigned long size)
{
	unsigned int parent;
	unsigned char name83[11];
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char entry[32];
	unsigned int cur_cluster;
	unsigned int first_cluster = 0;
	unsigned int prev_cluster = 0;
	unsigned int needed_clusters;
	unsigned int allocated = 0;
	unsigned long written = 0;
	unsigned char sec[512];
	unsigned int s;

	if (!fs.mounted || path == (void *)0 || data == (void *)0) return -1;
	if (fat32_resolve_parent_and_leaf(path, &parent, name83) != 0) return -1;
	if (fat32_find_entry_in_dir(parent, name83, &entry_lba, &entry_off, entry) != 0)
	{
		if (fat32_touch_file_path(path) != 0) return -1;
		if (fat32_find_entry_in_dir(parent, name83, &entry_lba, &entry_off, entry) != 0) return -1;
	}
	if (entry[11] & FAT32_ATTR_DIRECTORY) return -1;

	{
		unsigned int old_first = entry_first_cluster(entry);
		if (old_first >= 2 && fat32_free_chain(old_first) != 0) return -1;
	}

	needed_clusters = (size == 0) ? 0 : (unsigned int)((size + (fs.sectors_per_cluster * 512UL) - 1) / (fs.sectors_per_cluster * 512UL));
	for (allocated = 0; allocated < needed_clusters; allocated++)
	{
		if (fat32_alloc_cluster(&cur_cluster) != 0)
		{
			if (first_cluster >= 2) fat32_free_chain(first_cluster);
			return -1;
		}
		if (first_cluster == 0) first_cluster = cur_cluster;
		if (prev_cluster >= 2)
		{
			if (fat32_set_cluster_value(prev_cluster, cur_cluster) != 0)
			{
				if (first_cluster >= 2) fat32_free_chain(first_cluster);
				return -1;
			}
		}
		prev_cluster = cur_cluster;
	}

	if (prev_cluster >= 2)
	{
		if (fat32_set_cluster_value(prev_cluster, 0x0FFFFFFF) != 0)
		{
			if (first_cluster >= 2) fat32_free_chain(first_cluster);
			return -1;
		}
	}

	cur_cluster = first_cluster;
	while (cur_cluster >= 2 && written < size)
	{
		for (s = 0; s < fs.sectors_per_cluster && written < size; s++)
		{
			unsigned int i;
			unsigned int lba = cluster_to_lba(cur_cluster) + s;
			for (i = 0; i < 512; i++)
			{
				if (written < size) sec[i] = data[written++];
				else sec[i] = 0;
			}
			if (fs.dev->write_sector(lba, sec) != 0) return -1;
		}
		if (written >= size) break;
		if (fat32_next_cluster(cur_cluster, &cur_cluster) != 0) return -1;
		if (cur_cluster >= FAT32_EOC) break;
	}

	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;
	sec[entry_off + 11] = 0x20;
	entry_set_first_cluster(&sec[entry_off], first_cluster);
	sec[entry_off + 28] = (unsigned char)(size & 0xFF);
	sec[entry_off + 29] = (unsigned char)((size >> 8) & 0xFF);
	sec[entry_off + 30] = (unsigned char)((size >> 16) & 0xFF);
	sec[entry_off + 31] = (unsigned char)((size >> 24) & 0xFF);
	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;

	return 0;
}

int fat32_mkdir_path(const char *path)
{
	unsigned int parent;
	unsigned char name83[11];
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char sec[512];
	unsigned int i;
	unsigned int cluster;
	unsigned int lba;

	if (!fs.mounted || path == (void *)0) return -1;
	if (fat32_resolve_parent_and_leaf(path, &parent, name83) != 0) return -1;
	if (fat32_find_entry_in_dir(parent, name83, (void *)0, (void *)0, (void *)0) == 0) return -1;
	if (fat32_find_free_entry_in_dir(parent, &entry_lba, &entry_off) != 0) return -1;
	if (fat32_alloc_cluster(&cluster) != 0) return -1;

	for (i = 0; i < fs.sectors_per_cluster; i++)
	{
		unsigned int j;
		lba = cluster_to_lba(cluster) + i;
		for (j = 0; j < 512; j++) sec[j] = 0;
		if (fs.dev->write_sector(lba, sec) != 0)
		{
			fat32_free_chain(cluster);
			return -1;
		}
	}

	lba = cluster_to_lba(cluster);
	if (fs.dev->read_sector(lba, sec) != 0)
	{
		fat32_free_chain(cluster);
		return -1;
	}
	for (i = 0; i < 32; i++) sec[i] = 0;
	sec[0] = '.';
	for (i = 1; i < 11; i++) sec[i] = ' ';
	sec[11] = FAT32_ATTR_DIRECTORY;
	entry_set_first_cluster(&sec[0], cluster);

	for (i = 0; i < 32; i++) sec[32 + i] = 0;
	sec[32 + 0] = '.';
	sec[32 + 1] = '.';
	for (i = 2; i < 11; i++) sec[32 + i] = ' ';
	sec[32 + 11] = FAT32_ATTR_DIRECTORY;
	entry_set_first_cluster(&sec[32], parent);
	if (fs.dev->write_sector(lba, sec) != 0)
	{
		fat32_free_chain(cluster);
		return -1;
	}

	if (fs.dev->read_sector(entry_lba, sec) != 0)
	{
		fat32_free_chain(cluster);
		return -1;
	}
	for (i = 0; i < 32; i++) sec[entry_off + i] = 0;
	for (i = 0; i < 11; i++) sec[entry_off + i] = name83[i];
	sec[entry_off + 11] = FAT32_ATTR_DIRECTORY;
	entry_set_first_cluster(&sec[entry_off], cluster);
	if (fs.dev->write_sector(entry_lba, sec) != 0)
	{
		fat32_free_chain(cluster);
		return -1;
	}

	return 0;
}

int fat32_remove_path(const char *path)
{
	unsigned int parent;
	unsigned char name83[11];
	unsigned int entry_lba;
	unsigned int entry_off;
	unsigned char entry[32];
	unsigned int first_cluster;
	unsigned char sec[512];

	if (!fs.mounted || path == (void *)0) return -1;
	if (fat32_resolve_parent_and_leaf(path, &parent, name83) != 0) return -1;
	if (fat32_find_entry_in_dir(parent, name83, &entry_lba, &entry_off, entry) != 0) return -1;

	first_cluster = entry_first_cluster(entry);
	if (entry[11] & FAT32_ATTR_DIRECTORY)
	{
		char names[2][40];
		int count;
		if (first_cluster < 2) return -1;
		if (fat32_ls_dir_cluster(first_cluster, names, 2, &count) != 0) return -1;
		if (count != 0) return -1;
		if (fat32_free_chain(first_cluster) != 0) return -1;
	}
	else
	{
		if (first_cluster >= 2 && fat32_free_chain(first_cluster) != 0) return -1;
	}

	if (fs.dev->read_sector(entry_lba, sec) != 0) return -1;
	sec[entry_off + 0] = 0xE5;
	if (fs.dev->write_sector(entry_lba, sec) != 0) return -1;
	return 0;
}
