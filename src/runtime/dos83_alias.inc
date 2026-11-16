/* SPDX-License-Identifier: MIT */
/* Included after confined_open_raw and before resolve_name.
 * Lazy DOS 8.3 aliases: persistent on native Linux inodes. Restricted:
 * no hardlinked files, no symlinks, no guarantee against Linux processes
 * allocating aliases concurrently outside this runtime's mutex. */
#define WIN_DOS83_XATTR "user.ntnative.dos83"
static ssize_t read_guest_xattr(int fd, const char *name, void *value, size_t size);
static int write_guest_xattr(int fd, const char *name, const void *value, size_t size);
static pthread_mutex_t dos83_lock = PTHREAD_MUTEX_INITIALIZER;

static bool dos83_char(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '-' || c == '$';
}

static bool dos83_native(const char *name, char alias[14])
{
    const size_t n = strlen(name);
    const char *dot = strrchr(name, '.');
    const size_t stem = dot ? (size_t)(dot - name) : n;
    if (!stem || stem > 8 || (dot && (n - stem - 1 == 0 || n - stem - 1 > 3)))
        return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (dot && i == stem)
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (!dos83_char(c))
            return false;
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)name[i];
        alias[i] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    alias[n] = 0;
    return true;
}

static bool dos83_read_at(int parent, const char *leaf, char alias[14])
{
    const int fd = confined_open_raw(parent, leaf, O_PATH | O_NOFOLLOW, 0);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 &&
              (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
    if (ok) {
        const ssize_t count = read_guest_xattr(fd, WIN_DOS83_XATTR, alias, 14);
        ok = count >= 3 && count <= 13 && alias[count - 1] == '\0' &&
             memchr(alias, '~', (size_t)count - 1) != NULL;
        if (ok)
            for (ssize_t i = 0; i < count - 1; ++i)
                if (!dos83_char((unsigned char)alias[i]) &&
                    alias[i] != '.' && alias[i] != '~') {
                    ok = false;
                    break;
                }
    }
    close(fd);
    return ok;
}

static void dos83_make(const char *name, unsigned int ordinal, char alias[14])
{
    char stem[9] = {0}, ext[4] = {0}, digit[16] = {0};
    const char *dot = strrchr(name, '.');
    const size_t limit = dot ? (size_t)(dot - name) : strlen(name);
    snprintf(digit, sizeof(digit), "%u", ordinal);
    const size_t digit_len = strlen(digit);
    const size_t keep = digit_len < 7 ? 7 - digit_len : 1;
    size_t p = 0, e = 0;
    for (size_t i = 0; i < limit && p < keep; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (dos83_char(c))
            stem[p++] = (char)c;
    }
    if (!p)
        stem[p++] = 'F';
    if (dot)
        for (const char *q = dot + 1; *q && e < 3; ++q) {
            unsigned char c = (unsigned char)*q;
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (dos83_char(c))
                ext[e++] = (char)c;
        }
    /* The 8.3 field is at most 8+1+3 bytes; assemble with the
     * already bounded stem/digit/extension lengths (no truncation). */
    size_t result = 0;
    memcpy(alias + result, stem, p);
    result += p;
    alias[result++] = '~';
    memcpy(alias + result, digit, digit_len);
    result += digit_len;
    if (e) {
        alias[result++] = '.';
        memcpy(alias + result, ext, e);
        result += e;
    }
    alias[result] = 0;
}

static int dos83_collision(int parent, const char *owner, const char *alias)
{
    const int fd = confined_open_raw(parent, ".", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0)
        return -1;
    DIR *dir = fdopendir(fd);
    if (!dir) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    int found = 0;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            if (errno)
                found = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
            !strcmp(entry->d_name, owner))
            continue;
        char previous[14] = {0};
        if (same_unicode_name(alias, entry->d_name) ||
            (dos83_read_at(parent, entry->d_name, previous) &&
             strcasecmp(alias, previous) == 0)) {
            found = 1;
            break;
        }
    }
    int error = errno;
    closedir(dir);
    if (found < 0)
        errno = error;
    return found;
}

static bool dos83_assign_at(int parent, const char *name, char alias[14])
{
    if (dos83_native(name, alias))
        return true;
    pthread_mutex_lock(&dos83_lock);
    int fd = confined_open_raw(parent, name, O_PATH | O_NOFOLLOW, 0);
    if (fd < 0) {
        pthread_mutex_unlock(&dos83_lock);
        return false;
    }
    struct stat st;
    int error = fstat(fd, &st) != 0 ? errno : 0;
    if (!error && !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)))
        error = ENOTSUP;
    if (!error && S_ISREG(st.st_mode) && st.st_nlink > 1)
        error = ENOTSUP;
    if (!error && dos83_read_at(parent, name, alias)) {
        const int collision = dos83_collision(parent, name, alias);
        if (collision != 0)
            error = collision < 0 ? errno : EEXIST;
    } else if (!error) {
        bool assigned = false;
        for (unsigned int i = 1; i <= 999999; ++i) {
            dos83_make(name, i, alias);
            const int collision = dos83_collision(parent, name, alias);
            if (collision < 0) {
                error = errno;
                break;
            }
            if (collision == 0) {
                if (write_guest_xattr(fd, WIN_DOS83_XATTR,
                                      alias, strlen(alias) + 1) == 0)
                    assigned = true;
                else
                    error = errno;
                break;
            }
        }
        if (!assigned && !error)
            error = ENOSPC;
    }
    close(fd);
    pthread_mutex_unlock(&dos83_lock);
    if (error) {
        errno = error;
        return false;
    }
    return true;
}
