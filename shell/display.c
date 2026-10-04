#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "psxterm/shell.h"

/* Decode without relying on the daemon's locale. Invalid bytes stay visible. */
static size_t
decode_utf8(const unsigned char *p, uint32_t *code)
{
    size_t length;
    uint32_t value;

    if(*p < 0x80) {
        *code = *p;
        return 1;
    }
    if(*p >= 0xc2 && *p <= 0xdf) {
        length = 2;
        value = *p & 0x1f;
    } else if(*p >= 0xe0 && *p <= 0xef) {
        length = 3;
        value = *p & 0x0f;
    } else if(*p >= 0xf0 && *p <= 0xf4) {
        length = 4;
        value = *p & 0x07;
    } else {
        return 0;
    }
    for(size_t i = 1; i < length; i++) {
        if((p[i] & 0xc0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (p[i] & 0x3f);
    }
    if((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
       value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
        return 0;
    }
    *code = value;
    return length;
}

static size_t
cell_width(uint32_t code)
{
    if((code >= 0x0300 && code <= 0x036f) ||
       (code >= 0x1ab0 && code <= 0x1aff) ||
       (code >= 0x1dc0 && code <= 0x1dff) ||
       (code >= 0x20d0 && code <= 0x20ff) ||
       (code >= 0xfe00 && code <= 0xfe0f) ||
       (code >= 0xfe20 && code <= 0xfe2f) ||
       (code >= 0xe0100 && code <= 0xe01ef)) {
        return 0;
    }
    return (code >= 0x1100 && code <= 0x115f) || code == 0x2329 ||
                   code == 0x232a ||
                   (code >= 0x2e80 && code <= 0xa4cf && code != 0x303f) ||
                   (code >= 0xac00 && code <= 0xd7a3) ||
                   (code >= 0xf900 && code <= 0xfaff) ||
                   (code >= 0xfe10 && code <= 0xfe19) ||
                   (code >= 0xfe30 && code <= 0xfe6f) ||
                   (code >= 0xff01 && code <= 0xff60) ||
                   (code >= 0xffe0 && code <= 0xffe6) ||
                   (code >= 0x1f300 && code <= 0x1faff) ||
                   (code >= 0x20000 && code <= 0x3fffd)
               ? 2
               : 1;
}

char *
psh_display_text(const char *text, bool quote, size_t *width)
{
    static const char hex[] = "0123456789abcdef";
    size_t length = strlen(text);
    size_t used = 0;
    size_t cells = 0;
    bool quoted =
        quote && (!length || strpbrk(text, " \t\n\r'\"\\;|&()<>") != NULL);
    char *out;

    if(length > (SIZE_MAX - 3) / 4 || !(out = malloc(length * 4 + 3))) {
        return NULL;
    }
    if(quoted) {
        out[used++] = '"';
        cells++;
    }
    for(const unsigned char *p = (const unsigned char *)text; *p;) {
        uint32_t code;
        size_t n = decode_utf8(p, &code);

        if(!n || code < 0x20 || (code >= 0x7f && code <= 0x9f) ||
           (code >= 0x200b && code <= 0x200f) ||
           (code >= 0x2028 && code <= 0x202e) ||
           (code >= 0x2060 && code <= 0x206f)) {
            /* Escape each byte of controls, including UTF-8 encoded C1. */
            if(!n)
                n = 1;
            for(size_t i = 0; i < n; i++) {
                out[used++] = '\\';
                out[used++] = 'x';
                out[used++] = hex[p[i] >> 4];
                out[used++] = hex[p[i] & 15];
                cells += 4;
            }
        } else if(*p == '\\' || (quoted && *p == '"')) {
            out[used++] = '\\';
            out[used++] = (char)*p;
            cells += 2;
        } else {
            memcpy(out + used, p, n);
            used += n;
            cells += cell_width(code);
        }
        p += n;
    }
    if(quoted) {
        out[used++] = '"';
        cells++;
    }
    out[used] = '\0';
    if(width)
        *width = cells;
    return out;
}

int
psh_no_arguments(psx_session_t *session, int argc, char **argv)
{
    if(argc == 2 && strcmp(argv[1], "--help") == 0) {
        return psh_out(session, "%s\n", psh_command_usage(argv[0])) < 0 ? 1 : 0;
    }
    if(argc == 1 || (argc == 2 && strcmp(argv[1], "--") == 0)) {
        return -1;
    }
    psh_err(
        session,
        "%s: unexpected argument '%s'\nTry '%s --help' for more information.\n",
        argv[0], argv[1], argv[0]);
    return 2;
}
