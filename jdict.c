/* See LICENSE for license details. */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "arg.h"
#include "util.h"
#include "yomidict.h"

#include "config.h"

#define YOMI_TOKS_PER_ENT 10
#define YOMI_TOK_DELTA (YOMI_TOKS_PER_ENT * 100)

typedef struct {
	char *term;
	char **defs;
	size_t ndefs;
} DictEnt;

char *argv0;

static void
cleanup(char **dicts, char **terms)
{
	if (dicts != default_dicts)
		free(dicts);
	free(terms);

	dicts = NULL;
	terms = NULL;
}

static void
usage(void)
{
	die("usage: %s [-d path] term ...\n", argv0);
}

/* takes a token of type YOMI_ENTRY and creates a DictEnt */
static DictEnt *
make_ent(YomiTok *toks, size_t ntoks, char *data)
{
	size_t i;
	DictEnt *d;
	YomiTok *tstr, *tdefs;

	if (toks[0].type != YOMI_ENTRY)
		return NULL;

	/* FIXME: hacky but works */
	/* definition array = YOMI_ENTRY tok + 6 */
	if (ntoks - 6 < 0)
		return NULL;
	tdefs = toks + 6;
	/* term = YOMI_ENTRY tok + 1 */
	tstr = toks + 1;

	d = xreallocarray(NULL, 1, sizeof(DictEnt));
	d->term = strndup(data + tstr->start, tstr->end - tstr->start);
	d->ndefs = tdefs->len;
	d->defs = xreallocarray(NULL, d->ndefs, sizeof(char *));
	for (i = 1; i <= d->ndefs; i++)
		d->defs[i-1] = strndup(data + (tdefs + i)->start,
		                       (tdefs + i)->end - (tdefs + i)->start);

	return d;
}

static DictEnt *
parse_term_bank(DictEnt *ents, size_t *nents, const char *tbank, size_t stride)
{
	int r, ntoks, fd;
	size_t i, flen;
	char *data;
	YomiTok *toks = NULL;
	YomiParser p;
	DictEnt *e;

	/* FIXME: these need to be checked for errors */
	fd = open(tbank, O_RDONLY);
	flen = lseek(fd, 0, SEEK_END);
	data = mmap(NULL, flen, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);

	/* allocate tokens */
	ntoks = stride * YOMI_TOKS_PER_ENT + 1;
	if ((ntoks - 1) / YOMI_TOKS_PER_ENT != stride)
		die("stride multiplication overflowed: %s\n", tbank);
	toks = xreallocarray(toks, ntoks, sizeof(YomiTok));

	yomi_init(&p);
	while ((r = yomi_parse(&p, toks, ntoks, data, flen)) < 0) {
		switch (r) {
		case YOMI_ERROR_NOMEM:
			/* allocate more mem and try again */
			if (ntoks + YOMI_TOK_DELTA < 0)
				die("too many toks: %s\n", tbank);
			ntoks += YOMI_TOK_DELTA;
			toks = xreallocarray(toks, ntoks, sizeof(YomiTok));
			break;
		case YOMI_ERROR_INVAL: /* FALLTRHOUGH */
		case YOMI_ERROR_MALFO:
			munmap(data, flen);
			free(toks);
			return NULL;
		}
	}

	ents = xreallocarray(ents, (*nents) + r/YOMI_TOKS_PER_ENT, sizeof(DictEnt));
	for (i = 0; i < r; i++) {
		if (toks[i].type == YOMI_ENTRY) {
			e = make_ent(&toks[i], r - i, data);
			if (e == NULL)
				return NULL;
			memcpy(&ents[(*nents)++], e, sizeof(DictEnt));
		}
	}
	munmap(data, flen);
	free(toks);

	return ents;
}

static DictEnt *
make_dict(const char *path, size_t stride, size_t *nents)
{
	char tbank[PATH_MAX];
	size_t i, nbanks = 0;
	DIR *dir;
	struct dirent *dent;
	DictEnt *dict = NULL;

	if (!(dir = opendir(path)))
		die("opendir(): failed to open: %s\n", path);

	/* count term banks in path */
	while ((dent = readdir(dir)) != NULL)
		if (dent->d_type == DT_REG)
			nbanks++;
	/* remove index.json from count */
	nbanks--;

	closedir(dir);

	for (i = 1; i <= nbanks; i++) {
		snprintf(tbank, sizeof(tbank), "%s/term_bank_%d.json", path, (int)i);
		dict = parse_term_bank(dict, nents, tbank, stride);
		if (dict == NULL)
			return NULL;
	}

	return dict;
}

static int
entcmp(const void *va, const void *vb)
{
	const DictEnt *a = va, *b = vb;
	return strcmp(a->term, b->term);
}


static DictEnt *
find_ent(const char *term, DictEnt *ents, size_t nents)
{
	int r;

	if (nents == 0)
		return NULL;

	r = strcmp(term, ents[nents/2].term);
	if (r == 0)
		return &ents[nents/2];
	else if (r < 0)
		return find_ent(term, ents, nents/2);

	if (nents % 2)
		return find_ent(term, &ents[nents/2 + 1], nents/2);
	else
		return find_ent(term, &ents[nents/2 + 1], nents/2 - 1);
}

static char *
fix_newlines(char *str)
{
	char *t = str;

	while ((t = strstr(t, "\\n")) != NULL) {
		t[0] = '\n';
		t++;
		memmove(t, t + 1, strlen(t + 1) + 1);
	}

	return str;
}

static void
print_ent(DictEnt *ent)
{
	size_t i;
	for (i = 0; i < ent->ndefs; i++)
		printf("%s\n", fix_newlines(ent->defs[i]));
}

static int
find_and_print_defs(char **terms, size_t nterms, char **dicts, size_t ndicts)
{
	char path[PATH_MAX - 18];
	size_t i, j, k;
	size_t nents;
	DictEnt *ent, *ents;

	for (i = 0; i < ndicts; i++) {
		snprintf(path, LEN(path), "%s/%s", prefix, dicts[i]);
		nents = 0;
		ents = make_dict(path, DICT_STRIDE, &nents);
		if (ents == NULL)
			return -1;
		qsort(ents, nents, sizeof(DictEnt), entcmp);

		printf("%s\n", dicts[i]);
		for (j = 0; j < nterms; j++) {
			ent = find_ent(terms[j], ents, nents);
			if (ent == NULL) {
				printf("term not found: %s\n", terms[j]);
				return -1;
			}
			print_ent(ent);
		}

		for (j = 0; j < nents; j++) {
			for (k = 0; k < ents[j].ndefs; k++)
				free(ents[j].defs[k]);
			free(ents[j].defs);
			free(ents[j].term);
		}
		free(ents);
	}
	return 0;
}

int
main(int argc, char *argv[])
{
	char **dicts = NULL, **terms = NULL;
	size_t ndicts = 0, nterms = 0;
	int i;

	argv0 = argv[0];

	ARGBEGIN {
	case 'd':
		dicts = xreallocarray(dicts, ++ndicts, sizeof(char *));
		dicts[0] = EARGF(usage());
		break;
	default:
		usage();
	} ARGEND

	if (ndicts == 0) {
		dicts = default_dicts;
		ndicts = LEN(default_dicts);
	}

	/* remaining argv elements are terms to search for */
	for (i = 0; argc && *argv; argv++, i++, argc--) {
		terms = xreallocarray(terms, ++nterms, sizeof(char *));
		terms[i] = *argv;
	}

	if (nterms == 0) {
		cleanup(dicts, terms);
		usage();
	}

	find_and_print_defs(terms, nterms, dicts, ndicts);

	cleanup(dicts, terms);

	return 0;
}
