/* SPDX-License-Identifier: MIT */
/* Included after confined_open_raw and before resolve_name.
 * Lazy DOS 8.3 aliases are keyed by native Linux directory ENTRY and
 * stored as xattrs on the pinned inode. Hardlinks have distinct keys.
 * External Linux renames do not atomically transfer those keys; callers
 * must revalidate the directory entry and must never follow stale aliases. */
#define WIN_DOS83_XATTR "user.ntnative.dos83"
/* NTFS short names belong to directory entries, not to bare inodes. Use a
 * separate key for each (parent directory, leaf), so Linux hardlinks and
 * a file moved between directories do not inherit a different entry's name.
 * Both hashes are from untrusted spellings; a collision is only a hint:
 * caller still verifies real inode/name and rejects ambiguous aliases. */
static bool dos83_entry_key(int parent, const char *leaf, char key[112])
{
    struct stat st;
    if (fstat(parent, &st) != 0)
        return false;
    uint64_t hash1 = UINT64_C(0xcbf29ce484222325);
    uint64_t hash2 = UINT64_C(0x84222325cbf29ce4);
    for (const unsigned char *p = (const unsigned char *)leaf; *p; ++p) {
        const uint8_t c = *p >= 'A' && *p <= 'Z' ? (uint8_t)(*p + 32) : *p;
        hash1 = (hash1 ^ c) * UINT64_C(0x100000001b3);
        hash2 = (hash2 ^ (uint8_t)(c ^ 0xa5u)) * UINT64_C(0x100000001b3);
    }
    const int n = snprintf(key, 112, "%s.%016llx.%016llx.%016llx.%016llx",
          WIN_DOS83_XATTR, (unsigned long long)(uint64_t)st.st_dev,
          (unsigned long long)(uint64_t)st.st_ino,
          (unsigned long long)hash1, (unsigned long long)hash2);
    if (n <= 0 || n >= 112) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}
static ssize_t read_guest_xattr(int fd, const char *name, void *value, size_t size);
static int write_guest_xattr(int fd, const char *name, const void *value, size_t size);
static int open_share_registry(const struct stat *info);
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
        char key[112];
        if (!dos83_entry_key(parent, leaf, key)) {
            close(fd);
            return false;
        }
        /* Older per-inode aliases are intentionally not used for lookups:
         * they could refer to another hardlink or a renamed directory entry. */
        const ssize_t count = read_guest_xattr(fd, key, alias, 14);
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
    /* A non-cooperating Linux writer may replace/rename this leaf during
     * the xattr read. Never return metadata for an unlinked pinned inode
     * as though it belonged to the newly observed directory entry. */
    if (ok) {
        struct stat current;
        ok = fstatat(parent, leaf, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
             current.st_dev == st.st_dev && current.st_ino == st.st_ino;
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
                char key[112];
                if (!dos83_entry_key(parent, name, key)) {
                    error = errno;
                } else if (write_guest_xattr(fd, key, alias,
                                              strlen(alias) + 1) == 0) {
                    assigned = true;
                    /* Keep the legacy attribute solely as an optional
                     * diagnostic for single-link files. It is NOT a
                     * lookup key and does not affect alias correctness. */
                    if (S_ISREG(st.st_mode) && st.st_nlink == 1)
                        (void)write_guest_xattr(fd, WIN_DOS83_XATTR,
                                                alias, strlen(alias) + 1);
                } else {
                    error = errno;
                }
                break;
            }
        }
        if (!assigned && !error)
            error = ENOSPC;
    }
    /* flock coordinates only NT participants, not arbitrary host renames.
     * Detect replacement before success, rather than silently returning
     * an alias bound to a directory entry that no longer exists. A later
     * host mutation remains an explicit interoperability limitation. */
    if (!error) {
        struct stat current;
        if (fstatat(parent, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            current.st_dev != st.st_dev || current.st_ino != st.st_ino)
            error = EBUSY;
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
