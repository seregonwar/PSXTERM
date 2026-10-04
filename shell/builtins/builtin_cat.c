#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "psxterm/shell.h"

struct cat_output {
    psx_session_t *session;
    uint8_t bytes[4096];
    size_t used;
    bool failed;
};

static void
cat_flush(struct cat_output *out)
{
    if(out->used && !out->failed &&
       psx_session_emit(out->session, PTTY_MSG_STDOUT, out->bytes, out->used) <
           0) {
        out->failed = true;
    }
    out->used = 0;
}

static void
cat_byte(struct cat_output *out, uint8_t byte)
{
    if(out->failed)
        return;
    if(out->used == sizeof(out->bytes))
        cat_flush(out);
    if(!out->failed)
        out->bytes[out->used++] = byte;
}

static void
cat_visible(struct cat_output *out, uint8_t byte)
{
    if(byte >= 128) {
        cat_byte(out, 'M');
        cat_byte(out, '-');
        byte -= 128;
    }
    if(byte < 32 || byte == 127) {
        cat_byte(out, '^');
        cat_byte(out, byte == 127 ? '?' : byte + 64);
    } else {
        cat_byte(out, byte);
    }
}

int
psh_builtin_cat(psx_session_t *session, int argc, char **argv)
{
    bool number = false, nonblank = false, squeeze = false;
    bool ends = false, tabs = false, visible = false, ended = false;
    bool start = true, pending_cr = false;
    unsigned blank_run = 0;
    uintmax_t line = 1;
    int status = 0, count = 0;
    char **files = calloc((size_t)argc, sizeof(*files));
    struct cat_output out = {.session = session};
    if(!files)
        return 1;
    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if(ended || arg[0] != '-' || !arg[1]) {
            files[count++] = argv[i];
            continue;
        }
        if(strcmp(arg, "--") == 0) {
            ended = true;
            continue;
        }
        if(strcmp(arg, "--help") == 0) {
            status =
                psh_out(session, "%s\n", psh_command_usage("cat")) < 0 ? 1 : 0;
            goto done;
        }
        for(size_t k = 1; arg[k]; k++) {
            switch(arg[k]) {
            case 'n':
                number = true;
                break;
            case 'b':
                nonblank = true;
                break;
            case 's':
                squeeze = true;
                break;
            case 'E':
                ends = true;
                break;
            case 'T':
                tabs = true;
                break;
            case 'v':
                visible = true;
                break;
            case 'A':
                visible = ends = tabs = true;
                break;
            default:
                psh_err(session,
                        "cat: invalid option -- '%c'\nTry 'cat --help' for "
                        "more information.\n",
                        arg[k]);
                status = 2;
                goto done;
            }
        }
    }
    if(!count) {
        psh_err(session, "cat: missing file operand\n");
        status = 1;
        goto done;
    }
    for(int i = 0; i < count && !out.failed; i++) {
        char path[PSX_PATH_MAX];
        uint8_t buffer[4096];
        int fd;
        if(strcmp(files[i], "-") == 0) {
            cat_flush(&out);
            psh_err(session,
                    "cat: session stdin is not supported; specify a file\n");
            status = 1;
            continue;
        }
        if(psx_session_absolute_path(session, files[i], path, sizeof(path)) <
               0 ||
           (fd = open(path, O_RDONLY)) < 0) {
            int saved = errno;
            char *name = psh_display_text(files[i], true, NULL);
            cat_flush(&out);
            psh_err(session, "cat: %s: %s\n", name ? name : "?",
                    strerror(saved));
            free(name);
            status = 1;
            continue;
        }
        for(;;) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if(n < 0) {
                if(errno == EINTR)
                    continue;
                int saved = errno;
                char *name = psh_display_text(files[i], true, NULL);
                cat_flush(&out);
                psh_err(session, "cat: %s: %s\n", name ? name : "?",
                        strerror(saved));
                free(name);
                status = 1;
                break;
            }
            if(!n || out.failed)
                break;
            if(!number && !nonblank && !squeeze && !ends && !tabs && !visible) {
                if(psx_session_emit(session, PTTY_MSG_STDOUT, buffer,
                                    (size_t)n) < 0)
                    out.failed = true;
                continue;
            }
            for(ssize_t j = 0; j < n && !out.failed; j++) {
                uint8_t byte = buffer[j];
                if(pending_cr) {
                    if(byte == '\n') {
                        cat_byte(&out, '^');
                        cat_byte(&out, 'M');
                    } else
                        cat_byte(&out, '\r');
                    pending_cr = false;
                }
                if(start) {
                    if(byte == '\n') {
                        if(squeeze && blank_run)
                            continue;
                        blank_run = 1;
                    } else
                        blank_run = 0;
                    if((nonblank && byte != '\n') || (number && !nonblank)) {
                        char prefix[32];
                        int length =
                            snprintf(prefix, sizeof(prefix), "%6ju\t", line++);
                        for(int k = 0; k < length; k++)
                            cat_byte(&out, (uint8_t)prefix[k]);
                    }
                }
                if(byte == '\n') {
                    if(ends)
                        cat_byte(&out, '$');
                    cat_byte(&out, '\n');
                    start = true;
                } else {
                    start = false;
                    if(byte == '\t') {
                        if(tabs) {
                            cat_byte(&out, '^');
                            cat_byte(&out, 'I');
                        } else
                            cat_byte(&out, byte);
                    } else if(visible)
                        cat_visible(&out, byte);
                    else if(ends && byte == '\r')
                        pending_cr = true;
                    else
                        cat_byte(&out, byte);
                }
            }
        }
        close(fd);
    }
    if(pending_cr)
        cat_byte(&out, '\r');
    cat_flush(&out);
    if(out.failed)
        status = 1;
done:
    free(files);
    return status;
}
