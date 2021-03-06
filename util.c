/* See LICENSE for license details. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

/* trim whitespace from start and end of str */
void
trim(char *s)
{
	char *p = s;
	size_t len;

	if (s == NULL)
		return;

	len = strlen(s);

	for (; isspace(p[len-1]); p[--len] = 0);
	for (; *p && isspace(*p); p++, len--);

	memmove(s, p, len + 1);
}

/* replace embedded escaped newlines with actual newlines */
char *
fix_newlines(char *s)
{
	char *t = s;

	while ((t = strstr(t, "\\n")) != NULL) {
		t[0] = '\n';
		t++;
		memmove(t, t + 1, strlen(t + 1) + 1);
	}

	return s;
}

void *
xreallocarray(void *o, size_t n, size_t s)
{
	void *new;

	if (!(new = reallocarray(o, n, s)))
		die("reallocarray()\n");

	return new;
}
