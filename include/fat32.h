#ifndef TG11_FAT32_H
#define TG11_FAT32_H

#include "blockdev.h"

int fat32_mount(struct block_device *dev);
int fat32_is_mounted(void);
void fat32_unmount(void);
const char *fat32_last_error(void);

int fat32_ls_root(char names[][40], int max_entries, int *out_count);
int fat32_read_file_root(const char *name, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size);
int fat32_touch_file_root(const char *name);
int fat32_write_file_root(const char *name, const unsigned char *data, unsigned long size);
int fat32_delete_file_root(const char *name);

int fat32_ls_path(const char *path, char names[][40], int max_entries, int *out_count);
int fat32_read_file_path(const char *path, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size);
int fat32_touch_file_path(const char *path);
int fat32_write_file_path(const char *path, const unsigned char *data, unsigned long size);
int fat32_mkdir_path(const char *path);
int fat32_remove_path(const char *path);
int fat32_get_attr_path(const char *path, unsigned char *out_attr);
int fat32_set_attr_path(const char *path, unsigned char set_mask, unsigned char clear_mask, unsigned char *out_attr);
int fat32_stat_path(const char *path, int *out_is_dir, unsigned long *out_size);
int fat32_get_free_bytes(unsigned long *out_free_bytes);

/* Performance tuning: ATA multi-sector read batch size (1..16). */
int fat32_set_io_batch_sectors(unsigned int count);
unsigned int fat32_get_io_batch_sectors(void);

/* Cache controls for FAT-table lookups and data-sector reads. */
void fat32_set_data_cache_enabled(int enabled);
void fat32_set_fat_cache_enabled(int enabled);
int fat32_get_data_cache_enabled(void);
int fat32_get_fat_cache_enabled(void);
void fat32_flush_cache(void);

#endif
