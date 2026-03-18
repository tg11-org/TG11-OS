#include "fs.h"

#define FS_MAX_NODES      256
#define FS_MAX_FILE_SIZE  2048

struct fs_node
{
	int used;
	int is_dir;
	int parent;
	int first_child;
	int next_sibling;
	char name[FS_NAME_MAX + 1];
	unsigned long size;
	char data[FS_MAX_FILE_SIZE];
};

static struct fs_node nodes[FS_MAX_NODES];
static int cwd_node = 0;

static unsigned long str_len(const char *s)
{
	unsigned long n = 0;
	while (s[n] != '\0') n++;
	return n;
}

static int str_eq(const char *a, const char *b)
{
	unsigned long i = 0;
	while (a[i] != '\0' && b[i] != '\0')
	{
		if (a[i] != b[i]) return 0;
		i++;
	}
	return a[i] == '\0' && b[i] == '\0';
}

static void str_copy(char *dst, const char *src, unsigned long max_len)
{
	unsigned long i = 0;
	if (max_len == 0) return;
	while (src[i] != '\0' && i < (max_len - 1))
	{
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static int alloc_node(void)
{
	int i;
	for (i = 1; i < FS_MAX_NODES; i++)
	{
		if (!nodes[i].used)
		{
			nodes[i].used = 1;
			nodes[i].is_dir = 0;
			nodes[i].parent = -1;
			nodes[i].first_child = -1;
			nodes[i].next_sibling = -1;
			nodes[i].name[0] = '\0';
			nodes[i].size = 0;
			nodes[i].data[0] = '\0';
			return i;
		}
	}
	return -1;
}

static int find_child(int parent, const char *name)
{
	int n = nodes[parent].first_child;
	while (n != -1)
	{
		if (str_eq(nodes[n].name, name)) return n;
		n = nodes[n].next_sibling;
	}
	return -1;
}

static int add_child(int parent, const char *name, int is_dir)
{
	int idx;
	unsigned long name_len;

	if (parent < 0 || !nodes[parent].used || !nodes[parent].is_dir) return -1;
	if (name[0] == '\0') return -1;
	name_len = str_len(name);
	if (name_len > FS_NAME_MAX) return -1;
	if (str_eq(name, ".") || str_eq(name, "..")) return -1;
	if (find_child(parent, name) != -1) return -1;

	idx = alloc_node();
	if (idx < 0) return -1;

	nodes[idx].is_dir = is_dir;
	nodes[idx].parent = parent;
	nodes[idx].first_child = -1;
	nodes[idx].next_sibling = nodes[parent].first_child;
	str_copy(nodes[idx].name, name, sizeof(nodes[idx].name));
	nodes[parent].first_child = idx;
	return idx;
}

static int split_parent_and_name(const char *path, int *out_parent, char *out_name)
{
	int cur;
	const char *p = path;
	char part[FS_NAME_MAX + 1];
	unsigned long i;

	if (path == (void *)0 || path[0] == '\0') return -1;
	cur = (path[0] == '/') ? 0 : cwd_node;

	while (*p == '/') p++;
	if (*p == '\0') return -1;

	while (*p != '\0')
	{
		i = 0;
		while (*p != '\0' && *p != '/')
		{
			if (i >= FS_NAME_MAX) return -1;
			part[i++] = *p++;
		}
		part[i] = '\0';

		while (*p == '/') p++;

		if (*p == '\0')
		{
			if (str_eq(part, ".") || str_eq(part, "..")) return -1;
			*out_parent = cur;
			str_copy(out_name, part, FS_NAME_MAX + 1);
			return 0;
		}

		if (str_eq(part, "."))
		{
			continue;
		}
		if (str_eq(part, ".."))
		{
			if (nodes[cur].parent != -1) cur = nodes[cur].parent;
			continue;
		}

		cur = find_child(cur, part);
		if (cur < 0 || !nodes[cur].is_dir) return -1;
	}

	return -1;
}

static int resolve_path(const char *path)
{
	int cur;
	const char *p;
	char part[FS_NAME_MAX + 1];
	unsigned long i;

	if (path == (void *)0 || path[0] == '\0') return cwd_node;
	cur = (path[0] == '/') ? 0 : cwd_node;
	p = path;

	while (*p == '/') p++;
	if (*p == '\0') return 0;

	while (*p != '\0')
	{
		i = 0;
		while (*p != '\0' && *p != '/')
		{
			if (i >= FS_NAME_MAX) return -1;
			part[i++] = *p++;
		}
		part[i] = '\0';
		while (*p == '/') p++;

		if (str_eq(part, ".")) continue;
		if (str_eq(part, ".."))
		{
			if (nodes[cur].parent != -1) cur = nodes[cur].parent;
			continue;
		}

		cur = find_child(cur, part);
		if (cur < 0) return -1;
	}

	return cur;
}

static int unlink_node_from_parent(int idx)
{
	int parent;
	int cur;
	int prev;

	if (idx <= 0 || !nodes[idx].used) return -1;
	parent = nodes[idx].parent;
	if (parent < 0) return -1;

	cur = nodes[parent].first_child;
	prev = -1;
	while (cur != -1)
	{
		if (cur == idx)
		{
			if (prev == -1) nodes[parent].first_child = nodes[cur].next_sibling;
			else nodes[prev].next_sibling = nodes[cur].next_sibling;
			return 0;
		}
		prev = cur;
		cur = nodes[cur].next_sibling;
	}

	return -1;
}

void fs_init(void)
{
	int i;
	for (i = 0; i < FS_MAX_NODES; i++)
	{
		nodes[i].used = 0;
		nodes[i].is_dir = 0;
		nodes[i].parent = -1;
		nodes[i].first_child = -1;
		nodes[i].next_sibling = -1;
		nodes[i].name[0] = '\0';
		nodes[i].size = 0;
		nodes[i].data[0] = '\0';
	}

	nodes[0].used = 1;
	nodes[0].is_dir = 1;
	nodes[0].parent = -1;
	nodes[0].first_child = -1;
	nodes[0].next_sibling = -1;
	nodes[0].name[0] = '/';
	nodes[0].name[1] = '\0';
	cwd_node = 0;
}

int fs_mkdir(const char *path)
{
	int parent;
	char name[FS_NAME_MAX + 1];
	return split_parent_and_name(path, &parent, name) == 0 ? (add_child(parent, name, 1) >= 0 ? 0 : -1) : -1;
}

int fs_touch(const char *path)
{
	int parent;
	char name[FS_NAME_MAX + 1];
	if (split_parent_and_name(path, &parent, name) != 0) return -1;
	if (add_child(parent, name, 0) >= 0) return 0;

	/* If file already exists, treat touch as success */
	{
		int existing = find_child(parent, name);
		if (existing >= 0 && !nodes[existing].is_dir) return 0;
	}
	return -1;
}

int fs_write_file(const char *path, const unsigned char *data, unsigned long size)
{
	int node = resolve_path(path);
	unsigned long n;
	unsigned long i;

	if (node < 0)
	{
		if (fs_touch(path) != 0) return -1;
		node = resolve_path(path);
		if (node < 0) return -1;
	}

	if (nodes[node].is_dir) return -1;
	if (data == (void *)0 && size != 0) return -1;

	n = size;
	if (n >= FS_MAX_FILE_SIZE) n = FS_MAX_FILE_SIZE - 1;
	for (i = 0; i < n; i++) nodes[node].data[i] = (char)data[i];
	nodes[node].data[n] = '\0';
	nodes[node].size = n;
	return 0;
}

int fs_read_file(const char *path, unsigned char *out_data, unsigned long out_capacity, unsigned long *out_size)
{
	int node = resolve_path(path);
	unsigned long i;
	unsigned long n;
	if (node < 0 || nodes[node].is_dir || out_size == (void *)0) return -1;
	n = nodes[node].size;
	if (out_data != (void *)0)
	{
		unsigned long copy_n = (n < out_capacity) ? n : out_capacity;
		for (i = 0; i < copy_n; i++) out_data[i] = (unsigned char)nodes[node].data[i];
	}
	*out_size = (n < out_capacity) ? n : out_capacity;
	return 0;
}

int fs_write_text(const char *path, const char *text)
{
	return fs_write_file(path, (const unsigned char *)text, str_len(text));
}

int fs_read_text(const char *path, const char **out_text)
{
	int node = resolve_path(path);
	if (node < 0 || nodes[node].is_dir) return -1;
	*out_text = nodes[node].data;
	return 0;
}

int fs_rm(const char *path)
{
	int node = resolve_path(path);
	if (node <= 0) return -1;
	if (nodes[node].is_dir && nodes[node].first_child != -1) return -1;
	if (unlink_node_from_parent(node) != 0) return -1;
	nodes[node].used = 0;
	nodes[node].first_child = -1;
	nodes[node].next_sibling = -1;
	nodes[node].size = 0;
	nodes[node].name[0] = '\0';
	return 0;
}

int fs_cd(const char *path)
{
	int node = resolve_path(path);
	if (node < 0 || !nodes[node].is_dir) return -1;
	cwd_node = node;
	return 0;
}

int fs_cp(const char *src_path, const char *dst_path)
{
	int src = resolve_path(src_path);
	if (src < 0 || nodes[src].is_dir) return -1;
	return fs_write_file(dst_path, (const unsigned char *)nodes[src].data, nodes[src].size);
}

int fs_mv(const char *src_path, const char *dst_path)
{
	int src;
	int dst_parent;
	char dst_name[FS_NAME_MAX + 1];
	int old_parent;

	src = resolve_path(src_path);
	if (src <= 0) return -1;
	if (split_parent_and_name(dst_path, &dst_parent, dst_name) != 0) return -1;
	if (!nodes[dst_parent].is_dir) return -1;
	if (find_child(dst_parent, dst_name) != -1) return -1;
	if (nodes[src].is_dir)
	{
		int p = dst_parent;
		while (p != -1)
		{
			if (p == src) return -1;
			p = nodes[p].parent;
		}
	}

	old_parent = nodes[src].parent;
	if (unlink_node_from_parent(src) != 0) return -1;
	nodes[src].parent = dst_parent;
	nodes[src].next_sibling = nodes[dst_parent].first_child;
	nodes[dst_parent].first_child = src;
	str_copy(nodes[src].name, dst_name, sizeof(nodes[src].name));

	if (cwd_node == old_parent || cwd_node == src || old_parent != dst_parent)
	{
		/* no-op, path-based cwd stays valid by node index */
	}

	return 0;
}

void fs_get_pwd(char *buffer, unsigned long buffer_size)
{
	int stack[FS_MAX_NODES];
	int depth = 0;
	int n = cwd_node;
	unsigned long out = 0;
	int i;

	if (buffer_size == 0) return;

	if (cwd_node == 0)
	{
		if (buffer_size >= 2)
		{
			buffer[0] = '/';
			buffer[1] = '\0';
		}
		else
		{
			buffer[0] = '\0';
		}
		return;
	}

	while (n > 0 && depth < FS_MAX_NODES)
	{
		stack[depth++] = n;
		n = nodes[n].parent;
	}

	for (i = depth - 1; i >= 0; i--)
	{
		unsigned long j;
		if (out + 1 >= buffer_size) break;
		buffer[out++] = '/';
		for (j = 0; nodes[stack[i]].name[j] != '\0'; j++)
		{
			if (out + 1 >= buffer_size) break;
			buffer[out++] = nodes[stack[i]].name[j];
		}
	}

	buffer[out] = '\0';
}

int fs_ls(const char *path, char names[][FS_NAME_MAX + 2], int types[], int max_entries, int *out_count)
{
	int dir;
	int child;
	int count = 0;

	dir = (path == (void *)0 || path[0] == '\0') ? cwd_node : resolve_path(path);
	if (dir < 0 || !nodes[dir].is_dir) return -1;

	child = nodes[dir].first_child;
	while (child != -1 && count < max_entries)
	{
		int i = 0;
		while (nodes[child].name[i] != '\0' && i < FS_NAME_MAX)
		{
			names[count][i] = nodes[child].name[i];
			i++;
		}
		if (nodes[child].is_dir && i < FS_NAME_MAX + 1)
		{
			names[count][i++] = '/';
		}
		names[count][i] = '\0';
		types[count] = nodes[child].is_dir;
		count++;
		child = nodes[child].next_sibling;
	}

	*out_count = count;
	return 0;
}
