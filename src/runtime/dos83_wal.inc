/* SPDX-License-Identifier: MIT */
/* FileSystem4: one durable, fail-closed DOS 8.3 rename journal per grant.
 * This is a storage protocol, NOT an NTFS filesystem or a global Linux
 * write barrier. A new rename cannot reuse an unresolved record. */
/* v2 adds the replaced inode's per-entry xattr preimage. Reading v1
 * remains mandatory: never strand a pending journal after an upgrade. */
enum {
    DOS83_WAL_V1_HEADER = 140,
    DOS83_WAL_HEADER = 160,
    DOS83_WAL_CAP = DOS83_WAL_HEADER + 2 * PATH_MAX
};
#define DOS83_WAL_NAME "dos83-rename-v1.wal"
#define DOS83_WAL_MAGIC_V1 "N83WAL01"
#define DOS83_WAL_MAGIC "N83WAL02"

struct dos83_wal {
    int fd, directory;
};
struct dos83_wal_entry {
    uint64_t root_dev, root_ino, file_dev, file_ino;
    uint64_t from_dir_dev, from_dir_ino, to_dir_dev, to_dir_ino;
    uint64_t victim_dev, victim_ino;
    bool staged, victim, victim_preclean;
    size_t old_size, victim_old_size;
    char old_dest[14], alias[14], victim_old[14];
    char from[PATH_MAX], to[PATH_MAX];
};

static int open_parent(int root, const char *path, char leaf[NAME_MAX + 1]);

/* A failed WAL sync makes both on-disk and page-cache state uncertain.
 * Never reuse an apparently empty WAL after a failed ftruncate/fsync:
 * the old record may reappear after power failure and be replayed against
 * a newer transaction. Recovery must occur in a NEW process, which first
 * syncs the opened WAL inode before declaring it reusable. */
static _Atomic bool wal_storage_poisoned;

static bool wal_poison(int cause)
{
    atomic_store(&wal_storage_poisoned, true);
    errno = cause ? cause : EIO;
    return false;
}

static bool wal_healthy(void)
{
    if (atomic_load(&wal_storage_poisoned)) {
        errno = EUCLEAN;
        return false;
    }
    return true;
}

static void wal_put32(unsigned char *p, uint32_t v)
{
    for (unsigned int i = 0; i < 4; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}
static void wal_put64(unsigned char *p, uint64_t v)
{
    for (unsigned int i = 0; i < 8; ++i)
        p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t wal_get32(const unsigned char *p)
{
    uint32_t v = 0;
    for (unsigned int i = 0; i < 4; ++i)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint64_t wal_get64(const unsigned char *p)
{
    uint64_t v = 0;
    for (unsigned int i = 0; i < 8; ++i)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static uint64_t wal_hash(const unsigned char *bytes, size_t size)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}
static bool wal_sync_pinned(int path_fd, bool directory)
{
    /* fsync does not work on O_PATH. Reopen only the pinned inode through
     * procfs, never through a potentially replaced external name. */
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", path_fd);
    int fd = open(path, O_RDONLY | O_CLOEXEC |
                 (directory ? O_DIRECTORY : 0));
    if (fd < 0 && !directory)
        fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return wal_poison(errno);
    int result = fsync(fd);
    int error = errno;
    close(fd);
    if (result != 0)
        return wal_poison(error);
    return true;
}

static void wal_close(struct dos83_wal *wal)
{
    if (wal->fd >= 0)
        close(wal->fd);
    if (wal->directory >= 0)
        close(wal->directory);
    wal->fd = wal->directory = -1;
}

static bool wal_open(int root, struct dos83_wal *wal)
{
    *wal = (struct dos83_wal){.fd = -1, .directory = -1};
    if (!wal_healthy())
        return false;
    if (mkdirat(root, ":ntnative-file-leases", 0700) != 0 &&
        errno != EEXIST)
        return false;
    wal->directory = confined_open_raw(root, ":ntnative-file-leases",
                                        O_RDONLY | O_DIRECTORY, 0);
    if (wal->directory < 0)
        return false;
    struct stat info;
    if (fstat(wal->directory, &info) != 0 ||
        !S_ISDIR(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 077u) != 0) {
        errno = EACCES;
        wal_close(wal);
        return false;
    }
    wal->fd = confined_open_raw(wal->directory, DOS83_WAL_NAME,
                                O_RDWR | O_CREAT, 0600);
    if (wal->fd < 0) {
        wal_close(wal);
        return false;
    }
    if (fstat(wal->fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 077u) != 0 ||
        info.st_nlink != 1) {
        errno = EACCES;
        wal_close(wal);
        return false;
    }
    while (flock(wal->fd, LOCK_EX) != 0) {
        if (errno == EINTR)
            continue;
        wal_close(wal);
        return false;
    }
    /* Persist the directory entry before a new WAL can authorize changes. */
    if (fsync(wal->directory) != 0 ||
        !wal_sync_pinned(root, true)) {
        int error = errno;
        wal_close(wal);
        return wal_poison(error);
    }
    /* The private registry itself may have been created by mkdirat:
     * parent-root fsync ensures its directory ENTRY is durable, while
     * fsync(wal->directory) above ensures the WAL filename is durable. */
    /* Also fsync the WAL inode on EVERY new process/admission, even when
     * empty: a previous process may have observed a failed clear fsync.
     * A current page-cache size of zero alone is not durable evidence. */
    if (dos83_test_io_error("wal-open-fsync") || fsync(wal->fd) != 0) {
        int error = errno;
        wal_close(wal);
        return wal_poison(error);
    }
    return true;
}
static bool wal_empty(struct dos83_wal *wal)
{
    struct stat st;
    if (fstat(wal->fd, &st) != 0)
        return false;
    if (st.st_size != 0) {
        errno = EUCLEAN;
        return false;
    }
    return true;
}
static bool wal_clear(struct dos83_wal *wal)
{
    if (ftruncate(wal->fd, 0) != 0)
        return wal_poison(errno);
    if (dos83_test_io_error("wal-clear-fsync") || fsync(wal->fd) != 0)
        return wal_poison(errno);
    return true;
}

static bool wal_encode(const struct dos83_wal_entry *entry,
                       unsigned char bytes[DOS83_WAL_CAP], size_t *size)
{
    const size_t from_size = strnlen(entry->from, PATH_MAX) + 1;
    const size_t to_size = strnlen(entry->to, PATH_MAX) + 1;
    if (from_size <= 1 || to_size <= 1 ||
        from_size > PATH_MAX || to_size > PATH_MAX ||
        entry->old_size > sizeof(entry->old_dest) ||
        entry->victim_old_size > sizeof(entry->victim_old) ||
        (entry->victim_preclean && !entry->victim) ||
        (!entry->victim_preclean && entry->victim_old_size)) {
        errno = EINVAL;
        return false;
    }
    memset(bytes, 0, DOS83_WAL_HEADER);
    memcpy(bytes, DOS83_WAL_MAGIC, 8);
    const uint64_t ids[] = {
        entry->root_dev, entry->root_ino, entry->file_dev, entry->file_ino,
        entry->from_dir_dev, entry->from_dir_ino,
        entry->to_dir_dev, entry->to_dir_ino,
        entry->victim_dev, entry->victim_ino
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        wal_put64(bytes + 16 + 8 * i, ids[i]);
    wal_put32(bytes + 96, (entry->staged ? 1u : 0u) |
                             (entry->victim ? 2u : 0u) |
                             (entry->victim_preclean ? 4u : 0u));
    wal_put32(bytes + 100, (uint32_t)entry->old_size);
    wal_put32(bytes + 104, (uint32_t)from_size);
    wal_put32(bytes + 108, (uint32_t)to_size);
    memcpy(bytes + 112, entry->old_dest, 14);
    memcpy(bytes + 126, entry->alias, 14);
    memcpy(bytes + 140, entry->victim_old, sizeof(entry->victim_old));
    wal_put32(bytes + 154, (uint32_t)entry->victim_old_size);
    memcpy(bytes + DOS83_WAL_HEADER, entry->from, from_size);
    memcpy(bytes + DOS83_WAL_HEADER + from_size, entry->to, to_size);
    *size = DOS83_WAL_HEADER + from_size + to_size;
    wal_put64(bytes + 8, wal_hash(bytes + 16, *size - 16));
    return true;
}

static bool wal_write(struct dos83_wal *wal,
                      const struct dos83_wal_entry *entry)
{
    unsigned char bytes[DOS83_WAL_CAP];
    size_t size;
    if (!wal_empty(wal) || !wal_encode(entry, bytes, &size))
        return false;
    size_t done = 0;
    while (done < size) {
        ssize_t written = pwrite(wal->fd, bytes + done, size - done,
                                 (off_t)done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (written == 0)
                errno = EIO;
            return wal_poison(errno);
        }
        done += (size_t)written;
    }
    if (ftruncate(wal->fd, (off_t)size) != 0)
        return wal_poison(errno);
    if (dos83_test_io_error("wal-write-fsync") || fsync(wal->fd) != 0)
        return wal_poison(errno);
    return true;
}

/* Returns 1 if a verified pending record exists, 0 if empty, -1 on error.
 * Corrupt, truncated and future-version records are NEVER discarded. */
static int wal_read(struct dos83_wal *wal, struct dos83_wal_entry *entry)
{
    struct stat st;
    if (fstat(wal->fd, &st) != 0)
        return -1;
    if (st.st_size == 0)
        return 0;
    if (st.st_size < DOS83_WAL_V1_HEADER + 4 ||
        st.st_size > DOS83_WAL_CAP) {
        errno = EUCLEAN;
        return -1;
    }
    unsigned char bytes[DOS83_WAL_CAP];
    size_t done = 0, size = (size_t)st.st_size;
    while (done < size) {
        ssize_t got = pread(wal->fd, bytes + done, size - done, (off_t)done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            errno = EUCLEAN;
            return -1;
        }
        done += (size_t)got;
    }
    const bool version2 = memcmp(bytes, DOS83_WAL_MAGIC, 8) == 0;
    const bool version1 = memcmp(bytes, DOS83_WAL_MAGIC_V1, 8) == 0;
    const size_t header = version2 ? DOS83_WAL_HEADER : DOS83_WAL_V1_HEADER;
    const uint32_t flags = wal_get32(bytes + 96);
    const uint32_t old_size = wal_get32(bytes + 100);
    const uint32_t victim_old_size = version2 ? wal_get32(bytes + 154) : 0;
    const uint32_t from_size = wal_get32(bytes + 104);
    const uint32_t to_size = wal_get32(bytes + 108);
    if ((!version1 && !version2) || size < header + 4 ||
        wal_get64(bytes + 8) != wal_hash(bytes + 16, size - 16) ||
        (flags & (version2 ? ~7u : ~3u)) != 0 ||
        old_size > 14 || victim_old_size > 14 ||
        ((flags & 4u) && !(flags & 2u)) ||
        (!(flags & 4u) && victim_old_size) ||
        (version2 && (bytes[158] != 0 || bytes[159] != 0)) ||
        from_size < 2 || to_size < 2 ||
        from_size > PATH_MAX || to_size > PATH_MAX ||
        (size_t)from_size + (size_t)to_size != size - header ||
        bytes[header + from_size - 1] != '\0' ||
        bytes[size - 1] != '\0' ||
        memchr(bytes + header, 0, from_size - 1) ||
        memchr(bytes + header + from_size, 0, to_size - 1)) {
        errno = EUCLEAN;
        return -1;
    }
    *entry = (struct dos83_wal_entry){0};
    uint64_t *ids[] = {
        &entry->root_dev, &entry->root_ino,
        &entry->file_dev, &entry->file_ino,
        &entry->from_dir_dev, &entry->from_dir_ino,
        &entry->to_dir_dev, &entry->to_dir_ino,
        &entry->victim_dev, &entry->victim_ino
    };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i)
        *ids[i] = wal_get64(bytes + 16 + 8 * i);
    entry->staged = (flags & 1u) != 0;
    entry->victim = (flags & 2u) != 0;
    entry->victim_preclean = (flags & 4u) != 0;
    entry->victim_old_size = victim_old_size;
    if (version2)
        memcpy(entry->victim_old, bytes + 140, sizeof(entry->victim_old));
    entry->old_size = old_size;
    memcpy(entry->old_dest, bytes + 112, 14);
    memcpy(entry->alias, bytes + 126, 14);
    if (entry->staged &&
        (!memchr(entry->alias, '\0', sizeof(entry->alias)) ||
         entry->alias[0] == '\0')) {
        errno = EUCLEAN;
        return -1;
    }
    memcpy(entry->from, bytes + header, from_size);
    memcpy(entry->to, bytes + header + from_size, to_size);
    return 1;
}

/* Pre-clearing a replaced inode's alias before rename makes its removal
 * durable even when the overwritten file has hardlinks OUTSIDE this grant.
 * On abort (source still at its old name), restore only an exact known
 * preimage or our own absent postimage. Unknown edits fail closed. */
static bool wal_restore_victim_alias(int victim_fd, const char *key,
                                     const struct dos83_wal_entry *record)
{
    unsigned char current[14] = {0};
    const ssize_t n = read_guest_xattr(victim_fd, key, current,
                                       sizeof(current));
    const bool absent = n < 0 && errno == ENODATA;
    const bool old = record->victim_old_size > 0 &&
                     n == (ssize_t)record->victim_old_size &&
                     memcmp(current, record->victim_old,
                            record->victim_old_size) == 0;
    if (!absent && !old) {
        errno = EUCLEAN;
        return false;
    }
    if (record->victim_old_size)
        return write_guest_xattr(victim_fd, key, record->victim_old,
                                 record->victim_old_size) == 0;
    return clear_guest_xattr(victim_fd, key) == 0;
}

/* A crash may happen before *or* after staging. An external writer may
 * also touch the destination-entry xattr while the WAL is pending. Only
 * restore the preimage if the current value is demonstrably one of the two
 * values that our transaction could have written. Otherwise preserve the
 * unknown value, keep the WAL and require manual reconciliation.
 *
 * Comparing raw bytes and lengths matters: strcmp on a short/unterminated
 * xattr is insufficient for proof of identity. */
static bool wal_preimage_or_staged(int pinned, const char *key,
                                   const struct dos83_wal_entry *record)
{
    unsigned char current[14] = {0};
    const ssize_t n = read_guest_xattr(pinned, key, current,
                                      sizeof(current));
    if (n < 0) {
        if (errno == ENODATA && record->old_size == 0)
            return true;
        if (errno == ERANGE)
            errno = EUCLEAN;
        return false;
    }
    const size_t alias_len = strnlen(record->alias,
                                    sizeof(record->alias)) + 1;
    if (alias_len <= sizeof(record->alias) &&
        (size_t)n == alias_len &&
        memcmp(current, record->alias, alias_len) == 0)
        return true;
    if (record->old_size > 0 && (size_t)n == record->old_size &&
        memcmp(current, record->old_dest, record->old_size) == 0)
        return true;
    errno = EUCLEAN;
    return false;
}

static bool wal_same_inode(const struct stat *st, uint64_t dev, uint64_t ino)
{
    return (uint64_t)st->st_dev == dev && (uint64_t)st->st_ino == ino;
}
static bool wal_missing_or_identity(int parent, const char *leaf,
                                    struct stat *st, bool *exists)
{
    if (fstatat(parent, leaf, st, AT_SYMLINK_NOFOLLOW) == 0) {
        *exists = true;
        return true;
    }
    *exists = false;
    return errno == ENOENT;
}

/* Recovery does not guess which inode an external writer moved/reused.
 * A valid journal with undecidable physical state blocks future renames. */
static bool wal_recover_root(int root)
{
    struct dos83_wal wal;
    if (!wal_open(root, &wal))
        return false;
    struct dos83_wal_entry record;
    int state = wal_read(&wal, &record);
    if (state <= 0) {
        int error = errno;
        wal_close(&wal);
        errno = error;
        return state == 0;
    }
    struct stat root_stat;
    bool ok = fstat(root, &root_stat) == 0 &&
              wal_same_inode(&root_stat, record.root_dev, record.root_ino);
    if (!ok)
        errno = EUCLEAN;
    char from_leaf[NAME_MAX + 1], to_leaf[NAME_MAX + 1];
    int from = ok ? open_parent(root, record.from, from_leaf) : -1;
    int to = from >= 0 ? open_parent(root, record.to, to_leaf) : -1;
    if (from < 0 || to < 0)
        ok = false;
    const char *from_exact = strrchr(record.from, '/');
    const char *to_exact = strrchr(record.to, '/');
    from_exact = from_exact ? from_exact + 1 : record.from;
    to_exact = to_exact ? to_exact + 1 : record.to;
    struct stat from_dir, to_dir;
    if (ok && (fstat(from, &from_dir) != 0 ||
               fstat(to, &to_dir) != 0 ||
               !wal_same_inode(&from_dir, record.from_dir_dev,
                               record.from_dir_ino) ||
               !wal_same_inode(&to_dir, record.to_dir_dev,
                               record.to_dir_ino))) {
        errno = EUCLEAN;
        ok = false;
    }
    int gate_a = -1, gate_b = -1;
    bool same = false;
    if (ok && !dos83_lock_pair(from, to, &gate_a, &gate_b, &same))
        ok = false;
    if (ok) {
        struct stat a, b;
        bool a_exists, b_exists;
        if (!wal_missing_or_identity(from, from_exact, &a, &a_exists) ||
            !wal_missing_or_identity(to, to_exact, &b, &b_exists)) {
            ok = false;
        } else {
            bool at_source = a_exists &&
                wal_same_inode(&a, record.file_dev, record.file_ino);
            bool at_dest = b_exists &&
                wal_same_inode(&b, record.file_dev, record.file_ino);
            bool original_target = record.victim ?
                b_exists && wal_same_inode(&b, record.victim_dev,
                                           record.victim_ino) : !b_exists;
            if ((at_source && at_dest) ||
                (!at_source && !at_dest) ||
                (at_dest && a_exists) ||
                (at_source && !original_target)) {
                errno = EUCLEAN;
                ok = false;
            } else {
                int pinned = confined_open_raw(at_dest ? to : from,
                                    at_dest ? to_exact : from_exact,
                                    O_PATH | O_NOFOLLOW, 0);
                if (pinned < 0) {
                    ok = false;
                } else {
                    char source_key[112], dest_key[112];
                    ok = dos83_entry_key(from, from_exact, source_key) &&
                         dos83_entry_key(to, to_exact, dest_key);
                    /* A v2 replacement pre-cleans the victim's
                     * destination-entry xattr, even when it has hardlinks
                     * beyond the granted root. Before rename, the victim
                     * remains addressable and its exact old value can be
                     * restored. After rename, no victim mutation is needed. */
                    if (ok && at_source && record.victim_preclean) {
                        int victim_fd = confined_open_raw(to, to_exact,
                                                       O_PATH | O_NOFOLLOW, 0);
                        struct stat victim_st;
                        if (victim_fd < 0 ||
                            fstat(victim_fd, &victim_st) != 0 ||
                            !wal_same_inode(&victim_st, record.victim_dev,
                                            record.victim_ino) ||
                            !wal_restore_victim_alias(victim_fd, dest_key,
                                                      &record) ||
                            !wal_sync_pinned(victim_fd,
                                             S_ISDIR(victim_st.st_mode))) {
                            if (victim_fd >= 0)
                                close(victim_fd);
                            errno = EUCLEAN;
                            ok = false;
                        } else {
                            close(victim_fd);
                            /* Recovery itself is crashable: repeating replay
                             * after this fsync must not lose the old victim
                             * alias or trust an unrelated inode. */
                            dos83_test_crash("replay-after-victim-restore");
                        }
                    }
                    if (ok && record.staged) {
                        if (at_dest) {
                            char actual[14] = {0};
                            const ssize_t n = read_guest_xattr(pinned,
                                                     dest_key, actual, sizeof(actual));
                            const size_t wanted = strnlen(record.alias,
                                                            sizeof(record.alias)) + 1;
                            if (wanted > sizeof(record.alias) ||
                                n != (ssize_t)wanted ||
                                memcmp(record.alias, actual, wanted) != 0) {
                                /* A damaged/unterminated xattr is not
                                 * evidence of the staged short identity. */
                                errno = EUCLEAN;
                                ok = false;
                            } else if (strcmp(source_key, dest_key) != 0 &&
                                       clear_guest_xattr(pinned, source_key) != 0)
                                ok = false;
                        } else if (!wal_preimage_or_staged(pinned, dest_key,
                                                               &record)) {
                            /* Not ours to overwrite: fail closed. */
                            ok = false;
                        } else if (record.old_size > 0) {
                            ok = write_guest_xattr(pinned, dest_key,
                                        record.old_dest, record.old_size) == 0;
                        } else {
                            ok = clear_guest_xattr(pinned, dest_key) == 0;
                        }
                    }
                    if (ok)
                        ok = wal_sync_pinned(pinned, S_ISDIR(at_dest ? b.st_mode : a.st_mode));
                    if (ok && record.staged)
                        dos83_test_crash("replay-after-source-restore");
                    close(pinned);
                }
                if (ok)
                    ok = wal_sync_pinned(from, true) &&
                         wal_sync_pinned(to, true) && wal_clear(&wal);
            }
        }
    }
    int error = ok ? 0 : errno;
    if (gate_b >= 0)
        close(gate_b);
    if (gate_a >= 0)
        close(gate_a);
    if (to >= 0)
        close(to);
    if (from >= 0)
        close(from);
    wal_close(&wal);
    errno = error ? error : 0;
    return ok;
}

bool nt_runtime_recover_dos83(void)
{
    pthread_mutex_lock(&root_lock);
    int root = drive_root >= 0 ? fcntl(drive_root, F_DUPFD_CLOEXEC, 0) : -1;
    pthread_mutex_unlock(&root_lock);
    if (root < 0) {
        errno = EACCES;
        return false;
    }
    pthread_mutex_lock(&dos83_lock);
    bool ok = wal_recover_root(root);
    int error = errno;
    pthread_mutex_unlock(&dos83_lock);
    /* A crashed final HANDLE holder may leave a durable delete disposition
     * but no path through which another opener could find its inode gate.
     * Reap these records before declaring startup storage clean. */
    if (ok && !fs4_recover_pending_deletes(root)) {
        ok = false;
        error = errno;
    }
    close(root);
    errno = error;
    return ok;
}

/* A drive, UNC share or redirected known-folder is an independent physical
 * grant. Do not silently start PE execution against a pending WAL on D: or
 * another root merely because C: was clean. Absence of the journal is fine,
 * including for read-only grants; a present corrupt or unwritable pending
 * record is not. This is startup preflight, NOT a runtime global write lock. */
static bool wal_recover_if_pending(int root)
{
    int directory = confined_open_raw(root, ":ntnative-file-leases",
                                       O_RDONLY | O_DIRECTORY, 0);
    if (directory < 0)
        return errno == ENOENT;
    struct stat dir_info;
    if (fstat(directory, &dir_info) != 0 ||
        !S_ISDIR(dir_info.st_mode) ||
        dir_info.st_uid != geteuid() ||
        (dir_info.st_mode & 077u) != 0) {
        int error = errno ? errno : EACCES;
        close(directory);
        errno = error;
        return false;
    }
    int wal_file = confined_open_raw(directory, DOS83_WAL_NAME, O_RDONLY, 0);
    const int open_error = errno;
    close(directory);
    if (wal_file < 0)
        return open_error == ENOENT;
    struct stat info;
    const bool valid = fstat(wal_file, &info) == 0 &&
                       S_ISREG(info.st_mode) && info.st_nlink == 1 &&
                       info.st_uid == geteuid() &&
                       (info.st_mode & 077u) == 0;
    const off_t pending = valid ? info.st_size : -1;
    const int error = errno;
    if (!valid) {
        close(wal_file);
        errno = error ? error : EACCES;
        return false;
    }
    /* A fresh service/PE process cannot trust st_size==0 alone: the prior
     * process could have truncated WAL before a failed fsync. Force the
     * journal inode durable before treating it as empty, even in an
     * all-grant read-only preflight that does not call wal_open(). */
    if (fsync(wal_file) != 0) {
        const int cause = errno;
        close(wal_file);
        return wal_poison(cause);
    }
    close(wal_file);
    if (pending == 0)
        return true;
    pthread_mutex_lock(&dos83_lock);
    const bool ok = wal_recover_root(root);
    const int recovery_error = errno;
    pthread_mutex_unlock(&dos83_lock);
    errno = recovery_error;
    return ok;
}

bool nt_runtime_recover_all_dos83_grants(void)
{
    enum { CAP = 1 + 26 + UNC_LIMIT + NT_USER_FOLDER_COUNT };
    int grants[CAP];
    size_t count = 0;
    bool copied = true;
    int error = 0;
    pthread_mutex_lock(&root_lock);
    if (drive_root < 0) {
        copied = false;
        error = EACCES;
    }
#define DOS83_WAL_DUP_GRANT(fd_) do { \
    if (copied) { \
        int copy_ = fcntl((fd_), F_DUPFD_CLOEXEC, 0); \
        if (copy_ < 0) { copied = false; error = errno; } \
        else grants[count++] = copy_; \
    } \
} while (0)
    if (drive_root >= 0)
        DOS83_WAL_DUP_GRANT(drive_root);
    for (size_t i = 0; i < 26; ++i)
        if (drive_roots[i].active)
            DOS83_WAL_DUP_GRANT(drive_roots[i].fd);
    for (size_t i = 0; i < UNC_LIMIT; ++i)
        if (unc_roots[i].active)
            DOS83_WAL_DUP_GRANT(unc_roots[i].fd);
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i)
        if (folder_roots[i].active)
            DOS83_WAL_DUP_GRANT(folder_roots[i].fd);
#undef DOS83_WAL_DUP_GRANT
    pthread_mutex_unlock(&root_lock);
    if (copied) {
        for (size_t i = 0; i < count; ++i) {
            if (!wal_recover_if_pending(grants[i])) {
                copied = false;
                error = errno;
                break;
            }
        }
    }
    if (copied && count > 0 &&
        !fs4_recover_pending_deletes(grants[0])) {
        copied = false;
        error = errno;
    }
    for (size_t i = 0; i < count; ++i)
        close(grants[i]);
    if (!copied)
        errno = error ? error : EIO;
    return copied;
}
