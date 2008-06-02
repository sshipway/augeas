/*
 * internal.c: internal data structures and helpers
 *
 * Copyright (C) 2007, 2008 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Author: David Lutterkort <dlutter@redhat.com>
 */

#include <ctype.h>
#include <stdarg.h>

#include "internal.h"
#include "memory.h"

#ifndef MIN
# define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* Cap file reads somwhat arbitrarily at 32 MB */
#define MAX_READ_LEN (32*1024*1024)

int pathjoin(char **path, int nseg, ...) {
    va_list ap;

    va_start(ap, nseg);
    for (int i=0; i < nseg; i++) {
        const char *seg = va_arg(ap, const char *);
        if (seg == NULL)
            seg = "()";
        int len = strlen(seg) + 1;

        if (*path != NULL) {
            len += strlen(*path) + 1;
            if (REALLOC_N(*path, len) == -1) {
                FREE(*path);
                return -1;
            }
            if (strlen(*path) == 0 || (*path)[strlen(*path)-1] != SEP)
                strcat(*path, "/");
            if (seg[0] == SEP)
                seg += 1;
            strcat(*path, seg);
        } else {
            if ((*path = malloc(len)) == NULL)
                return -1;
            strcpy(*path, seg);
        }
    }
    va_end(ap);
    return 0;
}

/* Like gnulib's fread_file, but read no more than the specified maximum
   number of bytes.  If the length of the input is <= max_len, and
   upon error while reading that data, it works just like fread_file.

   Taken verbatim from libvirt's util.c
*/

static char *
fread_file_lim (FILE *stream, size_t max_len, size_t *length)
{
    char *buf = NULL;
    size_t alloc = 0;
    size_t size = 0;
    int save_errno;

    for (;;) {
        size_t count;
        size_t requested;

        if (size + BUFSIZ + 1 > alloc) {
            char *new_buf;

            alloc += alloc / 2;
            if (alloc < size + BUFSIZ + 1)
                alloc = size + BUFSIZ + 1;

            new_buf = realloc (buf, alloc);
            if (!new_buf) {
                save_errno = errno;
                break;
            }

            buf = new_buf;
        }

        /* Ensure that (size + requested <= max_len); */
        requested = MIN (size < max_len ? max_len - size : 0,
                         alloc - size - 1);
        count = fread (buf + size, 1, requested, stream);
        size += count;

        if (count != requested || requested == 0) {
            save_errno = errno;
            if (ferror (stream))
                break;
            buf[size] = '\0';
            *length = size;
            return buf;
        }
    }

    free (buf);
    errno = save_errno;
    return NULL;
}

char* read_file(const char *path) {
    FILE *fp = fopen(path, "r");
    char *result;
    size_t len;

    if (!fp)
        return NULL;

    result = fread_file_lim(fp, MAX_READ_LEN, &len);
    fclose (fp);

    if (result != NULL
        && len <= MAX_READ_LEN
        && (int) len == len)
        return result;

    free(result);
    return NULL;
}

/*
 * Escape/unescape of string literals
 */
static const char *const escape_chars    = "\"\a\b\t\n\v\f\r\\";
static const char *const escape_names = "\"abtnvfr\\";

char *unescape(const char *s, int len) {
    size_t size;
    const char *n;
    char *result, *t;
    int i;

    if (len < 0 || len > strlen(s))
        len = strlen(s);

    size = 0;
    for (i=0; i < len; i++, size++)
        if (s[i] == '\\' && strchr(escape_names, s[i+1]) != NULL) {
            i += 1;
        }

    CALLOC(result, size + 1);
    for (i = 0, t = result; i < len; i++, size++) {
        if (s[i] == '\\' && (n = strchr(escape_names, s[i+1])) != NULL) {
            *t++ = escape_chars[n - escape_names];
            i += 1;
        } else {
            *t++ = s[i];
        }
    }
    return result;
}

char *escape(const char *text, int cnt) {

    int len = 0;
    char *esc = NULL, *e;

    if (cnt < 0 || cnt > strlen(text))
        cnt = strlen(text);

    for (int i=0; i < cnt; i++) {
        if (text[i] && (strchr(escape_chars, text[i]) != NULL))
            len += 2;  /* Escaped as '\x' */
        else if (! isprint(text[i]))
            len += 4;  /* Escaped as '\ooo' */
        else
            len += 1;
    }
    CALLOC(esc, len+1);
    e = esc;
    for (int i=0; i < cnt; i++) {
        char *p;
        if (text[i] && ((p = strchr(escape_chars, text[i])) != NULL)) {
            *e++ = '\\';
            *e++ = escape_names[p - escape_chars];
        } else if (! isprint(text[i])) {
            sprintf(e, "\\%03o", text[i]);
            e += 4;
        } else {
            *e++ = text[i];
        }
    }
    return esc;
}

int print_chars(FILE *out, const char *text, int cnt) {
    int total = 0;
    char *esc;

    if (text == NULL) {
        fprintf(out, "nil");
        return 3;
    }
    if (cnt < 0)
        cnt = strlen(text);

    esc = escape(text, cnt);
    total = strlen(esc);
    if (out != NULL)
        fprintf(out, "%s", esc);
    free(esc);

    return total;
}

char *format_pos(const char *text, int pos) {
    static const int window = 28;
    char *buf = NULL, *left = NULL, *right = NULL;
    int before = pos;
    int llen, rlen;
    int r;

    if (before > window)
        before = window;
    left = escape(text + pos - before, before);
    if (left == NULL)
        goto done;
    right = escape(text + pos, window);
    if (right == NULL)
        goto done;

    llen = strlen(left);
    rlen = strlen(right);
    if (llen < window && rlen < window) {
        r = asprintf(&buf, "%*s%s|=|%s%-*s\n", window - llen, "<", left,
                     right, window - rlen, ">");
    } else if (strlen(left) < window) {
        r = asprintf(&buf, "%*s%s|=|%s>\n", window - llen, "<", left, right);
    } else if (strlen(right) < window) {
        r = asprintf(&buf, "<%s|=|%s%-*s\n", left, right, window - rlen, ">");
    } else {
        r = asprintf(&buf, "<%s|=|%s>\n", left, right);
    }
    if (r < 0) {
        buf = NULL;
    }

 done:
    free(left);
    free(right);
    return buf;
}

void print_pos(FILE *out, const char *text, int pos) {
    char *format = format_pos(text, pos);

    if (format != NULL) {
        fputs(format, out);
        FREE(format);
    }
}

int init_memstream(struct memstream *ms) {
    MEMZERO(ms, 1);
#if HAVE_OPEN_MEMSTREAM
    ms->stream = open_memstream(&(ms->buf), &(ms->size));
    return ms->stream == NULL ? -1 : 0;
#else
    ms->stream = tmpfile();
    if (ms->stream == NULL) {
        return -1;
    }
    return 0;
#endif
}

int close_memstream(struct memstream *ms) {
#if !HAVE_OPEN_MEMSTREAM
    rewind(ms->stream);
    ms->buf = fread_file_lim(ms->stream, MAX_READ_LEN, &(ms->size));
#endif
    if (fclose(ms->stream) == EOF) {
        FREE(ms->buf);
        ms->size = 0;
        return -1;
    }
    return 0;
}



/*
 * Local variables:
 *  indent-tabs-mode: nil
 *  c-indent-level: 4
 *  c-basic-offset: 4
 *  tab-width: 4
 * End:
 */
