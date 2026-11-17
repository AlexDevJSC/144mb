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
static int clear_guest_xattr(int fd, const char *name);
static int open_share_registry(const struct stat *info);
static pthread_mutex_t dos83_lock = PTHREAD_MUTEX_INITIALIZER;
/* An alias is durable only after its owner inode's metadata fsync.
 * wal_sync_pinned is defined in the versioned journal include below. */
static bool wal_sync_pinned(int path_fd, bool directory);

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

static int dos83_collision_except(int parent, const char *owner,
                                  const char *ignore, const char *alias)
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
            !strcmp(entry->d_name, owner) ||
            (ignore && !strcmp(entry->d_name, ignore)))
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

static int dos83_collision(int parent, const char *owner, const char *alias)
{
    return dos83_collision_except(parent, owner, NULL, alias);
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
                    dos83_test_crash("alias-after-write");
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
    /* An xattr write that has reached the page cache is not a durable
     * alias. A successful refresh/lookup must fsync the pinned owner,
     * INCLUDING when retrying after an earlier failed sync. Never claim
     * success if this barrier fails. */
    if (!error) {
        if (!wal_sync_pinned(fd, S_ISDIR(st.st_mode)))
            error = errno;
        else
            dos83_test_crash("alias-after-fsync");
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


/* Directory-level gate shared between Win32 and the host reconciliation
 * endpoint. Lock (dev,ino) in canonical order to avoid ABBA deadlocks. */
static bool dos83_lock_pair(int source_parent, int dest_parent,
                            int *gate_a, int *gate_b, bool *same_dir)
{
    struct stat source, dest;
    *gate_a = -1;
    *gate_b = -1;
    if (fstat(source_parent, &source) != 0 ||
        fstat(dest_parent, &dest) != 0)
        return false;
    *same_dir = source.st_dev == dest.st_dev && source.st_ino == dest.st_ino;
    int source_gate = open_share_registry(&source);
    if (source_gate < 0)
        return false;
    int dest_gate = *same_dir ? -1 : open_share_registry(&dest);
    if (!*same_dir && dest_gate < 0) {
        int error = errno;
        close(source_gate);
        errno = error;
        return false;
    }
    if (!*same_dir &&
        ((uint64_t)source.st_dev > (uint64_t)dest.st_dev ||
         (source.st_dev == dest.st_dev &&
          (uint64_t)source.st_ino > (uint64_t)dest.st_ino))) {
        *gate_a = dest_gate;
        *gate_b = source_gate;
    } else {
        *gate_a = source_gate;
        *gate_b = dest_gate;
    }
    if (flock(*gate_a, LOCK_EX) != 0 ||
        (*gate_b >= 0 && flock(*gate_b, LOCK_EX) != 0)) {
        int error = errno;
        if (*gate_b >= 0)
            close(*gate_b);
        close(*gate_a);
        *gate_a = -1;
        *gate_b = -1;
        errno = error;
        return false;
    }
    return true;
}

/* Startup WAL admission also reaps durable FILE_DELETE_PENDING markers.
 * Defined in guest_files.c after the per-inode registry helpers. */
static bool fs4_recover_pending_deletes(int c_root);
#include "dos83_wal.inc"

/* Cooperating rename transaction. Stage the new xattr on the pinned
 * source inode BEFORE renameat2: if the syscall commits, the new entry
 * immediately has its alias. Release locks only after cleanup/rollback.
 * Linux writers which bypass the broker remain outside atomic guarantees. */
struct dos83_move_txn {
    int source_fd, victim_fd, gate_a, gate_b;
    int source_parent, dest_parent;
    struct dos83_wal wal;
    char source_key[112], dest_key[112], old_dest[14];
    ssize_t old_dest_size, victim_old_size;
    char victim_old[14], victim_key[112];
    bool staged, has_source_alias, journaled, victim_preclean;
};

static bool dos83_move_end(struct dos83_move_txn *txn, bool committed)
{
    const int saved_errno = errno;
    bool ok = true;
    int error = 0;
    if (txn->source_fd >= 0) {
        if (txn->staged) {
            if (committed) {
                if (strcmp(txn->source_key, txn->dest_key) != 0 &&
                    clear_guest_xattr(txn->source_fd, txn->source_key) != 0)
                    ok = false;
            } else if (txn->old_dest_size > 0) {
                if (write_guest_xattr(txn->source_fd, txn->dest_key,
                                      txn->old_dest,
                                      (size_t)txn->old_dest_size) != 0)
                    ok = false;
            } else if (clear_guest_xattr(txn->source_fd,
                                          txn->dest_key) != 0) {
                ok = false;
            }
            if (!ok)
                error = errno;
        }
        if (ok && txn->journaled) {
            struct stat st;
            if (fstat(txn->source_fd, &st) != 0 ||
                !wal_sync_pinned(txn->source_fd, S_ISDIR(st.st_mode))) {
                ok = false;
                error = errno;
            }
        }
    }
    if (txn->victim_fd >= 0 && txn->journaled &&
        txn->victim_preclean && !committed) {
        /* Roll back metadata staged on a hardlinked replacement victim.
         * Check against the two states our transaction can have written;
         * a foreign value is NOT ours to overwrite. */
        struct dos83_wal_entry original = {
            .victim_old_size = txn->victim_old_size > 0 ?
                               (size_t)txn->victim_old_size : 0
        };
        if (original.victim_old_size)
            memcpy(original.victim_old, txn->victim_old,
                   original.victim_old_size);
        struct stat st;
        if (fstat(txn->victim_fd, &st) != 0 ||
            !wal_restore_victim_alias(txn->victim_fd, txn->victim_key,
                                      &original) ||
            !wal_sync_pinned(txn->victim_fd, S_ISDIR(st.st_mode))) {
            ok = false;
            if (!error)
                error = errno;
        }
    }
    if (txn->victim_fd >= 0 && committed && !txn->victim_preclean) {
        struct stat st;
        if (fstat(txn->victim_fd, &st) != 0) {
            ok = false;
            if (!error)
                error = errno;
        } else if (st.st_nlink > 0) {
            /* A replaced final link cannot be reopened via procfs and
             * has no durable metadata to preserve. Surviving hardlinks
             * still need the stale destination-entry xattr removed. */
            if (clear_guest_xattr(txn->victim_fd, txn->dest_key) != 0 ||
                !wal_sync_pinned(txn->victim_fd,
                                 S_ISDIR(st.st_mode))) {
                ok = false;
                if (!error)
                    error = errno;
            }
        }
    }
    /* Both directory entries and xattrs must reach stable storage before
     * clearing the replay record. A failure leaves the journal intact. */
    if (txn->journaled && ok && committed)
        dos83_test_crash("after-cleanup");
    if (txn->journaled && ok) {
        /* Keep the barriers explicit so the test-only mediator can die
         * between each durable step, without changing release behavior.
         * Recovery must be correct with a pending WAL after either
         * directory fsync; after a synced clear it must be a no-op. */
        if (!wal_sync_pinned(txn->source_parent, true)) {
            ok = false;
            error = errno;
        } else {
            if (committed)
                dos83_test_crash("after-source-parent-fsync");
            if (!wal_sync_pinned(txn->dest_parent, true)) {
                ok = false;
                error = errno;
            } else {
                if (committed)
                    dos83_test_crash("after-both-parents-fsync");
                if (!wal_clear(&txn->wal)) {
                    ok = false;
                    error = errno;
                } else if (committed) {
                    dos83_test_crash("after-wal-clear");
                }
            }
        }
    }
    if (txn->source_fd >= 0)
        close(txn->source_fd);
    if (txn->victim_fd >= 0)
        close(txn->victim_fd);
    if (txn->gate_b >= 0)
        close(txn->gate_b);
    if (txn->gate_a >= 0)
        close(txn->gate_a);
    wal_close(&txn->wal);
    pthread_mutex_unlock(&dos83_lock);
    errno = ok ? saved_errno : (error ? error : EIO);
    return ok;
}

/* Both parents are already pinned and the caller holds its file-lock.
 * The same gate order is used by concurrent NT rename workers, including
 * opposite-direction moves. Return a locked transaction to be finished
 * on success OR failure of the following renameat2 call. */
static bool dos83_move_begin(int root, const char *from_path,
                             const char *to_path,
                             int source_parent, const char *source_leaf,
                             int dest_parent, const char *dest_leaf,
                             const struct stat *expected_source,
                             bool replacing,
                             struct dos83_move_txn *txn)
{
    *txn = (struct dos83_move_txn){
        .source_fd = -1, .victim_fd = -1, .gate_a = -1,
        .gate_b = -1, .old_dest_size = -1, .victim_old_size = -1,
        .source_parent = source_parent, .dest_parent = dest_parent,
        .wal = {.fd = -1, .directory = -1}
    };
    pthread_mutex_lock(&dos83_lock);
    /* One WAL per physical grant; the global lock comes BEFORE the
     * directory locks in both normal operation and crash recovery. */
    int error = wal_open(root, &txn->wal) ? 0 : errno;
    if (!error && !wal_empty(&txn->wal))
        error = errno;
    bool same_dir = false;
    if (!error && !dos83_lock_pair(source_parent, dest_parent,
                         &txn->gate_a, &txn->gate_b, &same_dir))
        error = errno;
    if (!error) {
        txn->source_fd = confined_open_raw(source_parent, source_leaf,
                                           O_PATH | O_NOFOLLOW, 0);
        if (txn->source_fd < 0)
            error = errno;
    }
    struct stat pinned;
    if (!error && (fstat(txn->source_fd, &pinned) != 0 ||
                   pinned.st_dev != expected_source->st_dev ||
                   pinned.st_ino != expected_source->st_ino))
        error = EBUSY;
    if (!error && (!dos83_entry_key(source_parent, source_leaf,
                                   txn->source_key) ||
                   !dos83_entry_key(dest_parent, dest_leaf, txn->dest_key)))
        error = errno;
    if (!error && replacing) {
        txn->victim_fd = confined_open_raw(dest_parent, dest_leaf,
                                           O_PATH | O_NOFOLLOW, 0);
        if (txn->victim_fd < 0)
            error = errno;
        else {
            struct stat victim;
            if (fstat(txn->victim_fd, &victim) != 0 ||
                (victim.st_dev == pinned.st_dev &&
                 victim.st_ino == pinned.st_ino))
                error = EBUSY;
        }
    }
    if (!error && replacing) {
        if (!dos83_entry_key(dest_parent, dest_leaf, txn->victim_key)) {
            error = errno;
        } else {
            txn->victim_old_size = read_guest_xattr(
                txn->victim_fd, txn->victim_key,
                txn->victim_old, sizeof(txn->victim_old));
            if (txn->victim_old_size < 0) {
                if (errno != ENODATA)
                    error = errno == ERANGE ? EUCLEAN : errno;
                else
                    txn->victim_old_size = 0;
            }
        }
    }
    char alias[14] = {0};
    if (!error) {
        const ssize_t count = read_guest_xattr(txn->source_fd,
                                                txn->source_key, alias,
                                                sizeof(alias));
        if (count < 0) {
            /* ENODATA means no alias was ever assigned; an unreadable,
             * oversized or unsupported attribute is NOT proof of absence. */
            if (errno != ENODATA)
                error = errno == ERANGE ? EUCLEAN : errno;
        } else if (count < 3 || count > 13 ||
                   alias[count - 1] != '\0' ||
                   memchr(alias, '\0', (size_t)count - 1) != NULL ||
                   memchr(alias, '~', (size_t)count - 1) == NULL) {
            error = EUCLEAN;
        } else {
            txn->has_source_alias = true;
            for (ssize_t i = 0; i < count - 1; ++i) {
                if (!dos83_char((unsigned char)alias[i]) &&
                    alias[i] != '.' && alias[i] != '~') {
                    error = EUCLEAN;
                    break;
                }
            }
        }
    }
    char alias_for_move[14] = {0};
    const bool needs_staging = txn->has_source_alias &&
                               strcmp(txn->source_key, txn->dest_key) != 0;
    if (!error && needs_staging) {
        const char *ignore = same_dir ? source_leaf : NULL;
        int collision = dos83_collision_except(dest_parent, dest_leaf,
                                                ignore, alias);
        if (collision < 0)
            error = errno;
        if (!error && collision != 0) {
            bool allocated = false;
            for (unsigned int n = 1; n < 1000000; ++n) {
                dos83_make(dest_leaf, n, alias);
                collision = dos83_collision_except(dest_parent, dest_leaf,
                                                    ignore, alias);
                if (collision < 0) {
                    error = errno;
                    break;
                }
                if (!collision) {
                    allocated = true;
                    break;
                }
            }
            if (!allocated && !error)
                error = ENOSPC;
        }
        if (!error) {
            txn->old_dest_size = read_guest_xattr(txn->source_fd,
                         txn->dest_key, txn->old_dest, sizeof(txn->old_dest));
            if (txn->old_dest_size < 0 && errno != ENODATA)
                error = errno;
        }
        if (!error)
            memcpy(alias_for_move, alias, sizeof(alias_for_move));
    }
    if (!error) {
        struct stat root_st, from_st, to_st, victim_st = {0};
        if (fstat(root, &root_st) != 0 ||
            fstat(source_parent, &from_st) != 0 ||
            fstat(dest_parent, &to_st) != 0 ||
            (replacing && fstat(txn->victim_fd, &victim_st) != 0)) {
            error = errno;
        } else {
            struct dos83_wal_entry record = {
                .root_dev = (uint64_t)root_st.st_dev,
                .root_ino = (uint64_t)root_st.st_ino,
                .file_dev = (uint64_t)pinned.st_dev,
                .file_ino = (uint64_t)pinned.st_ino,
                .from_dir_dev = (uint64_t)from_st.st_dev,
                .from_dir_ino = (uint64_t)from_st.st_ino,
                .to_dir_dev = (uint64_t)to_st.st_dev,
                .to_dir_ino = (uint64_t)to_st.st_ino,
                .victim_dev = (uint64_t)victim_st.st_dev,
                .victim_ino = (uint64_t)victim_st.st_ino,
                .victim = replacing,
                .victim_preclean = replacing,
                .victim_old_size = replacing ? (size_t)txn->victim_old_size : 0,
                .staged = needs_staging,
                .old_size = txn->old_dest_size > 0 ?
                            (size_t)txn->old_dest_size : 0
            };
            if (record.old_size)
                memcpy(record.old_dest, txn->old_dest, record.old_size);
            if (record.victim_old_size)
                memcpy(record.victim_old, txn->victim_old,
                       record.victim_old_size);
            memcpy(record.alias, alias_for_move, sizeof(record.alias));
            if (snprintf(record.from, sizeof(record.from), "%s", from_path) >=
                (int)sizeof(record.from) ||
                snprintf(record.to, sizeof(record.to), "%s", to_path) >=
                (int)sizeof(record.to)) {
                error = ENAMETOOLONG;
            } else if (!wal_write(&txn->wal, &record)) {
                error = errno;
            } else {
                txn->journaled = true;
                dos83_test_crash("after-wal");
            }
        }
    }
    if (!error && replacing) {
        /* Remove victim per-entry metadata BEFORE rename, and fsync it
         * before unlinking the last destination name. This also cleans a
         * surviving hardlink on a completely different host directory.
         * Replay of a pre-rename SIGKILL restores the exact old preimage. */
        txn->victim_preclean = true;
        struct stat victim_stat;
        if (fstat(txn->victim_fd, &victim_stat) != 0 ||
            clear_guest_xattr(txn->victim_fd, txn->victim_key) != 0 ||
            !wal_sync_pinned(txn->victim_fd,
                             S_ISDIR(victim_stat.st_mode))) {
            error = errno;
        } else {
            dos83_test_crash("after-victim-cleanup");
        }
    }
    if (!error && needs_staging) {
        /* Even if setxattr partially fails, the durable log knows the
         * pre-image, so rollback is always attempted. */
        txn->staged = true;
        if (write_guest_xattr(txn->source_fd, txn->dest_key,
                              alias, strlen(alias) + 1) != 0 ||
            !wal_sync_pinned(txn->source_fd, S_ISDIR(pinned.st_mode)))
            error = errno;
        else
            dos83_test_crash("after-staging");
    }
    if (error) {
        int cause = error;
        if (!dos83_move_end(txn, false))
            cause = errno;
        errno = cause;
        return false;
    }
    return true;
}


/* Apply a paired inotify MOVED_FROM/MOVED_TO event AFTER an external
 * rename. This is reconciliation, NOT atomic mediation of native Linux
 * operations. If a matching old per-entry key survived on the moved
 * inode, migrate its short alias only when unique at the destination.
 * Ambiguous or changed paths fail closed; normal short-name allocation
 * can then occur on a later Win32 lookup. */
static bool dos83_adopt_host_move(int source_parent, const char *old_leaf,
                                 int dest_parent, const char *new_leaf)
{
    bool same_dir = false;
    int gate_a = -1, gate_b = -1;
    pthread_mutex_lock(&dos83_lock);
    bool ok = dos83_lock_pair(source_parent, dest_parent, &gate_a,
                              &gate_b, &same_dir);
    int error = ok ? 0 : errno;
    int pinned = -1;
    if (!error) {
        struct stat former;
        if (fstatat(source_parent, old_leaf, &former,
                    AT_SYMLINK_NOFOLLOW) == 0)
            error = EBUSY; /* Source name was reused: do not guess. */
        else if (errno != ENOENT)
            error = errno;
    }
    if (!error) {
        pinned = confined_open_raw(dest_parent, new_leaf,
                                   O_PATH | O_NOFOLLOW, 0);
        if (pinned < 0)
            error = errno;
    }
    struct stat entry;
    if (!error && (fstat(pinned, &entry) != 0 ||
                   !(S_ISREG(entry.st_mode) || S_ISDIR(entry.st_mode))))
        error = EACCES;
    char source_key[112], dest_key[112], alias[14] = {0};
    if (!error &&
        (!dos83_entry_key(source_parent, old_leaf, source_key) ||
         !dos83_entry_key(dest_parent, new_leaf, dest_key)))
        error = errno;
    if (!error && strcmp(source_key, dest_key) != 0) {
        const ssize_t count = read_guest_xattr(pinned, source_key,
                                               alias, sizeof(alias));
        if (count < 3 || count > 13 || alias[count - 1] != '\0' ||
            !memchr(alias, '~', (size_t)count - 1))
            error = ENODATA;
        for (ssize_t i = 0; !error && i < count - 1; ++i)
            if (!dos83_char((unsigned char)alias[i]) &&
                alias[i] != '.' && alias[i] != '~')
                error = EINVAL;
        if (!error) {
            char existing[14] = {0};
            if (!dos83_read_at(dest_parent, new_leaf, existing)) {
                int conflict = dos83_collision_except(dest_parent, new_leaf,
                                                       NULL, alias);
                if (conflict < 0)
                    error = errno;
                if (!error && conflict != 0) {
                    bool found = false;
                    for (unsigned int n = 1; n < 1000000; ++n) {
                        dos83_make(new_leaf, n, alias);
                        conflict = dos83_collision_except(dest_parent,
                                                           new_leaf, NULL, alias);
                        if (conflict < 0) {
                            error = errno;
                            break;
                        }
                        if (conflict == 0) {
                            found = true;
                            break;
                        }
                    }
                    if (!found && !error)
                        error = ENOSPC;
                }
                if (!error && write_guest_xattr(pinned, dest_key,
                                                alias, strlen(alias) + 1) != 0)
                    error = errno;
            }
            if (!error && clear_guest_xattr(pinned, source_key) != 0)
                error = errno;
        }
    }
    if (!error && pinned >= 0) {
        struct stat current;
        if (fstatat(dest_parent, new_leaf, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            current.st_dev != entry.st_dev || current.st_ino != entry.st_ino)
            error = EBUSY;
    }
    if (pinned >= 0)
        close(pinned);
    if (gate_b >= 0)
        close(gate_b);
    if (gate_a >= 0)
        close(gate_a);
    pthread_mutex_unlock(&dos83_lock);
    errno = error;
    return error == 0;
}
