#include "basic.h"
#include "terminal.h"
#include "screen.h"
#include "fs.h"

#define BASIC_MAX_LINES 256
#define BASIC_MAX_STEPS 20000
#define BASIC_MAX_CALL_DEPTH 32
#define BASIC_MAX_FOR_DEPTH 16
#define BASIC_MAX_WHILE_DEPTH 16
#define BASIC_MAX_STRING_LEN 96
#define BASIC_ARRAY_MAX 128
#define BASIC_MAX_DEF_FN 16

struct basic_line
{
	int label;
	char *text;
};

struct basic_for_frame
{
	int var_idx;
	int end_value;
	int step_value;
	int body_pc;
};

struct basic_while_frame
{
	int while_pc;
};

struct basic_def_fn
{
	int fn_letter;
	int param_var;
	char body[128];
};

enum b_cmp_op
{
	B_CMP_EQ = 0,
	B_CMP_NE,
	B_CMP_LT,
	B_CMP_LE,
	B_CMP_GT,
	B_CMP_GE
};

static int *b_rt_vars = (void *)0;
static char (*b_rt_str_vars)[BASIC_MAX_STRING_LEN] = (void *)0;
static int (*b_rt_arrays)[BASIC_ARRAY_MAX] = (void *)0;
static int *b_rt_array_sizes = (void *)0;
static unsigned int b_rt_rand_state = 0x12345678U;
static struct basic_def_fn b_def_fns[BASIC_MAX_DEF_FN];
static int b_def_fn_count = 0;

#define BASIC_MAX_FILES 4
#define BASIC_FILE_BUF 8192
struct basic_file_chan
{
	int active;
	int mode; /* 0=input, 1=output, 2=append */
	char path[64];
	char buf[BASIC_FILE_BUF];
	int buf_len;
	int buf_pos;
};
static struct basic_file_chan b_files[BASIC_MAX_FILES];

static int b_is_space(char c)
{
	return c == ' ' || c == '\t';
}

static int b_is_digit(char c)
{
	return c >= '0' && c <= '9';
}

static int b_is_alpha(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char b_up(char c)
{
	if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
	return c;
}

static int b_is_ident_tail(char c)
{
	return b_is_alpha(c) || b_is_digit(c) || c == '_';
}

static int b_starts_with_ci(const char *s, const char *prefix)
{
	while (*prefix != '\0')
	{
		if (b_up(*s) != b_up(*prefix)) return 0;
		s++;
		prefix++;
	}
	return 1;
}

static const char *b_skip_spaces(const char *s)
{
	while (b_is_space(*s)) s++;
	return s;
}

static int b_parse_int(const char **sp, int *out)
{
	const char *s = b_skip_spaces(*sp);
	int sign = 1;
	int v = 0;
	int any = 0;

	if (*s == '-') { sign = -1; s++; }
	if (!b_is_digit(*s)) return -1;

	while (b_is_digit(*s))
	{
		v = v * 10 + (*s - '0');
		s++;
		any = 1;
	}
	if (!any) return -1;
	*out = v * sign;
	*sp = s;
	return 0;
}

static int b_parse_var(const char **sp, int *idx)
{
	const char *s = b_skip_spaces(*sp);
	if (!b_is_alpha(*s)) return -1;
	*idx = b_up(*s) - 'A';
	if (*idx < 0 || *idx > 25) return -1;
	s++;
	while (b_is_alpha(*s) || b_is_digit(*s) || *s == '_') s++;
	if (*s == '$') return -1;
	*sp = s;
	return 0;
}

static int b_parse_var_ref(const char **sp, int *idx, int *is_string)
{
	const char *s = b_skip_spaces(*sp);
	if (!b_is_alpha(*s)) return -1;
	*idx = b_up(*s) - 'A';
	if (*idx < 0 || *idx > 25) return -1;
	s++;
	while (b_is_alpha(*s) || b_is_digit(*s) || *s == '_') s++;
	*is_string = 0;
	if (*s == '$')
	{
		*is_string = 1;
		s++;
	}
	*sp = s;
	return 0;
}

static int b_array_get(int idx, int pos, int *out)
{
	if (b_rt_arrays == (void *)0 || b_rt_array_sizes == (void *)0 || out == (void *)0) return -1;
	if (idx < 0 || idx > 25) return -1;
	if (b_rt_array_sizes[idx] <= 0) return -1;
	if (pos < 0 || pos >= b_rt_array_sizes[idx]) return -1;
	*out = b_rt_arrays[idx][pos];
	return 0;
}

static int b_array_set(int idx, int pos, int value)
{
	if (b_rt_arrays == (void *)0 || b_rt_array_sizes == (void *)0) return -1;
	if (idx < 0 || idx > 25) return -1;
	if (b_rt_array_sizes[idx] <= 0) return -1;
	if (pos < 0 || pos >= b_rt_array_sizes[idx]) return -1;
	b_rt_arrays[idx][pos] = value;
	return 0;
}

static int b_parse_expr(const char **sp, int vars[26], int *out);
static int b_parse_string_expr(const char **sp, char *out, int out_max);

static int b_parse_value(const char **sp, int vars[26], int *out)
{
	const char *s = b_skip_spaces(*sp);
	if (b_is_alpha(*s))
	{
		int idx;
		int is_string;
		if (b_parse_var_ref(&s, &idx, &is_string) != 0 || is_string) return -1;
		s = b_skip_spaces(s);
		if (*s == '(')
		{
			int pos;
			s++;
			if (b_parse_expr(&s, vars, &pos) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ')') return -1;
			s++;
			if (b_array_get(idx, pos, out) != 0) return -1;
		}
		else
		{
			*out = vars[idx];
		}
		*sp = s;
		return 0;
	}
	return b_parse_int(sp, out);
}

static void b_copy_string(char *dst, const char *src, int max_len)
{
	int i = 0;
	if (max_len <= 0) return;
	while (src[i] != '\0' && i + 1 < max_len)
	{
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static void b_trim_spaces_inplace(char *s)
{
	int start = 0;
	int end = 0;
	int i;
	while (s[end] != '\0') end++;
	while (s[start] == ' ' || s[start] == '\t') start++;
	while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) end--;
	for (i = 0; start + i < end; i++) s[i] = s[start + i];
	s[i] = '\0';
}

static int b_parse_string_literal(const char **sp, char *out, int out_max)
{
	const char *s = b_skip_spaces(*sp);
	int n = 0;
	if (*s != '"' || out == (void *)0 || out_max <= 0) return -1;
	s++;
	while (*s != '\0' && *s != '"')
	{
		if (n + 1 < out_max) out[n++] = *s;
		s++;
	}
	if (*s != '"') return -1;
	out[n] = '\0';
	s++;
	*sp = s;
	return 0;
}

static void b_append_char(char *out, int out_max, int *len, char c)
{
	if (out == (void *)0 || len == (void *)0 || out_max <= 0) return;
	if (*len + 1 < out_max)
	{
		out[*len] = c;
		(*len)++;
		out[*len] = '\0';
	}
}

static void b_append_text(char *out, int out_max, int *len, const char *src)
{
	int i = 0;
	if (src == (void *)0) return;
	while (src[i] != '\0')
	{
		b_append_char(out, out_max, len, src[i]);
		i++;
	}
}

static void b_int_to_text(int v, char *out, int out_max)
{
	char rev[16];
	int n = 0;
	unsigned int uv;
	int p = 0;
	if (out == (void *)0 || out_max <= 0) return;
	out[0] = '\0';
	if (v == 0)
	{
		if (out_max > 1) { out[0] = '0'; out[1] = '\0'; }
		return;
	}
	if (v < 0)
	{
		if (p + 1 < out_max) out[p++] = '-';
		uv = (unsigned int)(-v);
	}
	else uv = (unsigned int)v;
	while (uv > 0 && n < (int)sizeof(rev))
	{
		rev[n++] = (char)('0' + (uv % 10));
		uv /= 10;
	}
	while (n > 0 && p + 1 < out_max) out[p++] = rev[--n];
	out[p] = '\0';
}

static int b_parse_int_from_text(const char *s, int *out)
{
	const char *p = s;
	int sign = 1;
	int any = 0;
	int v = 0;
	if (s == (void *)0 || out == (void *)0) return -1;
	p = b_skip_spaces(p);
	if (*p == '-') { sign = -1; p++; }
	while (b_is_digit(*p))
	{
		v = v * 10 + (*p - '0');
		p++;
		any = 1;
	}
	if (!any) return -1;
	*out = v * sign;
	return 0;
}

static void b_print_int(int v)
{
	char out[16];
	int n = 0;
	unsigned int uv;

	if (v == 0)
	{
		terminal_write("0");
		return;
	}

	if (v < 0)
	{
		terminal_write("-");
		uv = (unsigned int)(-v);
	}
	else uv = (unsigned int)v;

	while (uv > 0 && n < (int)sizeof(out) - 1)
	{
		out[n++] = (char)('0' + (uv % 10));
		uv /= 10;
	}
	while (n > 0)
	{
		char cbuf[2];
		cbuf[0] = out[--n];
		cbuf[1] = '\0';
		terminal_write(cbuf);
	}
}

static int b_parse_expr(const char **sp, int vars[26], int *out);

static int b_name_matches_ci(const char *s, const char *name)
{
	int i = 0;
	while (name[i] != '\0')
	{
		if (b_up(s[i]) != b_up(name[i])) return 0;
		i++;
	}
	if (b_is_ident_tail(s[i])) return 0;
	return 1;
}

static int b_parse_builtin_paren_expr(const char **sp, int vars[26], int *out)
{
	const char *s = b_skip_spaces(*sp);
	if (*s != '(') return -1;
	s++;
	if (b_parse_expr(&s, vars, out) != 0) return -1;
	s = b_skip_spaces(s);
	if (*s != ')') return -1;
	s++;
	*sp = s;
	return 0;
}

static int b_parse_string_expr(const char **sp, char *out, int out_max)
{
	const char *s = b_skip_spaces(*sp);
	int out_len = 0;
	out[0] = '\0';

	for (;;)
	{
		char part[BASIC_MAX_STRING_LEN];
		part[0] = '\0';

		s = b_skip_spaces(s);
		if (*s == '"')
		{
			if (b_parse_string_literal(&s, part, sizeof(part)) != 0) return -1;
		}
		else if (b_is_alpha(*s))
		{
			int idx;
			int is_string;
			const char *sv = s;
			if (b_name_matches_ci(s, "CHR$") && b_skip_spaces(s + 4)[0] == '(')
			{
				int v;
				s = b_skip_spaces(s + 4);
				if (b_parse_builtin_paren_expr(&s, b_rt_vars, &v) != 0) return -1;
				part[0] = (char)(v & 0xFF);
				part[1] = '\0';
			}
			else if (b_name_matches_ci(s, "STR$") && b_skip_spaces(s + 4)[0] == '(')
			{
				int v;
				s = b_skip_spaces(s + 4);
				if (b_parse_builtin_paren_expr(&s, b_rt_vars, &v) != 0) return -1;
				b_int_to_text(v, part, sizeof(part));
			}
			else if (b_name_matches_ci(s, "MID$") && b_skip_spaces(s + 4)[0] == '(')
			{
				char src[BASIC_MAX_STRING_LEN];
				int start_pos, length;
				int src_len, pi;
				s = b_skip_spaces(s + 4);
				if (*s != '(') return -1;
				s++;
				if (b_parse_string_expr(&s, src, sizeof(src)) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s != ',') return -1;
				s++;
				if (b_parse_expr(&s, b_rt_vars, &start_pos) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s == ',')
				{
					s++;
					if (b_parse_expr(&s, b_rt_vars, &length) != 0) return -1;
				}
				else
				{
					length = BASIC_MAX_STRING_LEN;
				}
				s = b_skip_spaces(s);
				if (*s != ')') return -1;
				s++;
				src_len = 0;
				while (src[src_len] != '\0') src_len++;
				if (start_pos < 1) start_pos = 1;
				start_pos--;
				if (length < 0) length = 0;
				pi = 0;
				while (pi < length && start_pos + pi < src_len && pi + 1 < (int)sizeof(part))
				{
					part[pi] = src[start_pos + pi];
					pi++;
				}
				part[pi] = '\0';
			}
			else if (b_name_matches_ci(s, "LEFT$") && b_skip_spaces(s + 5)[0] == '(')
			{
				char src[BASIC_MAX_STRING_LEN];
				int count, src_len, pi;
				s = b_skip_spaces(s + 5);
				if (*s != '(') return -1;
				s++;
				if (b_parse_string_expr(&s, src, sizeof(src)) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s != ',') return -1;
				s++;
				if (b_parse_expr(&s, b_rt_vars, &count) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s != ')') return -1;
				s++;
				src_len = 0;
				while (src[src_len] != '\0') src_len++;
				if (count < 0) count = 0;
				if (count > src_len) count = src_len;
				pi = 0;
				while (pi < count && pi + 1 < (int)sizeof(part))
				{
					part[pi] = src[pi];
					pi++;
				}
				part[pi] = '\0';
			}
			else if (b_name_matches_ci(s, "RIGHT$") && b_skip_spaces(s + 6)[0] == '(')
			{
				char src[BASIC_MAX_STRING_LEN];
				int count, src_len, pi, start;
				s = b_skip_spaces(s + 6);
				if (*s != '(') return -1;
				s++;
				if (b_parse_string_expr(&s, src, sizeof(src)) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s != ',') return -1;
				s++;
				if (b_parse_expr(&s, b_rt_vars, &count) != 0) return -1;
				s = b_skip_spaces(s);
				if (*s != ')') return -1;
				s++;
				src_len = 0;
				while (src[src_len] != '\0') src_len++;
				if (count < 0) count = 0;
				if (count > src_len) count = src_len;
				start = src_len - count;
				pi = 0;
				while (pi < count && pi + 1 < (int)sizeof(part))
				{
					part[pi] = src[start + pi];
					pi++;
				}
				part[pi] = '\0';
			}
			else if (b_parse_var_ref(&sv, &idx, &is_string) == 0 && is_string)
			{
				b_copy_string(part, b_rt_str_vars[idx], sizeof(part));
				s = sv;
			}
			else
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}

		b_append_text(out, out_max, &out_len, part);
		s = b_skip_spaces(s);
		if (*s == '+')
		{
			s++;
			continue;
		}
		break;
	}

	*sp = s;
	return 0;
}

static int b_parse_factor(const char **sp, int vars[26], int *out)
{
	const char *s = b_skip_spaces(*sp);
	int sign = 1;
	int v;

	if (b_name_matches_ci(s, "NOT"))
	{
		s = b_skip_spaces(s + 3);
		if (b_parse_factor(&s, vars, &v) != 0) return -1;
		*out = ~v;
		*sp = s;
		return 0;
	}

	while (*s == '+' || *s == '-')
	{
		if (*s == '-') sign = -sign;
		s = b_skip_spaces(s + 1);
	}

	if (*s == '(')
	{
		s++;
		if (b_parse_expr(&s, vars, &v) != 0) return -1;
		s = b_skip_spaces(s);
		if (*s != ')') return -1;
		s++;
		*out = v * sign;
		*sp = s;
		return 0;
	}

	if (b_is_alpha(*s))
	{
		if (b_name_matches_ci(s, "ABS") && b_skip_spaces(s + 3)[0] == '(')
		{
			s = b_skip_spaces(s + 3);
			if (b_parse_builtin_paren_expr(&s, vars, &v) != 0) return -1;
			if (v < 0) v = -v;
			*out = v * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "RND") && b_skip_spaces(s + 3)[0] == '(')
		{
			int n;
			s = b_skip_spaces(s + 3);
			if (b_parse_builtin_paren_expr(&s, vars, &n) != 0) return -1;
			if (n <= 0) v = 0;
			else
			{
				b_rt_rand_state = b_rt_rand_state * 1664525U + 1013904223U;
				v = (int)(b_rt_rand_state % (unsigned int)n);
			}
			*out = v * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "LEN") && b_skip_spaces(s + 3)[0] == '(')
		{
			char tmp[BASIC_MAX_STRING_LEN];
			int n = 0;
			s = b_skip_spaces(s + 3);
			s = b_skip_spaces(s);
			if (*s != '(') return -1;
			s++;
			if (b_parse_string_expr(&s, tmp, sizeof(tmp)) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ')') return -1;
			s++;
			while (tmp[n] != '\0') n++;
			*out = n * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "VAL") && b_skip_spaces(s + 3)[0] == '(')
		{
			char tmp[BASIC_MAX_STRING_LEN];
			s = b_skip_spaces(s + 3);
			s = b_skip_spaces(s);
			if (*s != '(') return -1;
			s++;
			if (b_parse_string_expr(&s, tmp, sizeof(tmp)) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ')') return -1;
			s++;
			if (b_parse_int_from_text(tmp, &v) != 0) v = 0;
			*out = v * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "ASC") && b_skip_spaces(s + 3)[0] == '(')
		{
			char tmp[BASIC_MAX_STRING_LEN];
			s = b_skip_spaces(s + 3);
			s = b_skip_spaces(s);
			if (*s != '(') return -1;
			s++;
			if (b_parse_string_expr(&s, tmp, sizeof(tmp)) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ')') return -1;
			s++;
			v = (tmp[0] == '\0') ? 0 : (unsigned char)tmp[0];
			*out = v * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "EOF") && b_skip_spaces(s + 3)[0] == '(')
		{
			int fnum;
			s = b_skip_spaces(s + 3);
			if (b_parse_builtin_paren_expr(&s, vars, &fnum) != 0) return -1;
			if (fnum < 1 || fnum > BASIC_MAX_FILES) { v = -1; }
			else
			{
				fnum--;
				v = (!b_files[fnum].active || b_files[fnum].buf_pos >= b_files[fnum].buf_len) ? -1 : 0;
			}
			*out = v * sign;
			*sp = s;
			return 0;
		}
		if (b_name_matches_ci(s, "INSTR") && b_skip_spaces(s + 5)[0] == '(')
		{
			char haystack[BASIC_MAX_STRING_LEN];
			char needle[BASIC_MAX_STRING_LEN];
			int pos = -1;
			int hi, ni;
			s = b_skip_spaces(s + 5);
			if (*s != '(') return -1;
			s++;
			if (b_parse_string_expr(&s, haystack, sizeof(haystack)) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ',') return -1;
			s++;
			if (b_parse_string_expr(&s, needle, sizeof(needle)) != 0) return -1;
			s = b_skip_spaces(s);
			if (*s != ')') return -1;
			s++;
			for (hi = 0; haystack[hi] != '\0'; hi++)
			{
				int match = 1;
				for (ni = 0; needle[ni] != '\0'; ni++)
				{
					if (haystack[hi + ni] != needle[ni]) { match = 0; break; }
				}
				if (match) { pos = hi + 1; break; }
			}
			if (needle[0] == '\0') pos = 1;
			if (pos < 0) pos = 0;
			*out = pos * sign;
			*sp = s;
			return 0;
		}
		if (b_up(s[0]) == 'F' && b_up(s[1]) == 'N' && b_is_alpha(s[2]) && !b_is_ident_tail(s[3]))
		{
			int fn_idx = b_up(s[2]) - 'A';
			int fi;
			s += 3;
			for (fi = 0; fi < b_def_fn_count; fi++)
			{
				if (b_def_fns[fi].fn_letter == fn_idx)
				{
					int arg;
					const char *body;
					int old_val;
					int pv = b_def_fns[fi].param_var;
					if (b_parse_builtin_paren_expr(&s, vars, &arg) != 0) return -1;
					old_val = vars[pv];
					vars[pv] = arg;
					body = b_def_fns[fi].body;
					if (b_parse_expr(&body, vars, &v) != 0)
					{
						vars[pv] = old_val;
						return -1;
					}
					vars[pv] = old_val;
					*out = v * sign;
					*sp = s;
					return 0;
				}
			}
			return -1;
		}
	}

	if (b_parse_value(&s, vars, &v) != 0) return -1;
	*out = v * sign;
	*sp = s;
	return 0;
}

static int b_parse_term(const char **sp, int vars[26], int *out)
{
	const char *s = *sp;
	int lhs;

	if (b_parse_factor(&s, vars, &lhs) != 0) return -1;

	for (;;)
	{
		int rhs;
		s = b_skip_spaces(s);
		if (*s == '*')
		{
			s++;
			if (b_parse_factor(&s, vars, &rhs) != 0) return -1;
			lhs *= rhs;
		}
		else if (*s == '/')
		{
			s++;
			if (b_parse_factor(&s, vars, &rhs) != 0) return -1;
			if (rhs == 0) return -1;
			lhs /= rhs;
		}
		else if (*s == '%')
		{
			s++;
			if (b_parse_factor(&s, vars, &rhs) != 0) return -1;
			if (rhs == 0) return -1;
			lhs %= rhs;
		}
		else break;
	}

	*sp = s;
	*out = lhs;
	return 0;
}

static int b_parse_expr(const char **sp, int vars[26], int *out)
{
	const char *s = *sp;
	int lhs;

	if (b_parse_term(&s, vars, &lhs) != 0) return -1;

	for (;;)
	{
		int rhs;
		s = b_skip_spaces(s);
		if (*s == '+')
		{
			s++;
			if (b_parse_term(&s, vars, &rhs) != 0) return -1;
			lhs += rhs;
		}
		else if (*s == '-')
		{
			s++;
			if (b_parse_term(&s, vars, &rhs) != 0) return -1;
			lhs -= rhs;
		}
		else if (b_name_matches_ci(s, "AND"))
		{
			s = b_skip_spaces(s + 3);
			if (b_parse_term(&s, vars, &rhs) != 0) return -1;
			lhs &= rhs;
		}
		else if (b_name_matches_ci(s, "OR"))
		{
			s = b_skip_spaces(s + 2);
			if (b_parse_term(&s, vars, &rhs) != 0) return -1;
			lhs |= rhs;
		}
		else if (b_name_matches_ci(s, "XOR"))
		{
			s = b_skip_spaces(s + 3);
			if (b_parse_term(&s, vars, &rhs) != 0) return -1;
			lhs ^= rhs;
		}
		else break;
	}

	*sp = s;
	*out = lhs;
	return 0;
}

static void b_reset_vars(int vars[26])
{
	int i;
	for (i = 0; i < 26; i++) vars[i] = 0;
}

static void b_reset_string_vars(char str_vars[26][BASIC_MAX_STRING_LEN])
{
	int i;
	for (i = 0; i < 26; i++) str_vars[i][0] = '\0';
}

static void b_reset_arrays(int arrays[26][BASIC_ARRAY_MAX], int sizes[26])
{
	int i, j;
	for (i = 0; i < 26; i++)
	{
		sizes[i] = 0;
		for (j = 0; j < BASIC_ARRAY_MAX; j++) arrays[i][j] = 0;
	}
}

static const char *b_statement_start(const char *line)
{
	const char *s = b_skip_spaces(line);
	if (b_is_digit(*s) || *s == '-')
	{
		int dummy;
		const char *t = s;
		if (b_parse_int(&t, &dummy) == 0 && b_is_space(*t)) s = b_skip_spaces(t);
	}
	return s;
}

static int b_find_matching_next(struct basic_line lines[], int line_count, int for_pc)
{
	int i;
	int depth = 0;
	for (i = for_pc + 1; i < line_count; i++)
	{
		const char *s = b_statement_start(lines[i].text);
		if (b_starts_with_ci(s, "FOR")) depth++;
		else if (b_starts_with_ci(s, "NEXT"))
		{
			if (depth == 0) return i;
			depth--;
		}
	}
	return -1;
}

static int b_find_matching_wend(struct basic_line lines[], int line_count, int while_pc)
{
	int i;
	int depth = 0;
	for (i = while_pc + 1; i < line_count; i++)
	{
		const char *s = b_statement_start(lines[i].text);
		if (b_starts_with_ci(s, "WHILE")) depth++;
		else if (b_starts_with_ci(s, "WEND"))
		{
			if (depth == 0) return i;
			depth--;
		}
	}
	return -1;
}

static int b_find_label(struct basic_line lines[], int line_count, int label)
{
	int i;
	for (i = 0; i < line_count; i++)
	{
		if (lines[i].label == label) return i;
	}
	return -1;
}

static int b_parse_label_list_pick(const char **sp, int pick_index, int *label_out)
{
	const char *s = b_skip_spaces(*sp);
	int idx = 1;
	int label;
	if (pick_index <= 0 || label_out == (void *)0) return -1;

	for (;;)
	{
		if (b_parse_int(&s, &label) != 0) return -1;
		if (idx == pick_index)
		{
			*label_out = label;
			*sp = s;
			return 0;
		}

		s = b_skip_spaces(s);
		if (*s != ',') return -1;
		s++;
		idx++;
	}
}

static int b_data_reset_cursor(struct basic_line lines[], int line_count, int label, int *line_idx_out, int *offset_out)
{
	int i = 0;
	if (line_idx_out == (void *)0 || offset_out == (void *)0) return -1;
	if (label >= 0)
	{
		i = b_find_label(lines, line_count, label);
		if (i < 0) return -1;
	}
	*line_idx_out = i;
	*offset_out = 0;
	return 0;
}

static int b_read_next_data_item(
	struct basic_line lines[],
	int line_count,
	int *line_idx,
	int *offset,
	char *out,
	int out_max,
	int *is_string)
{
	int li;
	if (line_idx == (void *)0 || offset == (void *)0 || out == (void *)0 || out_max <= 0 || is_string == (void *)0) return -1;

	li = *line_idx;
	while (li >= 0 && li < line_count)
	{
		const char *s = b_statement_start(lines[li].text);
		const char *data_start;
		const char *item;
		int off;
		int n = 0;

		if (!b_starts_with_ci(s, "DATA"))
		{
			li++;
			*offset = 0;
			continue;
		}

		data_start = b_skip_spaces(s + 4);
		off = *offset;
		item = data_start;
		while (off > 0 && *item != '\0') { item++; off--; }

		while (*item == ' ' || *item == '\t' || *item == ',') item++;
		if (*item == '\0')
		{
			li++;
			*offset = 0;
			continue;
		}

		if (*item == '"')
		{
			*is_string = 1;
			item++;
			while (*item != '\0' && *item != '"')
			{
				if (n + 1 < out_max) out[n++] = *item;
				item++;
			}
			if (*item == '"') item++;
		}
		else
		{
			const char *end;
			*is_string = 0;
			while (*item != '\0' && *item != ',')
			{
				if (n + 1 < out_max) out[n++] = *item;
				item++;
			}
			end = out + n;
			while (n > 0 && (end[-1] == ' ' || end[-1] == '\t')) { n--; end--; }
		}
		out[n] = '\0';

		while (*item == ' ' || *item == '\t') item++;
		if (*item == ',') item++;

		*line_idx = li;
		*offset = (int)(item - data_start);
		return 0;
	}

	return -1;
}

static void b_print_uint(unsigned int value)
{
	char rev[16];
	int n = 0;
	if (value == 0)
	{
		terminal_write("0");
		return;
	}
	while (value > 0 && n < (int)sizeof(rev))
	{
		rev[n++] = (char)('0' + (value % 10));
		value /= 10;
	}
	while (n > 0)
	{
		char c[2];
		c[0] = rev[--n];
		c[1] = '\0';
		terminal_write(c);
	}
}

static void b_runtime_error(struct basic_line lines[], int line_count, int pc, const char *message)
{
	if (message == (void *)0) message = "runtime error";
	terminal_write("[basic ");
	if (pc >= 0 && pc < line_count)
	{
		if (lines[pc].label >= 0)
		{
			terminal_write("L");
			b_print_uint((unsigned int)lines[pc].label);
		}
		else
		{
			terminal_write("PC");
			b_print_uint((unsigned int)pc);
		}
	}
	else
	{
		terminal_write("PC?");
	}
	terminal_write("] ");
	terminal_write_line(message);
	if (pc >= 0 && pc < line_count)
	{
		terminal_write("  -> ");
		terminal_write_line(lines[pc].text);
	}
}

int basic_run(char *program_text)
{
	struct basic_line lines[BASIC_MAX_LINES];
	int vars[26];
	char str_vars[26][BASIC_MAX_STRING_LEN];
	int arrays[26][BASIC_ARRAY_MAX];
	int array_sizes[26];
	int call_stack[BASIC_MAX_CALL_DEPTH];
	struct basic_for_frame for_stack[BASIC_MAX_FOR_DEPTH];
	struct basic_while_frame while_stack[BASIC_MAX_WHILE_DEPTH];
	int call_sp = 0;
	int for_sp = 0;
	int while_sp = 0;
	int data_line = 0;
	int data_offset = 0;
	int line_count = 0;
	int pc = 0;
	int steps = 0;
	int i;
	char *p;

#define B_RT_ERR(msg) do { b_runtime_error(lines, line_count, pc, (msg)); b_err_flag = 1; goto b_cleanup; } while (0)

	int b_err_flag = 0;

	if (program_text == (void *)0) return -1;

	b_reset_vars(vars);
	b_reset_string_vars(str_vars);
	b_reset_arrays(arrays, array_sizes);
	b_data_reset_cursor(lines, 0, -1, &data_line, &data_offset);
	b_rt_vars = vars;
	b_rt_str_vars = str_vars;
	b_rt_arrays = arrays;
	b_rt_array_sizes = array_sizes;
	b_def_fn_count = 0;
	{
		int fi;
		for (fi = 0; fi < BASIC_MAX_FILES; fi++)
			b_files[fi].active = 0;
	}

	p = program_text;
	while (*p != '\0' && line_count < BASIC_MAX_LINES)
	{
		char *line = p;
		int label = -1;
		const char *q;
		int parsed;

		while (*p != '\0' && *p != '\n' && *p != '\r') p++;
		if (*p == '\r') { *p = '\0'; p++; if (*p == '\n') p++; }
		else if (*p == '\n') { *p = '\0'; p++; }

		q = line;
		q = b_skip_spaces(q);
		parsed = b_parse_int(&q, &label);
		if (parsed != 0) label = -1;
		else if (!b_is_space(*q)) label = -1;

		lines[line_count].label = label;
		lines[line_count].text = line;
		line_count++;
	}

	while (pc >= 0 && pc < line_count)
	{
		const char *s = lines[pc].text;
		int jump_to = -1;

		steps++;
		if (steps > BASIC_MAX_STEPS)
		{
			B_RT_ERR("stopped: too many steps");
		}

		s = b_skip_spaces(s);
		if (*s == '\0') { pc++; continue; }

		if (b_is_digit(*s) || *s == '-')
		{
			int dummy;
			const char *tmp = s;
			if (b_parse_int(&tmp, &dummy) == 0 && b_is_space(*tmp)) s = b_skip_spaces(tmp);
		}

		if (*s == '\0' || *s == '#') { pc++; continue; }
		if (b_starts_with_ci(s, "REM")) { pc++; continue; }
		if (b_starts_with_ci(s, "END") || b_starts_with_ci(s, "STOP")) return 0;
		if (b_starts_with_ci(s, "CLS") || b_starts_with_ci(s, "CLEAR"))
		{
			screen_clear();
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "LIST"))
		{
			for (i = 0; i < line_count; i++) terminal_write_line(lines[i].text);
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "RUN"))
		{
			b_reset_vars(vars);
			b_reset_string_vars(str_vars);
			b_reset_arrays(arrays, array_sizes);
			call_sp = 0;
			for_sp = 0;
			b_data_reset_cursor(lines, line_count, -1, &data_line, &data_offset);
			pc = 0;
			continue;
		}

		if (b_starts_with_ci(s, "PRINT"))
		{
			char trailing_sep = '\0';
			s = b_skip_spaces(s + 5);
			if (*s == '\0')
			{
				terminal_write("\n");
			}
			else
			{
				int keep_printing = 1;
				while (keep_printing)
				{
					int string_printed = 0;
					char str_part[BASIC_MAX_STRING_LEN];
					s = b_skip_spaces(s);
					if (b_name_matches_ci(s, "TAB") && b_skip_spaces(s + 3)[0] == '(')
					{
						int spaces;
						s = b_skip_spaces(s + 3);
						if (b_parse_builtin_paren_expr(&s, vars, &spaces) != 0)
						{
							B_RT_ERR("PRINT TAB parse error");
						}
						while (spaces-- > 0) terminal_write(" ");
					}
					else if (b_name_matches_ci(s, "SPC") && b_skip_spaces(s + 3)[0] == '(')
					{
						int spaces;
						s = b_skip_spaces(s + 3);
						if (b_parse_builtin_paren_expr(&s, vars, &spaces) != 0)
						{
							B_RT_ERR("PRINT SPC parse error");
						}
						while (spaces-- > 0) terminal_write(" ");
					}
					else
					{
						const char *ss = s;
						if (b_parse_string_expr(&ss, str_part, sizeof(str_part)) == 0)
						{
							terminal_write(str_part);
							s = ss;
							string_printed = 1;
						}
						else
						{
							int v;
							if (b_parse_expr(&s, vars, &v) != 0)
							{
								B_RT_ERR("PRINT parse error");
							}
							b_print_int(v);
						}
					}
					if (string_printed) s = b_skip_spaces(s);

					s = b_skip_spaces(s);
					if (*s == ',' || *s == ';')
					{
						trailing_sep = *s;
						if (*s == ',') terminal_write(" ");
						s++;
						s = b_skip_spaces(s);
						if (*s == '\0') break;
						continue;
					}
					trailing_sep = '\0';
					keep_printing = 0;
				}
				if (trailing_sep != ';') terminal_write("\n");
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "DATA"))
		{
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "READ"))
		{
			s = b_skip_spaces(s + 4);
			for (;;)
			{
				int idx;
				int is_string_var;
				char token[BASIC_MAX_STRING_LEN];
				int token_is_string = 0;

				if (b_parse_var_ref(&s, &idx, &is_string_var) != 0)
				{
					B_RT_ERR("READ var parse error");
				}

				if (b_read_next_data_item(lines, line_count, &data_line, &data_offset, token, sizeof(token), &token_is_string) != 0)
				{
					B_RT_ERR("READ out of DATA");
				}

				if (is_string_var)
				{
					b_copy_string(str_vars[idx], token, BASIC_MAX_STRING_LEN);
				}
				else
				{
					const char *tp = token;
					int v;
					if (token_is_string)
					{
						B_RT_ERR("READ type mismatch (string -> number)");
					}
					if (b_parse_expr(&tp, vars, &v) != 0)
					{
						B_RT_ERR("READ numeric parse error");
					}
					tp = b_skip_spaces(tp);
					if (*tp != '\0')
					{
						B_RT_ERR("READ numeric trailing text");
					}
					vars[idx] = v;
				}

				s = b_skip_spaces(s);
				if (*s != ',') break;
				s++;
				s = b_skip_spaces(s);
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "RESTORE"))
		{
			int label = -1;
			s = b_skip_spaces(s + 7);
			if (*s != '\0')
			{
				if (b_parse_int(&s, &label) != 0)
				{
					B_RT_ERR("RESTORE label parse error");
				}
			}
			if (b_data_reset_cursor(lines, line_count, label, &data_line, &data_offset) != 0)
			{
				B_RT_ERR("RESTORE label not found");
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "DIM"))
		{
			s = b_skip_spaces(s + 3);
			for (;;)
			{
				int idx;
				int size;
				if (b_parse_var(&s, &idx) != 0)
				{
					B_RT_ERR("DIM var parse error");
				}
				s = b_skip_spaces(s);
				if (*s != '(')
				{
					B_RT_ERR("DIM missing '('");
				}
				s++;
				if (b_parse_expr(&s, vars, &size) != 0)
				{
					B_RT_ERR("DIM size parse error");
				}
				s = b_skip_spaces(s);
				if (*s != ')')
				{
					B_RT_ERR("DIM missing ')'");
				}
				s++;
				if (size < 0 || size >= BASIC_ARRAY_MAX)
				{
					B_RT_ERR("DIM size out of range");
				}
				b_rt_array_sizes[idx] = size + 1;
				{
					int j;
					for (j = 0; j < b_rt_array_sizes[idx]; j++) b_rt_arrays[idx][j] = 0;
				}
				s = b_skip_spaces(s);
				if (*s != ',') break;
				s++;
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "LET"))
		{
			int idx;
			int is_string;
			int v;
			int arr_index = -1;
			char sbuf[BASIC_MAX_STRING_LEN];
			s = b_skip_spaces(s + 3);
			if (b_parse_var_ref(&s, &idx, &is_string) != 0)
			{
				B_RT_ERR("LET var error");
			}
			s = b_skip_spaces(s);
			if (!is_string)
			{
				const char *t = b_skip_spaces(s);
				if (*t == '(')
				{
					t++;
					if (b_parse_expr(&t, vars, &arr_index) != 0)
					{
						B_RT_ERR("LET array index parse error");
					}
					t = b_skip_spaces(t);
					if (*t != ')')
					{
						B_RT_ERR("LET array missing ')'");
					}
					t++;
					s = t;
					s = b_skip_spaces(s);
					if (*s != '=')
					{
						B_RT_ERR("LET missing '='");
					}
				}
			}
			s = b_skip_spaces(s);
			if (*s != '=')
			{
				B_RT_ERR("LET missing '='");
			}
			s++;
			if (is_string)
			{
				s = b_skip_spaces(s);
				if (*s == '"')
				{
					if (b_parse_string_literal(&s, sbuf, sizeof(sbuf)) != 0)
					{
						B_RT_ERR("LET string literal error");
					}
				}
				else if (b_is_alpha(*s))
				{
					int src_idx;
					int src_is_string;
					const char *sv = s;
					if (b_parse_var_ref(&sv, &src_idx, &src_is_string) != 0 || !src_is_string)
					{
						B_RT_ERR("LET string value error");
					}
					b_copy_string(sbuf, str_vars[src_idx], sizeof(sbuf));
					s = sv;
				}
				else
				{
					B_RT_ERR("LET string value error");
				}
				b_copy_string(str_vars[idx], sbuf, BASIC_MAX_STRING_LEN);
				pc++;
				continue;
			}
			if (b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("LET value error");
			}
			if (arr_index >= 0)
			{
				if (b_array_set(idx, arr_index, v) != 0)
				{
					B_RT_ERR("LET array index out of range");
				}
			}
			else vars[idx] = v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "INPUT"))
		{
			char prompt[BASIC_MAX_STRING_LEN];
			char inbuf[BASIC_MAX_STRING_LEN];
			int idx;
			int is_string;
			prompt[0] = '\0';
			s = b_skip_spaces(s + 5);
			if (*s == '"')
			{
				if (b_parse_string_literal(&s, prompt, sizeof(prompt)) != 0)
				{
					B_RT_ERR("INPUT prompt error");
				}
				s = b_skip_spaces(s);
				if (*s == ';' || *s == ',') s++;
			}

			if (b_parse_var_ref(&s, &idx, &is_string) != 0)
			{
				B_RT_ERR("INPUT var error");
			}

			if (prompt[0] != '\0') terminal_write(prompt);
			else terminal_write("? ");

			if (terminal_read_line(inbuf, sizeof(inbuf)) != 0)
			{
				B_RT_ERR("INPUT read error");
			}
			b_trim_spaces_inplace(inbuf);

			if (is_string)
			{
				const char *sp = inbuf;
				char tmp[BASIC_MAX_STRING_LEN];
				if (inbuf[0] == '"')
				{
					if (b_parse_string_literal(&sp, tmp, sizeof(tmp)) != 0)
					{
						B_RT_ERR("INPUT string parse error");
					}
					b_copy_string(str_vars[idx], tmp, BASIC_MAX_STRING_LEN);
				}
				else
				{
					b_copy_string(str_vars[idx], inbuf, BASIC_MAX_STRING_LEN);
				}
			}
			else
			{
				const char *sp = inbuf;
				int v;
				if (b_parse_expr(&sp, vars, &v) != 0)
				{
					B_RT_ERR("INPUT number parse error");
				}
				sp = b_skip_spaces(sp);
				if (*sp != '\0')
				{
					B_RT_ERR("INPUT trailing text");
				}
				vars[idx] = v;
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "FOR"))
		{
			int idx;
			int start_v;
			int end_v;
			int step_v = 1;
			int next_pc;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0)
			{
				B_RT_ERR("FOR var error");
			}
			s = b_skip_spaces(s);
			if (*s != '=')
			{
				B_RT_ERR("FOR missing '='");
			}
			s++;
			if (b_parse_expr(&s, vars, &start_v) != 0)
			{
				B_RT_ERR("FOR start value error");
			}
			s = b_skip_spaces(s);
			if (!b_starts_with_ci(s, "TO"))
			{
				B_RT_ERR("FOR missing TO");
			}
			s = b_skip_spaces(s + 2);
			if (b_parse_expr(&s, vars, &end_v) != 0)
			{
				B_RT_ERR("FOR end value error");
			}
			s = b_skip_spaces(s);
			if (b_starts_with_ci(s, "STEP"))
			{
				s = b_skip_spaces(s + 4);
				if (b_parse_expr(&s, vars, &step_v) != 0)
				{
					B_RT_ERR("FOR STEP parse error");
				}
			}
			if (step_v == 0)
			{
				B_RT_ERR("FOR STEP cannot be zero");
			}

			vars[idx] = start_v;
			if ((step_v > 0 && vars[idx] > end_v) || (step_v < 0 && vars[idx] < end_v))
			{
				next_pc = b_find_matching_next(lines, line_count, pc);
				if (next_pc < 0)
				{
					B_RT_ERR("FOR missing NEXT");
				}
				pc = next_pc + 1;
				continue;
			}

			if (for_sp >= BASIC_MAX_FOR_DEPTH)
			{
				B_RT_ERR("FOR stack overflow");
			}
			for_stack[for_sp].var_idx = idx;
			for_stack[for_sp].end_value = end_v;
			for_stack[for_sp].step_value = step_v;
			for_stack[for_sp].body_pc = pc + 1;
			for_sp++;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "NEXT"))
		{
			int idx = -1;
			int has_var = 0;
			int top;
			s = b_skip_spaces(s + 4);
			if (b_is_alpha(*s))
			{
				if (b_parse_var(&s, &idx) != 0)
				{
					B_RT_ERR("NEXT var parse error");
				}
				has_var = 1;
			}
			if (for_sp <= 0)
			{
				B_RT_ERR("NEXT without FOR");
			}
			top = for_sp - 1;
			if (has_var && for_stack[top].var_idx != idx)
			{
				B_RT_ERR("NEXT var mismatch");
			}

			vars[for_stack[top].var_idx] += for_stack[top].step_value;
			if ((for_stack[top].step_value > 0 && vars[for_stack[top].var_idx] <= for_stack[top].end_value) ||
				(for_stack[top].step_value < 0 && vars[for_stack[top].var_idx] >= for_stack[top].end_value))
			{
				pc = for_stack[top].body_pc;
			}
			else
			{
				for_sp--;
				pc++;
			}
			continue;
		}

		if (b_starts_with_ci(s, "WHILE") && (s[5] == ' ' || s[5] == '\t'))
		{
			int cond;
			s = b_skip_spaces(s + 5);
			if (b_parse_expr(&s, vars, &cond) != 0)
			{
				B_RT_ERR("WHILE condition error");
			}
			if (cond)
			{
				if (while_sp >= BASIC_MAX_WHILE_DEPTH)
				{
					B_RT_ERR("WHILE nested too deep");
				}
				while_stack[while_sp].while_pc = pc;
				while_sp++;
				pc++;
			}
			else
			{
				int wend_pc = b_find_matching_wend(lines, line_count, pc);
				if (wend_pc < 0)
				{
					B_RT_ERR("WHILE without WEND");
				}
				pc = wend_pc + 1;
			}
			continue;
		}

		if (b_starts_with_ci(s, "WEND"))
		{
			int cond;
			const char *ws;
			if (while_sp <= 0)
			{
				B_RT_ERR("WEND without WHILE");
			}
			/* Re-evaluate condition at the WHILE line */
			ws = b_statement_start(lines[while_stack[while_sp - 1].while_pc].text);
			ws = b_skip_spaces(ws + 5);
			if (b_parse_expr(&ws, vars, &cond) != 0)
			{
				B_RT_ERR("WEND condition re-eval error");
			}
			if (cond)
			{
				pc = while_stack[while_sp - 1].while_pc + 1;
			}
			else
			{
				while_sp--;
				pc++;
			}
			continue;
		}

		if (b_starts_with_ci(s, "DEF") && (s[3] == ' ' || s[3] == '\t'))
		{
			int fn_idx, pvar;
			s = b_skip_spaces(s + 3);
			if (!(s[0] == 'F' || s[0] == 'f') || !(s[1] == 'N' || s[1] == 'n') || !b_is_alpha(s[2]))
			{
				B_RT_ERR("DEF FN syntax error");
			}
			fn_idx = (s[2] >= 'a' && s[2] <= 'z') ? s[2] - 'a' : s[2] - 'A';
			s += 3;
			s = b_skip_spaces(s);
			if (*s != '(')
			{
				B_RT_ERR("DEF FN missing (");
			}
			s++;
			s = b_skip_spaces(s);
			if (!b_is_alpha(*s))
			{
				B_RT_ERR("DEF FN missing param");
			}
			pvar = (*s >= 'a' && *s <= 'z') ? *s - 'a' : *s - 'A';
			s++;
			s = b_skip_spaces(s);
			if (*s != ')')
			{
				B_RT_ERR("DEF FN missing )");
			}
			s++;
			s = b_skip_spaces(s);
			if (*s != '=')
			{
				B_RT_ERR("DEF FN missing =");
			}
			s++;
			s = b_skip_spaces(s);
			/* Find or replace existing definition for this function letter */
			{
				int fi, found = -1;
				for (fi = 0; fi < b_def_fn_count; fi++)
				{
					if (b_def_fns[fi].fn_letter == fn_idx)
					{
						found = fi;
						break;
					}
				}
				if (found < 0)
				{
					if (b_def_fn_count >= BASIC_MAX_DEF_FN)
					{
						B_RT_ERR("Too many DEF FN");
					}
					found = b_def_fn_count++;
				}
				b_def_fns[found].fn_letter = fn_idx;
				b_def_fns[found].param_var = pvar;
				{
					int bi = 0;
					while (*s && bi + 1 < (int)sizeof(b_def_fns[found].body))
					{
						b_def_fns[found].body[bi++] = *s++;
					}
					b_def_fns[found].body[bi] = '\0';
				}
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "ADD"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("ADD parse error");
			}
			vars[idx] += v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "SUB"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("SUB parse error");
			}
			vars[idx] -= v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "MUL"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("MUL parse error");
			}
			vars[idx] *= v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "DIV"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("DIV parse error");
			}
			if (v == 0)
			{
				B_RT_ERR("DIV by zero");
			}
			vars[idx] /= v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "MOD"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_expr(&s, vars, &v) != 0)
			{
				B_RT_ERR("MOD parse error");
			}
			if (v == 0)
			{
				B_RT_ERR("MOD by zero");
			}
			vars[idx] %= v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "GOTO"))
		{
			int label;
			s = b_skip_spaces(s + 4);
			if (b_parse_int(&s, &label) != 0)
			{
				B_RT_ERR("GOTO parse error");
			}
			jump_to = b_find_label(lines, line_count, label);
			if (jump_to < 0)
			{
				B_RT_ERR("GOTO label not found");
			}
			pc = jump_to;
			continue;
		}

		if (b_starts_with_ci(s, "ON"))
		{
			int selector;
			int target_label;
			int do_gosub = 0;
			s = b_skip_spaces(s + 2);
			if (b_parse_expr(&s, vars, &selector) != 0)
			{
				B_RT_ERR("ON selector parse error");
			}
			s = b_skip_spaces(s);
			if (b_starts_with_ci(s, "GOTO"))
			{
				s = b_skip_spaces(s + 4);
				do_gosub = 0;
			}
			else if (b_starts_with_ci(s, "GOSUB"))
			{
				s = b_skip_spaces(s + 5);
				do_gosub = 1;
			}
			else
			{
				B_RT_ERR("ON missing GOTO/GOSUB");
			}

			if (selector <= 0)
			{
				pc++;
				continue;
			}
			if (b_parse_label_list_pick(&s, selector, &target_label) != 0)
			{
				pc++;
				continue;
			}

			jump_to = b_find_label(lines, line_count, target_label);
			if (jump_to < 0)
			{
				B_RT_ERR("ON target label not found");
			}
			if (do_gosub)
			{
				if (call_sp >= BASIC_MAX_CALL_DEPTH)
				{
					B_RT_ERR("ON GOSUB stack overflow");
				}
				call_stack[call_sp++] = pc + 1;
			}
			pc = jump_to;
			continue;
		}

		if (b_starts_with_ci(s, "GOSUB"))
		{
			int label;
			s = b_skip_spaces(s + 5);
			if (b_parse_int(&s, &label) != 0)
			{
				B_RT_ERR("GOSUB parse error");
			}
			jump_to = b_find_label(lines, line_count, label);
			if (jump_to < 0)
			{
				B_RT_ERR("GOSUB label not found");
			}
			if (call_sp >= BASIC_MAX_CALL_DEPTH)
			{
				B_RT_ERR("GOSUB stack overflow");
			}
			call_stack[call_sp++] = pc + 1;
			pc = jump_to;
			continue;
		}

		if (b_starts_with_ci(s, "RETURN"))
		{
			if (call_sp <= 0)
			{
				B_RT_ERR("RETURN without GOSUB");
			}
			pc = call_stack[--call_sp];
			continue;
		}

		if (b_starts_with_ci(s, "IF"))
		{
			int lhs, rhs;
			int cond = 0;
			enum b_cmp_op cmp = B_CMP_EQ;
			int label;
			s = b_skip_spaces(s + 2);
			if (b_parse_expr(&s, vars, &lhs) != 0)
			{
				B_RT_ERR("IF lhs error");
			}
			s = b_skip_spaces(s);
			if (s[0] == '=' && s[1] == '=') { cmp = B_CMP_EQ; s += 2; }
			else if (s[0] == '=' ) { cmp = B_CMP_EQ; s += 1; }
			else if (s[0] == '!' && s[1] == '=') { cmp = B_CMP_NE; s += 2; }
			else if (s[0] == '<' && s[1] == '>') { cmp = B_CMP_NE; s += 2; }
			else if (s[0] == '>' && s[1] == '=') { cmp = B_CMP_GE; s += 2; }
			else if (s[0] == '<' && s[1] == '=') { cmp = B_CMP_LE; s += 2; }
			else if (s[0] == '>') { cmp = B_CMP_GT; s += 1; }
			else if (s[0] == '<') { cmp = B_CMP_LT; s += 1; }
			else
			{
				B_RT_ERR("IF operator error");
			}
			if (b_parse_expr(&s, vars, &rhs) != 0)
			{
				B_RT_ERR("IF rhs error");
			}
			s = b_skip_spaces(s);
			if (!b_starts_with_ci(s, "THEN"))
			{
				B_RT_ERR("IF missing THEN");
			}
			s = b_skip_spaces(s + 4);
			if (b_parse_int(&s, &label) != 0)
			{
				B_RT_ERR("IF THEN label error");
			}

			if (cmp == B_CMP_EQ) cond = (lhs == rhs);
			else if (cmp == B_CMP_NE) cond = (lhs != rhs);
			else if (cmp == B_CMP_LT) cond = (lhs < rhs);
			else if (cmp == B_CMP_LE) cond = (lhs <= rhs);
			else if (cmp == B_CMP_GT) cond = (lhs > rhs);
			else if (cmp == B_CMP_GE) cond = (lhs >= rhs);

			if (cond)
			{
				jump_to = b_find_label(lines, line_count, label);
				if (jump_to < 0)
				{
					B_RT_ERR("IF target label not found");
				}
				pc = jump_to;
			}
			else
			{
				pc++;
			}
			continue;
		}

		/* Allow classic BASIC implicit assignment without LET (e.g. A = 1, P$ = "X"). */

		if (b_starts_with_ci(s, "OPEN"))
		{
			char fname[64];
			int fmode = -1;
			int fnum;
			const char *tp;
			s = b_skip_spaces(s + 4);
			if (b_parse_string_literal(&s, fname, sizeof(fname)) != 0)
			{
				B_RT_ERR("OPEN filename error");
			}
			s = b_skip_spaces(s);
			if (b_starts_with_ci(s, "FOR"))
			{
				s = b_skip_spaces(s + 3);
				if (b_starts_with_ci(s, "INPUT"))
				{
					fmode = 0;
					s += 5;
				}
				else if (b_starts_with_ci(s, "OUTPUT"))
				{
					fmode = 1;
					s += 6;
				}
				else if (b_starts_with_ci(s, "APPEND"))
				{
					fmode = 2;
					s += 6;
				}
				else
				{
					B_RT_ERR("OPEN mode error");
				}
			}
			else
			{
				fmode = 0;
			}
			s = b_skip_spaces(s);
			if (b_starts_with_ci(s, "AS"))
			{
				s = b_skip_spaces(s + 2);
			}
			if (*s != '#')
			{
				B_RT_ERR("OPEN missing #");
			}
			s++;
			tp = s;
			if (b_parse_expr(&tp, vars, &fnum) != 0)
			{
				B_RT_ERR("OPEN file number error");
			}
			if (fnum < 1 || fnum > BASIC_MAX_FILES)
			{
				B_RT_ERR("OPEN file number out of range (1-4)");
			}
			fnum--;
			if (b_files[fnum].active)
			{
				B_RT_ERR("OPEN file already open");
			}
			b_files[fnum].active = 1;
			b_files[fnum].mode = fmode;
			{
				int ci = 0;
				while (fname[ci] && ci + 1 < (int)sizeof(b_files[fnum].path))
				{
					b_files[fnum].path[ci] = fname[ci];
					ci++;
				}
				b_files[fnum].path[ci] = '\0';
			}
			b_files[fnum].buf_len = 0;
			b_files[fnum].buf_pos = 0;
			if (fmode == 0)
			{
				unsigned long fsize = 0;
				if (fs_read_file(fname, (unsigned char *)b_files[fnum].buf, BASIC_FILE_BUF - 1, &fsize) != 0)
				{
					b_files[fnum].active = 0;
					B_RT_ERR("OPEN cannot read file");
				}
				b_files[fnum].buf[fsize] = '\0';
				b_files[fnum].buf_len = (int)fsize;
			}
			else if (fmode == 2)
			{
				unsigned long fsize = 0;
				if (fs_read_file(fname, (unsigned char *)b_files[fnum].buf, BASIC_FILE_BUF - 1, &fsize) == 0)
				{
					b_files[fnum].buf[fsize] = '\0';
					b_files[fnum].buf_len = (int)fsize;
				}
				b_files[fnum].buf_pos = b_files[fnum].buf_len;
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "CLOSE"))
		{
			int fnum;
			const char *tp;
			s = b_skip_spaces(s + 5);
			if (*s != '#')
			{
				B_RT_ERR("CLOSE missing #");
			}
			s++;
			tp = s;
			if (b_parse_expr(&tp, vars, &fnum) != 0)
			{
				B_RT_ERR("CLOSE file number error");
			}
			if (fnum < 1 || fnum > BASIC_MAX_FILES)
			{
				B_RT_ERR("CLOSE file number out of range");
			}
			fnum--;
			if (!b_files[fnum].active)
			{
				B_RT_ERR("CLOSE file not open");
			}
			if (b_files[fnum].mode == 1 || b_files[fnum].mode == 2)
			{
				b_files[fnum].buf[b_files[fnum].buf_len] = '\0';
				fs_write_file(b_files[fnum].path, (unsigned char *)b_files[fnum].buf, (unsigned long)b_files[fnum].buf_len);
			}
			b_files[fnum].active = 0;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "PRINT") && b_skip_spaces(s + 5)[0] == '#')
		{
			int fnum;
			const char *tp;
			s = b_skip_spaces(s + 5);
			s++;
			tp = s;
			if (b_parse_expr(&tp, vars, &fnum) != 0)
			{
				B_RT_ERR("PRINT# file number error");
			}
			s = tp;
			if (fnum < 1 || fnum > BASIC_MAX_FILES)
			{
				B_RT_ERR("PRINT# file number out of range");
			}
			fnum--;
			if (!b_files[fnum].active || b_files[fnum].mode == 0)
			{
				B_RT_ERR("PRINT# file not open for output");
			}
			s = b_skip_spaces(s);
			if (*s == ',') s++;
			s = b_skip_spaces(s);
			{
				char out_buf[256];
				int out_len = 0;
				while (*s && out_len + 1 < (int)sizeof(out_buf))
				{
					if (*s == '"')
					{
						char lit[BASIC_MAX_STRING_LEN];
						if (b_parse_string_literal(&s, lit, sizeof(lit)) != 0) break;
						{
							int li = 0;
							while (lit[li] && out_len + 1 < (int)sizeof(out_buf))
								out_buf[out_len++] = lit[li++];
						}
					}
					else if (b_is_alpha(*s) && (s[1] == '$' || (b_is_alpha(s[0]) && !b_is_ident_tail(s[1]))))
					{
						int idx, is_str;
						const char *sv = s;
						if (b_parse_var_ref(&sv, &idx, &is_str) == 0 && is_str)
						{
							int li = 0;
							s = sv;
							while (str_vars[idx][li] && out_len + 1 < (int)sizeof(out_buf))
								out_buf[out_len++] = str_vars[idx][li++];
						}
						else
						{
							int v;
							char nb[16];
							if (b_parse_expr(&s, vars, &v) != 0) break;
							b_int_to_text(v, nb, sizeof(nb));
							{
								int li = 0;
								while (nb[li] && out_len + 1 < (int)sizeof(out_buf))
									out_buf[out_len++] = nb[li++];
							}
						}
					}
					else
					{
						int v;
						char nb[16];
						if (b_parse_expr(&s, vars, &v) != 0) break;
						b_int_to_text(v, nb, sizeof(nb));
						{
							int li = 0;
							while (nb[li] && out_len + 1 < (int)sizeof(out_buf))
								out_buf[out_len++] = nb[li++];
						}
					}
					s = b_skip_spaces(s);
					if (*s == ';' || *s == ',')
					{
						if (*s == ',') out_buf[out_len++] = '\t';
						s++;
						s = b_skip_spaces(s);
					}
					else break;
				}
				out_buf[out_len++] = '\n';
				{
					int wi = 0;
					while (wi < out_len && b_files[fnum].buf_len + 1 < BASIC_FILE_BUF)
					{
						b_files[fnum].buf[b_files[fnum].buf_len++] = out_buf[wi++];
					}
				}
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "INPUT") && b_skip_spaces(s + 5)[0] == '#')
		{
			int fnum, idx, is_str;
			const char *tp;
			s = b_skip_spaces(s + 5);
			s++;
			tp = s;
			if (b_parse_expr(&tp, vars, &fnum) != 0)
			{
				B_RT_ERR("INPUT# file number error");
			}
			s = tp;
			if (fnum < 1 || fnum > BASIC_MAX_FILES)
			{
				B_RT_ERR("INPUT# file number out of range");
			}
			fnum--;
			if (!b_files[fnum].active || b_files[fnum].mode != 0)
			{
				B_RT_ERR("INPUT# file not open for input");
			}
			s = b_skip_spaces(s);
			if (*s == ',') s++;
			s = b_skip_spaces(s);
			if (b_parse_var_ref(&s, &idx, &is_str) != 0)
			{
				B_RT_ERR("INPUT# variable error");
			}
			{
				char line_buf[BASIC_MAX_STRING_LEN];
				int li = 0;
				while (b_files[fnum].buf_pos < b_files[fnum].buf_len &&
					   b_files[fnum].buf[b_files[fnum].buf_pos] != '\n' &&
					   li + 1 < (int)sizeof(line_buf))
				{
					char c = b_files[fnum].buf[b_files[fnum].buf_pos++];
					if (c != '\r') line_buf[li++] = c;
				}
				line_buf[li] = '\0';
				if (b_files[fnum].buf_pos < b_files[fnum].buf_len &&
					b_files[fnum].buf[b_files[fnum].buf_pos] == '\n')
				{
					b_files[fnum].buf_pos++;
				}
				if (is_str)
				{
					b_copy_string(str_vars[idx], line_buf, BASIC_MAX_STRING_LEN);
				}
				else
				{
					const char *lp = line_buf;
					int v;
					if (b_parse_expr(&lp, vars, &v) != 0)
					{
						B_RT_ERR("INPUT# numeric parse error");
					}
					vars[idx] = v;
				}
			}
			pc++;
			continue;
		}

		if (b_is_alpha(*s))
		{
			const char *as = s;
			int idx;
			int is_string;
			int v;
			int arr_index = -1;
			char sbuf[BASIC_MAX_STRING_LEN];

			if (b_parse_var_ref(&as, &idx, &is_string) == 0)
			{
				as = b_skip_spaces(as);
				if (!is_string && *as == '(')
				{
					as++;
					if (b_parse_expr(&as, vars, &arr_index) != 0)
					{
						B_RT_ERR("assignment array index parse error");
					}
					as = b_skip_spaces(as);
					if (*as != ')')
					{
						B_RT_ERR("assignment array missing ')'");
					}
					as++;
					as = b_skip_spaces(as);
				}

				if (*as == '=')
				{
					as++;
					if (is_string)
					{
						as = b_skip_spaces(as);
						if (*as == '"')
						{
							if (b_parse_string_literal(&as, sbuf, sizeof(sbuf)) != 0)
							{
								B_RT_ERR("assignment string literal error");
							}
						}
						else if (b_is_alpha(*as))
						{
							int src_idx;
							int src_is_string;
							const char *sv = as;
							if (b_parse_var_ref(&sv, &src_idx, &src_is_string) != 0 || !src_is_string)
							{
								B_RT_ERR("assignment string value error");
							}
							b_copy_string(sbuf, str_vars[src_idx], sizeof(sbuf));
							as = sv;
						}
						else
						{
							B_RT_ERR("assignment string value error");
						}
						b_copy_string(str_vars[idx], sbuf, BASIC_MAX_STRING_LEN);
						pc++;
						continue;
					}

					if (b_parse_expr(&as, vars, &v) != 0)
					{
						B_RT_ERR("assignment value error");
					}
					if (arr_index >= 0)
					{
						if (b_array_set(idx, arr_index, v) != 0)
						{
							B_RT_ERR("assignment array index out of range");
						}
					}
					else vars[idx] = v;
					pc++;
					continue;
				}
			}
		}

		B_RT_ERR("unknown statement");
	}

	#undef B_RT_ERR
b_cleanup:
	/* Close any files left open */
	{
		int fi;
		for (fi = 0; fi < BASIC_MAX_FILES; fi++)
		{
			if (b_files[fi].active)
			{
				if (b_files[fi].mode == 1 || b_files[fi].mode == 2)
				{
					b_files[fi].buf[b_files[fi].buf_len] = '\0';
					fs_write_file(b_files[fi].path, (unsigned char *)b_files[fi].buf, (unsigned long)b_files[fi].buf_len);
				}
				b_files[fi].active = 0;
			}
		}
	}
	return b_err_flag ? -1 : 0;
}
