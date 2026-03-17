#include "basic.h"
#include "terminal.h"

#define BASIC_MAX_LINES 256
#define BASIC_MAX_STEPS 20000

struct basic_line
{
	int label;
	char *text;
};

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
	*sp = s;
	return 0;
}

static int b_parse_value(const char **sp, int vars[26], int *out)
{
	const char *s = b_skip_spaces(*sp);
	if (b_is_alpha(*s))
	{
		int idx;
		if (b_parse_var(&s, &idx) != 0) return -1;
		*out = vars[idx];
		*sp = s;
		return 0;
	}
	return b_parse_int(sp, out);
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

int basic_run(char *program_text)
{
	struct basic_line lines[BASIC_MAX_LINES];
	int vars[26];
	int line_count = 0;
	int pc = 0;
	int steps = 0;
	int i;
	char *p;

	if (program_text == (void *)0) return -1;

	for (i = 0; i < 26; i++) vars[i] = 0;

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
			terminal_write_line("[basic] stopped: too many steps");
			return -1;
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
		if (b_starts_with_ci(s, "END")) return 0;

		if (b_starts_with_ci(s, "PRINT"))
		{
			s = b_skip_spaces(s + 5);
			if (*s == '"')
			{
				s++;
				while (*s != '\0' && *s != '"')
				{
					char ch[2];
					ch[0] = *s;
					ch[1] = '\0';
					terminal_write(ch);
					s++;
				}
				terminal_write("\n");
			}
			else
			{
				int v;
				if (b_parse_value(&s, vars, &v) != 0)
				{
					terminal_write_line("[basic] PRINT parse error");
					return -1;
				}
				{
					char out[16];
					int n = 0;
					unsigned int uv;
					if (v == 0)
					{
						terminal_write_line("0");
					}
					else
					{
						if (v < 0)
						{
							terminal_write("-");
							uv = (unsigned int)(-v);
						}
						else uv = (unsigned int)v;
						while (uv > 0 && n < (int)sizeof(out)-1)
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
						terminal_write("\n");
					}
				}
			}
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "LET"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0)
			{
				terminal_write_line("[basic] LET var error");
				return -1;
			}
			s = b_skip_spaces(s);
			if (*s != '=')
			{
				terminal_write_line("[basic] LET missing '='");
				return -1;
			}
			s++;
			if (b_parse_value(&s, vars, &v) != 0)
			{
				terminal_write_line("[basic] LET value error");
				return -1;
			}
			vars[idx] = v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "ADD"))
		{
			int idx;
			int v;
			s = b_skip_spaces(s + 3);
			if (b_parse_var(&s, &idx) != 0 || b_parse_value(&s, vars, &v) != 0)
			{
				terminal_write_line("[basic] ADD parse error");
				return -1;
			}
			vars[idx] += v;
			pc++;
			continue;
		}

		if (b_starts_with_ci(s, "GOTO"))
		{
			int label;
			s = b_skip_spaces(s + 4);
			if (b_parse_int(&s, &label) != 0)
			{
				terminal_write_line("[basic] GOTO parse error");
				return -1;
			}
			jump_to = b_find_label(lines, line_count, label);
			if (jump_to < 0)
			{
				terminal_write_line("[basic] GOTO label not found");
				return -1;
			}
			pc = jump_to;
			continue;
		}

		if (b_starts_with_ci(s, "IF"))
		{
			int lhs, rhs;
			int cond = 0;
			char op0, op1;
			int label;
			s = b_skip_spaces(s + 2);
			if (b_parse_value(&s, vars, &lhs) != 0)
			{
				terminal_write_line("[basic] IF lhs error");
				return -1;
			}
			s = b_skip_spaces(s);
			op0 = *s;
			op1 = *(s + 1);
			if ((op0 == '=' && op1 == '=') || (op0 == '!' && op1 == '=') || (op0 == '>' && op1 == '=') || (op0 == '<' && op1 == '=')) s += 2;
			else if (op0 == '>' || op0 == '<') s += 1;
			else
			{
				terminal_write_line("[basic] IF operator error");
				return -1;
			}
			if (b_parse_value(&s, vars, &rhs) != 0)
			{
				terminal_write_line("[basic] IF rhs error");
				return -1;
			}
			s = b_skip_spaces(s);
			if (!b_starts_with_ci(s, "THEN"))
			{
				terminal_write_line("[basic] IF missing THEN");
				return -1;
			}
			s = b_skip_spaces(s + 4);
			if (b_parse_int(&s, &label) != 0)
			{
				terminal_write_line("[basic] IF THEN label error");
				return -1;
			}

			if (op0 == '=' && op1 == '=') cond = (lhs == rhs);
			else if (op0 == '!' && op1 == '=') cond = (lhs != rhs);
			else if (op0 == '>') cond = (lhs > rhs);
			else if (op0 == '<') cond = (lhs < rhs);
			else if (op0 == '>' && op1 == '=') cond = (lhs >= rhs);
			else if (op0 == '<' && op1 == '=') cond = (lhs <= rhs);

			if (cond)
			{
				jump_to = b_find_label(lines, line_count, label);
				if (jump_to < 0)
				{
					terminal_write_line("[basic] IF target label not found");
					return -1;
				}
				pc = jump_to;
			}
			else
			{
				pc++;
			}
			continue;
		}

		terminal_write_line("[basic] unknown statement");
		return -1;
	}

	return 0;
}
