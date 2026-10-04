/*
 * ls: a listing that behaves the way people expect from a Linux terminal.
 *
 * Names are laid out in columns sized to the session's width, the usual file
 * classes are coloured from LS_COLORS, and the long format carries the same
 * fields as the GNU tool. Widths are always computed on the plain names, so
 * escape sequences never take part in the arithmetic.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef PSXTERM_HOST
#include <grp.h>
#include <pwd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#endif

#include "psxterm/shell.h"

/* The sticky bit is not always visible through the system headers. */
#ifndef S_ISVTX
#define S_ISVTX 01000
#endif

#define LS_MAX_RULES 160
#define LS_GAP 2

struct ls_rule {
    char key[24];
    char value[64];
};

struct ls_colors {
    struct ls_rule rules[LS_MAX_RULES];
    size_t count;
};

struct ls_entry {
    char *name;
    char *display;
    char *link;
    size_t width;
    struct stat st;
    bool statted;
    bool orphan;
    char owner[256];
    char group[256];
    char size[64];
    char classify;
    const char *color;
};

struct ls_options {
    bool all;
    bool almost_all;
    bool one_per_line;
    bool across;
    bool long_format;
    bool human;
    bool classify;
    bool directory;
    bool color;
    bool numeric;
    bool reverse;
    bool sort_size;
    bool sort_time;
    bool slash;
};

/*
 * Use familiar Linux file classes with bold, bright foreground colours for
 * contrast in a dark terminal. LS_COLORS can override every class.
 */
static void
ls_colors_default(struct ls_colors *colors)
{
    static const struct {
        const char *key;
        const char *value;
    } defaults[] = {
        {"di", "1;94"},     {"ln", "1;96"},     {"ex", "1;92"},
        {"so", "1;95"},     {"pi", "40;33"},    {"bd", "40;33;1"},
        {"cd", "40;33;1"},  {"or", "1;91"},     {"su", "37;41"},
        {"sg", "30;43"},    {"tw", "30;42"},    {"ow", "34;42"},
        {"st", "37;42"},    {"*.tar", "1;91"},  {"*.tgz", "1;91"},
        {"*.gz", "1;91"},   {"*.bz2", "1;91"},  {"*.xz", "1;91"},
        {"*.zst", "1;91"},  {"*.zip", "1;91"},  {"*.7z", "1;91"},
        {"*.rar", "1;91"},  {"*.pkg", "1;91"},  {"*.elf", "1;91"},
        {"*.o", "1;96"},    {"*.a", "1;96"},    {"*.so", "1;96"},
        {"*.png", "1;95"},  {"*.jpg", "1;95"},  {"*.jpeg", "1;95"},
        {"*.gif", "1;95"},  {"*.webp", "1;95"}, {"*.bmp", "1;95"},
        {"*.mp3", "1;95"},  {"*.mp4", "1;95"},  {"*.mkv", "1;95"},
        {"*.avi", "1;95"},  {"*.mov", "1;95"},  {"*.wav", "1;95"},
        {"*.flac", "1;95"},
    };

    memset(colors, 0, sizeof(*colors));

    for(size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) {
        snprintf(colors->rules[colors->count].key,
                 sizeof(colors->rules[colors->count].key), "%s",
                 defaults[i].key);
        snprintf(colors->rules[colors->count].value,
                 sizeof(colors->rules[colors->count].value), "%s",
                 defaults[i].value);
        colors->count++;
    }
}

/*
 * LS_COLORS is a colon separated list of "key=value" pairs, where a key is
 * either a two letter class or *extension. Later entries win, and anything the
 * environment does not mention keeps its default.
 */
static void
ls_colors_parse(struct ls_colors *colors, const char *spec)
{
    char buffer[8192];
    char *cursor;
    char *field;

    if(!spec || !*spec || strlen(spec) >= sizeof(buffer)) {
        return;
    }

    snprintf(buffer, sizeof(buffer), "%s", spec);

    for(field = strtok_r(buffer, ":", &cursor); field;
        field = strtok_r(NULL, ":", &cursor)) {
        char *equals = strchr(field, '=');

        if(!equals || equals == field || colors->count >= LS_MAX_RULES) {
            continue;
        }

        *equals = '\0';
        if(strlen(field) >= sizeof(colors->rules[0].key) ||
           strlen(equals + 1) >= sizeof(colors->rules[0].value) ||
           strspn(equals + 1, "0123456789;") != strlen(equals + 1)) {
            continue;
        }
        snprintf(colors->rules[colors->count].key,
                 sizeof(colors->rules[colors->count].key), "%s", field);
        snprintf(colors->rules[colors->count].value,
                 sizeof(colors->rules[colors->count].value), "%s", equals + 1);
        colors->count++;
    }
}

static const char *
ls_color_key(const struct ls_colors *colors, const char *key)
{
    /* The last matching rule wins, which is what the GNU tool documents. */
    for(size_t i = colors->count; i > 0; i--) {
        if(strcmp(colors->rules[i - 1].key, key) == 0) {
            return colors->rules[i - 1].value;
        }
    }

    return NULL;
}

static const char *
ls_color_extension(const struct ls_colors *colors, const char *name)
{
    size_t length = strlen(name);
    for(size_t i = colors->count; i > 0; i--) {
        const char *key = colors->rules[i - 1].key;
        size_t suffix = strlen(key + 1);
        if(key[0] == '*' && suffix <= length &&
           strcmp(name + length - suffix, key + 1) == 0) {
            return colors->rules[i - 1].value;
        }
    }

    return NULL;
}

static const char *
ls_color_for(const struct ls_colors *colors, const struct ls_entry *entry)
{
    const struct stat *st = &entry->st;
    const char *color;

    if(!colors->count) {
        return NULL;
    }

    if(S_ISDIR(st->st_mode)) {
        if(st->st_mode & S_ISVTX) {
            return ls_color_key(colors, (st->st_mode & S_IWOTH) ? "tw" : "st");
        }
        if(st->st_mode & S_IWOTH)
            return ls_color_key(colors, "ow");
        return ls_color_key(colors, "di");
    }

    if(S_ISLNK(st->st_mode)) {
        return ls_color_key(colors, entry->orphan ? "or" : "ln");
    }

    if(st->st_mode & S_ISUID) {
        return ls_color_key(colors, "su");
    }

    if(st->st_mode & S_ISGID) {
        return ls_color_key(colors, "sg");
    }

    if(S_ISBLK(st->st_mode)) {
        return ls_color_key(colors, "bd");
    }

    if(S_ISCHR(st->st_mode)) {
        return ls_color_key(colors, "cd");
    }

    if(S_ISSOCK(st->st_mode)) {
        return ls_color_key(colors, "so");
    }

    if(S_ISFIFO(st->st_mode)) {
        return ls_color_key(colors, "pi");
    }

    if(st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
        return ls_color_key(colors, "ex");
    }

    color = ls_color_extension(colors, entry->name);
    return color ? color : ls_color_key(colors, "fi");
}

static char *
ls_colored(const char *color, const char *text, char classify)
{
    size_t need = strlen(text) + 2 + (color && *color ? strlen(color) + 7 : 0);
    char suffix[2] = {classify, '\0'};
    char *out;

    if(!(out = malloc(need))) {
        return NULL;
    }

    if(color && *color) {
        snprintf(out, need, "\033[%sm%s%s\033[0m", color, text, suffix);
    } else {
        snprintf(out, need, "%s%s", text, suffix);
    }

    return out;
}

static void
ls_classify(char *suffix, const struct stat *st)
{
    if(S_ISDIR(st->st_mode)) {
        *suffix = '/';
    } else if(S_ISLNK(st->st_mode)) {
        *suffix = '@';
    } else if(S_ISSOCK(st->st_mode)) {
        *suffix = '=';
    } else if(S_ISFIFO(st->st_mode)) {
        *suffix = '|';
    } else if(S_ISREG(st->st_mode) &&
              (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        *suffix = '*';
    }
}

static int
ls_entry_comp(const void *a, const void *b)
{
    const struct ls_entry *left = a;
    const struct ls_entry *right = b;

    return strcmp(left->name, right->name);
}

static void
ls_entries_free(struct ls_entry *entries, size_t count)
{
    for(size_t i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].display);
        free(entries[i].link);
    }

    free(entries);
}

static void ls_human(char *out, size_t size, uintmax_t value);

static int
ls_size_comp(const void *a, const void *b)
{
    const struct ls_entry *left = a, *right = b;
    if(left->st.st_size != right->st.st_size) {
        return left->st.st_size > right->st.st_size ? -1 : 1;
    }
    return ls_entry_comp(a, b);
}

static int
ls_time_comp(const void *a, const void *b)
{
    const struct ls_entry *left = a, *right = b;
    if(left->st.st_mtime != right->st.st_mtime) {
        return left->st.st_mtime > right->st.st_mtime ? -1 : 1;
    }
#ifdef __APPLE__
    long lns = left->st.st_mtimespec.tv_nsec,
         rns = right->st.st_mtimespec.tv_nsec;
#else
    long lns = left->st.st_mtim.tv_nsec, rns = right->st.st_mtim.tv_nsec;
#endif
    return lns == rns ? ls_entry_comp(a, b) : (lns > rns ? -1 : 1);
}

static void
ls_sort(struct ls_entry *entries, size_t count,
        const struct ls_options *options)
{
    if(count < 2)
        return;
    qsort(entries, count, sizeof(*entries),
          options->sort_size   ? ls_size_comp
          : options->sort_time ? ls_time_comp
                               : ls_entry_comp);
    if(options->reverse) {
        for(size_t i = 0; i < count / 2; i++) {
            struct ls_entry swap = entries[i];
            entries[i] = entries[count - i - 1];
            entries[count - i - 1] = swap;
        }
    }
}

static int
ls_prepare(psx_session_t *session, struct ls_entry *entry, const char *path,
           const struct ls_options *options, const struct ls_colors *colors)
{
    entry->display = psh_display_text(entry->name, true, &entry->width);
    if(!entry->display)
        return -1;
    if(lstat(path, &entry->st) < 0) {
        psh_err(session, "ls: cannot access %s: %s\n", entry->display,
                strerror(errno));
        return 1;
    }
    entry->statted = true;
    ls_classify(&entry->classify, &entry->st);
    if(S_ISLNK(entry->st.st_mode)) {
        struct stat target_st;
        size_t capacity = PSX_PATH_MAX;
        char *target;
        for(;;) {
            if(!(target = malloc(capacity + 1)))
                return -1;
            ssize_t n = readlink(path, target, capacity);
            if(n < 0) {
                int saved = errno;
                free(target);
                psh_err(session, "ls: cannot read symbolic link %s: %s\n",
                        entry->display, strerror(saved));
                return 1;
            }
            if((size_t)n < capacity) {
                target[n] = '\0';
                break;
            }
            free(target);
            if(capacity >= 65536) {
                errno = ENAMETOOLONG;
                return -1;
            }
            capacity *= 2;
        }
        entry->link = psh_display_text(target, true, NULL);
        free(target);
        if(!entry->link)
            return -1;
        entry->orphan = stat(path, &target_st) < 0;
    }
    entry->color = ls_color_for(colors, entry);
    snprintf(entry->owner, sizeof(entry->owner), "%ju",
             (uintmax_t)entry->st.st_uid);
    snprintf(entry->group, sizeof(entry->group), "%ju",
             (uintmax_t)entry->st.st_gid);
#ifdef PSXTERM_HOST
    if(!options->numeric) {
        struct passwd *owner = getpwuid(entry->st.st_uid);
        if(owner)
            snprintf(entry->owner, sizeof(entry->owner), "%s", owner->pw_name);
        struct group *group = getgrgid(entry->st.st_gid);
        if(group)
            snprintf(entry->group, sizeof(entry->group), "%s", group->gr_name);
    }
#endif
    if(options->human) {
        ls_human(entry->size, sizeof(entry->size),
                 (uintmax_t)entry->st.st_size);
    } else {
        snprintf(entry->size, sizeof(entry->size), "%ju",
                 (uintmax_t)entry->st.st_size);
    }
#if defined(PSXTERM_HOST)
    if(S_ISCHR(entry->st.st_mode) || S_ISBLK(entry->st.st_mode)) {
        snprintf(entry->size, sizeof(entry->size), "%u, %u",
                 (unsigned)major(entry->st.st_rdev),
                 (unsigned)minor(entry->st.st_rdev));
    }
#endif
    return 0;
}

static int
ls_collect(psx_session_t *session, const struct ls_options *options,
           const struct ls_colors *colors, const char *path,
           struct ls_entry **out, size_t *count)
{
    struct ls_entry *entries = NULL;
    size_t used = 0, capacity = 0;
    DIR *dir = opendir(path);
    int status = 0;
    if(!dir)
        return -1;

    for(;;) {
        char full[PSX_PATH_MAX];
        struct ls_entry *entry;
        struct dirent *item;
        int rc;
        errno = 0;
        item = readdir(dir);
        if(!item) {
            if(errno)
                status = -1;
            break;
        }
        if(item->d_name[0] == '.' && !options->all &&
           (!options->almost_all || strcmp(item->d_name, ".") == 0 ||
            strcmp(item->d_name, "..") == 0))
            continue;
        if(used == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            struct ls_entry *grown;
            if(next < capacity || next > SIZE_MAX / sizeof(*entries) ||
               !(grown = realloc(entries, next * sizeof(*entries)))) {
                errno = ENOMEM;
                status = -1;
                break;
            }
            entries = grown;
            capacity = next;
        }
        entry = &entries[used++];
        memset(entry, 0, sizeof(*entry));
        entry->name = strdup(item->d_name);
        if(!entry->name ||
           psx_path_join(full, sizeof(full), path, item->d_name) < 0) {
            status = -1;
            break;
        }
        rc = ls_prepare(session, entry, full, options, colors);
        if(rc < 0) {
            status = -1;
            break;
        }
        if(rc)
            status = 1;
    }
    int saved = errno;
    closedir(dir);
    if(status < 0) {
        ls_entries_free(entries, used);
        errno = saved;
        return -1;
    }
    ls_sort(entries, used, options);
    *out = entries;
    *count = used;
    return status;
}

static void
ls_mode_string(mode_t mode, char out[11])
{
    static const char *rwx = "rwxrwxrwx";

    out[0] = S_ISDIR(mode)    ? 'd'
             : S_ISLNK(mode)  ? 'l'
             : S_ISBLK(mode)  ? 'b'
             : S_ISCHR(mode)  ? 'c'
             : S_ISFIFO(mode) ? 'p'
             : S_ISSOCK(mode) ? 's'
                              : '-';

    for(int i = 0; i < 9; i++) {
        out[i + 1] = (mode & (mode_t)(1 << (8 - i))) ? rwx[i] : '-';
    }

    if(mode & S_ISUID) {
        out[3] = (mode & S_IXUSR) ? 's' : 'S';
    }

    if(mode & S_ISGID) {
        out[6] = (mode & S_IXGRP) ? 's' : 'S';
    }

    if(mode & S_ISVTX) {
        out[9] = (mode & S_IXOTH) ? 't' : 'T';
    }

    out[10] = '\0';
}

static void
ls_human(char *out, size_t size, uintmax_t value)
{
    static const char units[] = "KMGTPE";
    uintmax_t divisor = 1;
    size_t unit = 0;
    while(value / divisor >= 1024 && unit < sizeof(units) - 1) {
        divisor *= 1024;
        unit++;
    }
    if(!unit) {
        snprintf(out, size, "%ju", value);
        return;
    }
    uintmax_t whole = value / divisor, remainder = value % divisor;
    if(whole < 10) {
        /* Round upward, like coreutils, without multiplying a large size. */
        unsigned tenth = 0;
        if(remainder) {
            for(tenth = 1; tenth <= 10; tenth++) {
                uintmax_t limit =
                    (divisor / 10) * tenth + (divisor % 10) * tenth / 10;
                if(remainder <= limit)
                    break;
            }
            if(tenth > 9) {
                whole++;
                tenth = 0;
            }
        }
        if(whole < 10)
            snprintf(out, size, "%ju.%u%c", whole, tenth, units[unit - 1]);
        else
            snprintf(out, size, "%ju%c", whole, units[unit - 1]);
    } else {
        snprintf(out, size, "%ju%c", whole + (remainder ? 1 : 0),
                 units[unit - 1]);
    }
}

static void
ls_time_string(char *out, size_t size, time_t when)
{
    struct tm broken;
    double age = difftime(time(NULL), when);

    if(!localtime_r(&when, &broken)) {
        snprintf(out, size, "?");
        return;
    }

    /* Recent entries show a time, older ones the year, as ls does. */
    if(age < -3600.0 || age > 15778476.0) {
        strftime(out, size, "%b %e  %Y", &broken);
    } else {
        strftime(out, size, "%b %e %H:%M", &broken);
    }
}

struct ls_long_widths {
    int links, owner, group, size;
};

static int
ls_long(psx_session_t *session, const struct ls_options *options,
        const struct ls_entry *entry, const struct ls_long_widths *widths)
{
    char mode[11], when[32];
    char suffix = options->classify ? entry->classify
                  : options->slash && S_ISDIR(entry->st.st_mode) ? '/'
                                                                 : '\0';
    char *colored = ls_colored(entry->color, entry->display, suffix);
    int rc;
    if(!colored)
        return 1;
    if(!entry->statted) {
        rc = psh_out(session, "?????????? %*s %-*s %-*s %*s ? %s\n",
                     widths->links, "?", widths->owner, "?", widths->group, "?",
                     widths->size, "?", colored);
    } else {
        ls_mode_string(entry->st.st_mode, mode);
        ls_time_string(when, sizeof(when), entry->st.st_mtime);
        rc = psh_out(session, "%s %*ju %-*s %-*s %*s %s %s%s%s\n", mode,
                     widths->links, (uintmax_t)entry->st.st_nlink,
                     widths->owner, entry->owner, widths->group, entry->group,
                     widths->size, entry->size, when, colored,
                     entry->link ? " -> " : "", entry->link ? entry->link : "");
    }
    free(colored);
    return rc < 0 ? 1 : 0;
}

/*
 * Column layout: the number of columns chosen is the largest one whose columns,
 * sized by their widest entry, keep the whole line inside the session width.
 */
static size_t
ls_fit_columns(const size_t *widths, size_t count, size_t terminal, bool across)
{
    size_t smallest = widths[0];
    for(size_t i = 1; i < count; i++) {
        if(widths[i] < smallest)
            smallest = widths[i];
    }
    size_t columns = (terminal + LS_GAP) / (smallest + LS_GAP);
    if(columns > count)
        columns = count;
    for(; columns > 1; columns--) {
        size_t rows = (count + columns - 1) / columns;
        size_t total = 0, occupied = 0;
        for(size_t col = 0; col < columns; col++) {
            size_t widest = 0;
            bool present = false;
            for(size_t row = 0; row < rows; row++) {
                size_t index = across ? row * columns + col : row + col * rows;
                if(index < count) {
                    present = true;
                    if(widths[index] > widest)
                        widest = widths[index];
                }
            }
            if(present) {
                total += widest;
                occupied++;
            }
        }
        if(total + (occupied - 1) * LS_GAP <= terminal)
            return columns;
    }
    return 1;
}

static char
ls_suffix(const struct ls_entry *entry, const struct ls_options *options)
{
    return options->classify                              ? entry->classify
           : options->slash && S_ISDIR(entry->st.st_mode) ? '/'
                                                          : '\0';
}

static int
ls_short(psx_session_t *session, const struct ls_options *options,
         const struct ls_entry *entries, size_t count)
{
    if(!count)
        return 0;
    size_t terminal = session->cols ? session->cols : 80;
    size_t *widths = calloc(count, sizeof(*widths));
    size_t *column_widths = NULL;
    size_t capacity = terminal + 2;
    char *line = NULL;
    int status = 1;
    if(!widths)
        goto done;
    for(size_t i = 0; i < count; i++) {
        widths[i] =
            entries[i].width + (ls_suffix(&entries[i], options) ? 1 : 0);
        capacity += strlen(entries[i].display) + 2 +
                    (entries[i].color ? strlen(entries[i].color) + 8 : 0);
    }
    size_t columns =
        options->one_per_line
            ? 1
            : ls_fit_columns(widths, count, terminal, options->across);
    size_t rows = (count + columns - 1) / columns;
    column_widths = calloc(columns, sizeof(*column_widths));
    line = malloc(capacity);
    if(!column_widths || !line)
        goto done;
    for(size_t i = 0; i < count; i++) {
        size_t col = options->across ? i % columns : i / rows;
        if(widths[i] > column_widths[col])
            column_widths[col] = widths[i];
    }
    for(size_t row = 0; row < rows; row++) {
        size_t used = 0;
        for(size_t col = 0; col < columns; col++) {
            size_t index =
                options->across ? row * columns + col : row + col * rows;
            if(index >= count)
                break;
            char *colored =
                ls_colored(entries[index].color, entries[index].display,
                           ls_suffix(&entries[index], options));
            if(!colored)
                goto done;
            size_t n = strlen(colored);
            memcpy(line + used, colored, n);
            used += n;
            free(colored);
            size_t next = options->across ? index + 1 : index + rows;
            if(col + 1 < columns && next < count) {
                size_t padding = column_widths[col] - widths[index] + LS_GAP;
                memset(line + used, ' ', padding);
                used += padding;
            }
        }
        line[used++] = '\n';
        if(psx_session_emit(session, PTTY_MSG_STDOUT, line, used) < 0)
            goto done;
    }
    status = 0;
done:
    free(widths);
    free(column_widths);
    free(line);
    return status;
}

static struct ls_long_widths
ls_measure_long(const struct ls_entry *entries, size_t count)
{
    struct ls_long_widths widths = {1, 1, 1, 1};
    for(size_t i = 0; i < count; i++) {
        char links[32];
        snprintf(links, sizeof(links), "%ju",
                 (uintmax_t)entries[i].st.st_nlink);
        int n = (int)strlen(links);
        if(n > widths.links)
            widths.links = n;
        n = (int)strlen(entries[i].owner);
        if(n > widths.owner)
            widths.owner = n;
        n = (int)strlen(entries[i].group);
        if(n > widths.group)
            widths.group = n;
        n = (int)strlen(entries[i].size);
        if(n > widths.size)
            widths.size = n;
    }
    return widths;
}

static int
ls_print(psx_session_t *session, const struct ls_options *options,
         const struct ls_entry *entries, size_t count, bool total,
         const struct ls_long_widths *operand_widths)
{
    if(!options->long_format)
        return ls_short(session, options, entries, count);
    struct ls_long_widths widths =
        operand_widths ? *operand_widths : ls_measure_long(entries, count);
    if(total) {
        uintmax_t blocks = 0;
        for(size_t i = 0; i < count; i++) {
            if(entries[i].st.st_blocks > 0)
                blocks += (uintmax_t)entries[i].st.st_blocks;
        }
        char value[64];
        if(options->human)
            ls_human(value, sizeof(value), blocks * 512);
        else
            snprintf(value, sizeof(value), "%ju", (blocks + 1) / 2);
        if(psh_out(session, "total %s\n", value) < 0)
            return 1;
    }
    for(size_t i = 0; i < count; i++) {
        if(ls_long(session, options, &entries[i], &widths))
            return 1;
    }
    return 0;
}

static void
ls_usage(psx_session_t *session)
{
    psh_out(
        session,
        "usage: ls [-aA1CxlhnFrStpd] [--color[=WHEN]] [path ...]\n"
        "\n"
        "  -a, --all          include entries starting with a dot\n"
        "  -A, --almost-all   include hidden entries but not . and ..\n"
        "  -1                 one entry per line\n"
        "  -C                 multi-column output (the default)\n"
        "  -x                 multi-column output filled by lines\n"
        "  -l                 long format\n"
        "  -h                 human readable sizes, with -l\n"
        "  -F                 append a class suffix (/ * @ = |)\n"
        "  -n                 long format with numeric owner and group IDs\n"
        "  -r                 reverse the sort order\n"
        "  -S, -t             sort by size or modification time (newest "
        "first)\n"
        "  -p                 append / to directories\n"
        "  -d                 list directories themselves, not their contents\n"
        "      --color[=WHEN] colourise: always, auto (the default), never\n");
}

/* Compact operands in place so options also work after path arguments. */
static int
ls_parse_options(psx_session_t *session, int argc, char **argv,
                 struct ls_options *options)
{
    int operands = 1;
    bool ended = false;
    for(int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if(ended || arg[0] != '-' || arg[1] == '\0') {
            argv[operands++] = argv[i];
            continue;
        }
        if(strcmp(arg, "--") == 0) {
            ended = true;
            continue;
        }
        if(strcmp(arg, "--help") == 0) {
            ls_usage(session);
            return -1;
        }
        if(strcmp(arg, "--all") == 0) {
            options->all = true;
            continue;
        }
        if(strcmp(arg, "--almost-all") == 0) {
            options->all = false;
            options->almost_all = true;
            continue;
        }
        if(strcmp(arg, "--color") == 0 || strncmp(arg, "--color=", 8) == 0) {
            const char *when = arg[7] == '=' ? arg + 8 : "always";
            if(strcmp(when, "auto") == 0 || strcmp(when, "tty") == 0) {
                const char *term = psx_env_get(&session->env, "TERM");
                const char *no_color = psx_env_get(&session->env, "NO_COLOR");
                options->color = (!term || strcmp(term, "dumb") != 0) &&
                                 (!no_color || !*no_color);
            } else if(strcmp(when, "always") == 0 || strcmp(when, "yes") == 0 ||
                      strcmp(when, "force") == 0)
                options->color = true;
            else if(strcmp(when, "never") == 0 || strcmp(when, "no") == 0 ||
                    strcmp(when, "none") == 0)
                options->color = false;
            else {
                psh_err(
                    session,
                    "ls: invalid color mode '%s' (use always, auto or never)\n",
                    when);
                return -2;
            }
            continue;
        }
        if(arg[1] == '-') {
            psh_err(session,
                    "ls: unrecognized option '%s'\nTry 'ls --help' for more "
                    "information.\n",
                    arg);
            return -2;
        }
        for(size_t k = 1; arg[k]; k++) {
            switch(arg[k]) {
            case 'a':
                options->all = true;
                break;
            case 'A':
                options->all = false;
                options->almost_all = true;
                break;
            case '1':
                options->one_per_line = true;
                break;
            case 'C':
                options->one_per_line = false;
                options->long_format = false;
                options->across = false;
                break;
            case 'x':
                options->one_per_line = false;
                options->long_format = false;
                options->across = true;
                break;
            case 'l':
                options->long_format = true;
                break;
            case 'n':
                options->numeric = true;
                options->long_format = true;
                break;
            case 'h':
                options->human = true;
                break;
            case 'F':
                options->classify = true;
                break;
            case 'p':
                options->slash = true;
                break;
            case 'd':
                options->directory = true;
                break;
            case 'r':
                options->reverse = true;
                break;
            case 'S':
                options->sort_size = true;
                options->sort_time = false;
                break;
            case 't':
                options->sort_time = true;
                options->sort_size = false;
                break;
            default:
                psh_err(session,
                        "ls: invalid option -- '%c'\nTry 'ls --help' for more "
                        "information.\n",
                        arg[k]);
                return -2;
            }
        }
    }
    return operands;
}

int
psh_builtin_ls(psx_session_t *session, int argc, char **argv)
{
    struct ls_options options = {0};
    struct ls_colors colors;
    struct ls_entry *operands = NULL;
    char **args = calloc((size_t)argc + 2, sizeof(*args));
    int status = 0;
    bool printed = false;
    size_t count = 0;
    if(!args)
        return 1;
    memcpy(args, argv, (size_t)argc * sizeof(*args));
    const char *term = psx_env_get(&session->env, "TERM");
    const char *no_color = psx_env_get(&session->env, "NO_COLOR");
    options.color =
        (!term || strcmp(term, "dumb") != 0) && (!no_color || !*no_color);
    int end = ls_parse_options(session, argc, args, &options);
    if(end < 0) {
        free(args);
        return end == -1 ? 0 : 2;
    }
    if(end == 1)
        args[end++] = ".";
    ls_colors_default(&colors);
    if(options.color)
        ls_colors_parse(&colors, psx_env_get(&session->env, "LS_COLORS"));
    else
        colors.count = 0;
    operands = calloc((size_t)end - 1, sizeof(*operands));
    if(!operands) {
        free(args);
        return 1;
    }
    for(int i = 1; i < end; i++) {
        char path[PSX_PATH_MAX];
        struct ls_entry *entry = &operands[count];
        entry->name = strdup(args[i]);
        if(!entry->name) {
            status = 1;
            break;
        }
        count++;
        if(psx_session_absolute_path(session, args[i], path, sizeof(path)) <
           0) {
            entry->display = psh_display_text(args[i], true, &entry->width);
            psh_err(session, "ls: cannot access %s: %s\n",
                    entry->display ? entry->display : "?", strerror(errno));
            status = 1;
            continue;
        }
        int rc = ls_prepare(session, entry, path, &options, &colors);
        if(rc)
            status = 1;
        if(rc < 0)
            break;
        /* A plain operand pointing at a directory follows the link, as GNU ls
         * does; long/classified listings retain the link and its target. */
        if(entry->statted && S_ISLNK(entry->st.st_mode) && !options.directory &&
           !options.long_format && !options.classify) {
            struct stat target;
            if(stat(path, &target) == 0 && S_ISDIR(target.st_mode))
                entry->st = target;
        }
    }
    ls_sort(operands, count, &options);
    struct ls_entry *files = calloc(count ? count : 1, sizeof(*files));
    if(!files) {
        status = 1;
        goto done;
    }
    size_t file_count = 0;
    for(size_t i = 0; i < count; i++) {
        if(operands[i].statted &&
           (options.directory || !S_ISDIR(operands[i].st.st_mode))) {
            files[file_count++] = operands[i];
        }
    }
    if(file_count) {
        struct ls_long_widths widths = ls_measure_long(operands, count);
        status |=
            ls_print(session, &options, files, file_count, false, &widths);
        printed = true;
    }
    free(files);
    if(options.directory)
        goto done;
    for(size_t i = 0; i < count; i++) {
        if(!operands[i].statted || !S_ISDIR(operands[i].st.st_mode))
            continue;
        char path[PSX_PATH_MAX];
        struct ls_entry *entries = NULL;
        size_t used = 0;
        if(psx_session_absolute_path(session, operands[i].name, path,
                                     sizeof(path)) < 0) {
            status = 1;
            continue;
        }
        int rc = ls_collect(session, &options, &colors, path, &entries, &used);
        if(rc < 0) {
            psh_err(session, "ls: cannot open directory %s: %s\n",
                    operands[i].display, strerror(errno));
            status = 1;
            continue;
        }
        if(rc)
            status = 1;
        if(end > 2) {
            if(psh_out(session, "%s%s:\n", printed ? "\n" : "",
                       operands[i].display) < 0)
                status = 1;
        }
        status |= ls_print(session, &options, entries, used, true, NULL);
        ls_entries_free(entries, used);
        printed = true;
    }
done:
    ls_entries_free(operands, count);
    free(args);
    return status;
}
