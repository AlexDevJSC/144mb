/* SPDX-License-Identifier: MIT */
/* Included after confined_open_raw and before resolve_name.
 * Every alias is bound to the *current directory entry*, not just an inode:
 * renaming a file invalidates its old alias; hardlinks carry independent
 * names. Normal files retain per-inode xattrs; hardlinks use a per-directory
 * xattr keyed by the name and guarded by the existing share registry.
 * External non-cooperating Linux writers are outside the atomicity model. */
#define WIN_DOS83_XATTR "user.ntnative.dos83"
static ssize_t read_guest_xattr(int fd, const char *name, void *value, size_t size);
static int write_guest_xattr(int fd, const char *name, const void *value, size_t size);
static int open_share_registry(const struct stat *info);
static pthread_mutex_t dos83_lock = PTHREAD_MUTEX_INITIALIZER;

/* New inode format: a single xattr update commits both alias and original
 * basename. Existing raw NUL-terminated aliases are read for compatibility
 * and migrated when GetShortPathName next claims them. */
struct dos83_inode_record {
    char signature[4]; /* D832 */
    char name[NAME_MAX + 1];
    char alias[14];
};
struct dos83_directory_record {
    char signature[4]; /* D833 */
    uint64_t device, inode;
    char name[NAME_MAX + 1];
    char alias[14];
};
static bool dos83_alias_valid(const char alias[14])
{
    const size_t length = strnlen(alias, 14);
    if (length < 2 || length > 12 || length == 14 ||
        strchr(alias, '~') == NULL)
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)alias[i];
        if (!dos83_char(c) && c != '~' && c != '.')
            return false;
    }
    return true;
}
static void dos83_xattr_key(const char *name, char key[64])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        hash ^= *p;
        hash *= UINT64_C(1099511628211);
    }
    snprintf(key, 64, "user.ntnative.dos83.%016" PRIx64, hash);
}


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
    bool valid = fstat(fd, &st) == 0 &&
                 (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode));
    if (valid) {
        char key[64];
        struct dos83_directory_record directory_record = {0};
        dos83_xattr_key(leaf, key);
        const ssize_t count = read_guest_xattr(parent, key, &directory_record,
                                                sizeof(directory_record));
        if (count == (ssize_t)sizeof(directory_record) &&
            memcmp(directory_record.signature, "D833", 4) == 0 &&
            directory_record.device == (uint64_t)st.st_dev &&
            directory_record.inode == (uint64_t)st.st_ino &&
            strnlen(directory_record.name, sizeof(directory_record.name)) ==
                strlen(leaf) &&
            strcmp(directory_record.name, leaf) == 0 &&
            dos83_alias_valid(directory_record.alias)) {
            memcpy(alias, directory_record.alias, 14);
        } else if (st.st_nlink == 1) {
            /* Regular file moved to another folder: the stored original
             * name must still match. Never expose a stale alias after rename. */
            struct dos83_inode_record inode_record = {0};
            const ssize_t bytes = read_guest_xattr(fd, WIN_DOS83_XATTR,
                                                    &inode_record, sizeof(inode_record));
            if (bytes == (ssize_t)sizeof(inode_record) &&
                memcmp(inode_record.signature, "D832", 4) == 0 &&
                strnlen(inode_record.name, sizeof(inode_record.name)) ==
                    strlen(leaf) &&
                strcmp(inode_record.name, leaf) == 0 &&
                dos83_alias_valid(inode_record.alias))
                memcpy(alias, inode_record.alias, 14);
            else if (bytes >= 3 && bytes <= 13) {
                /* Old pre-D832 data: read-only compatibility. */
                const char *raw = (const char *)&inode_record;
                if (raw[bytes - 1] == '\0' && dos83_alias_valid(raw))
                    memcpy(alias, raw, (size_t)bytes);
            }
        } else {
            valid = false;
        }
    }
    close(fd);
    return valid && dos83_alias_valid(alias);
}

static bool dos83_save_at(int parent, int fd, const struct stat *st,
                          const char *name, const char alias[14])
{
    if (st->st_nlink > 1 && S_ISREG(st->st_mode)) {
        char key[64];
        dos83_xattr_key(name, key);
        struct dos83_directory_record value = {0};
        memcpy(value.signature, "D833", 4);
        value.device = (uint64_t)st->st_dev;
        value.inode = (uint64_t)st->st_ino;
        strcpy(value.name, name);
        memcpy(value.alias, alias, 14);
        return write_guest_xattr(parent, key, &value, sizeof(value)) == 0;
    }
    struct dos83_inode_record value = {0};
    memcpy(value.signature, "D832", 4);
    strcpy(value.name, name);
    memcpy(value.alias, alias, 14);
    return write_guest_xattr(fd, WIN_DOS83_XATTR, &value, sizeof(value)) == 0;
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
    /* Serialize assignments among cooperating NT processes through the
     * existing per-directory (dev,ino) private share-registry gate. Linux
     * processes that do not participate are intentionally outside scope. */
    struct stat parent_st;
    int gate = -1;
    if (!error && fstat(parent, &parent_st) != 0)
        error = errno;
    if (!error) {
        gate = open_share_registry(&parent_st);
        if (gate < 0)
            error = errno;
    }
    if (!error) {
        int locked;
        do {
            locked = flock(gate, LOCK_EX);
        } while (locked < 0 && errno == EINTR);
        if (locked < 0)
            error = errno;
    }
    if (!error && dos83_read_at(parent, name, alias)) {
        const int collision = dos83_collision(parent, name, alias);
        if (collision != 0)
            error = collision < 0 ? errno : EEXIST;
        else if (!dos83_save_at(parent, fd, &st, name, alias))
            error = errno; /* migrate legacy xattrs and record hardlinks */
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
                if (dos83_save_at(parent, fd, &st, name, alias))
                    assigned = true;
                else
                    error = errno;
                break;
            }
        }
        if (!assigned && !error)
            error = ENOSPC;
    }
    if (gate >= 0)
        close(gate);
    close(fd);
    pthread_mutex_unlock(&dos83_lock);
    if (error) {
        errno = error;
        return false;
    }
    return true;
}
