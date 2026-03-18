#ifndef TG11_FS_H
#define TG11_FS_H

#define FS_NAME_MAX 31
#define FS_MAX_LIST 64

void fs_init(void);

int fs_mkdir(const char *path);
int fs_touch(const char *path);
int fs_write_file(const char *path, const unsigned char *data, unsigned long size);
int fs_read_file(const char *path, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size);
int fs_write_text(const char *path, const char *text);
int fs_read_text(const char *path, const char **out_text);
int fs_rm(const char *path);
int fs_cd(const char *path);
int fs_cp(const char *src_path, const char *dst_path);
int fs_mv(const char *src_path, const char *dst_path);

void fs_get_pwd(char *buffer, unsigned long buffer_size);
int fs_ls(const char *path, char names[][FS_NAME_MAX + 2], int types[], int max_entries, int *out_count);

#endif
