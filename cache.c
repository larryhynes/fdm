/* $Id$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <fnmatch.h>
#include <search.h>
#include <stdlib.h>
#include <string.h>

#include "fdm.h"

struct cache *cache_reference; /* XXX */

int	cache_sortcmp(const void *, const void *);
int	cache_searchcmp(const void *, const void *);
int	cache_matchcmp(const void *, const void *);

int
cache_sortcmp(const void *ptr1, const void *ptr2)
{
	const struct cacheent	*ce1 = ptr1;
	const struct cacheent	*ce2 = ptr2;
	const char		*key1, *key2;

	if (!ce1->used && ce2->used)
		return (1);
	else if (ce1->used && !ce2->used)
		return (-1);
	else if (!ce1->used && !ce2->used)
		return (0);

	key1 = CACHE_KEY(cache_reference, ce1);
	key2 = CACHE_KEY(cache_reference, ce2);
	return (strcmp(key1, key2));
}

int
cache_searchcmp(const void *ptr1, const void *ptr2)
{
	const struct cacheent	*ce = ptr2;
	const char		*key1 = ptr1;
	const char		*key2;

	if (!ce->used)
		return (-1);

	key2 = CACHE_KEY(cache_reference, ce);
	return (strcmp(key1, key2));
}

int
cache_matchcmp(const void *ptr1, const void *ptr2)
{
	const struct cacheent	*ce = ptr2;
	const char		*pattern = ptr1;
	const char		*key;

	if (!ce->used)
		return (-1);

	key = CACHE_KEY(cache_reference, ce);
	return (fnmatch(pattern, key, 0) == FNM_NOMATCH);
}
   
void
cache_create(struct cache **cc)
{
	*cc = xmalloc(sizeof **cc);
	cache_clear(cc);
}

void
cache_clear(struct cache **cc)
{
	(*cc)->entries = CACHEENTRIES;

	(*cc)->str_size = CACHEBUFFER;
	(*cc)->str_used = 0;

	*cc = xrealloc(*cc, 1, CACHE_SIZE(*cc));
	memset(CACHE_ENTRY(*cc, 0), 0, CACHE_ENTRYSIZE(*cc));
}

void
cache_destroy(struct cache **cc)
{
	xfree(*cc);
	*cc = NULL;
}

void
cache_dump(struct cache *cc, const char *prefix, void (*p)(const char *, ...))
{
	struct cacheent *ce;
	u_int		 i;

	for (i = 0; i < cc->entries; i++) {
		ce = CACHE_ENTRY(cc, i);
		if (!ce->used) {
			p("%s: %u: unused", prefix, i);
			continue;
		}
		p("%s: %u: %s: %s", 
		    prefix, i, CACHE_KEY(cc, ce), CACHE_VALUE(cc, ce));
	}
}

void
cache_add(struct cache **cc, const char *key, const char *value)
{
	size_t		 size, keylen, valuelen;
	u_int		 entries;
	struct cacheent *ce;

	keylen = strlen(key) + 1;
	valuelen = strlen(value) + 1;

	ce = CACHE_ENTRY(*cc, 0);
	size = (*cc)->str_size;
	while ((*cc)->str_size - (*cc)->str_used < keylen + valuelen) {
		if (CACHE_SIZE(*cc) > SIZE_MAX / 2)
			fatalx("cache_add: size too large");
		(*cc)->str_size *= 2;
	}
	if (size != (*cc)->str_size) {
		*cc = xrealloc(*cc, 1, CACHE_SIZE(*cc));
		memmove(CACHE_ENTRY(*cc, 0), ce, CACHE_ENTRYSIZE(*cc));
	}

	ce = cache_find(*cc, key);
	if (ce == NULL) {
		/* all of the unused entries are at the end */
		ce = CACHE_ENTRY(*cc, (*cc)->entries - 1);
		if (ce->used) {
			/* allocate some more */
			if ((*cc)->entries > UINT_MAX / 2)
				fatalx("cache_add: entries too large");
			entries = (*cc)->entries;
			
			(*cc)->entries *= 2;
			*cc = xrealloc(*cc, 1, CACHE_SIZE(*cc));
			memset(CACHE_ENTRY(*cc, entries), 0,
			    CACHE_ENTRYSIZE(*cc) / 2);
			ce = CACHE_ENTRY(*cc, (*cc)->entries - 1);
		}
		ce->key = (*cc)->str_used;
		memcpy(CACHE_KEY(*cc, ce), key, keylen);
		(*cc)->str_used += keylen;
	}
	ce->value = (*cc)->str_used;
	memcpy(CACHE_VALUE(*cc, ce), value, valuelen);
	(*cc)->str_used += valuelen;

	if (ce->used) {
		/* if replacing an existing key, there is no need to resort */
		return;
	}
	ce->used = 1;

	cache_reference = *cc;
	qsort(CACHE_ENTRY(*cc, 0), (*cc)->entries, sizeof *ce, cache_sortcmp);
}

void
cache_delete(struct cache **cc, struct cacheent *ce)
{
	ce->used = 0;

	cache_reference = *cc;
	qsort(CACHE_ENTRY(*cc, 0), (*cc)->entries, sizeof *ce, cache_sortcmp);
}

struct cacheent *
cache_find(struct cache *cc, const char *key)
{
	struct cacheent	*ce;

	cache_reference = cc;
	ce = bsearch(key,
	    CACHE_ENTRY(cc, 0), cc->entries, sizeof *ce, cache_searchcmp);
	return (ce);
}

struct cacheent *
cache_match(struct cache *cc, const char *pattern)
{
	struct cacheent	*ce;
	size_t		 n = cc->entries;

	cache_reference = cc;
	ce = lfind(pattern, CACHE_ENTRY(cc, 0), &n, sizeof *ce, cache_matchcmp);
	return (ce);
}
