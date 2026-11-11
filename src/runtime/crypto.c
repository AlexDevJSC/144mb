/* SPDX-License-Identifier: MIT */
#include "crypto.h"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <pthread.h>
#include <string.h>

#define INVALID_PARAMETER ((int32_t)UINT32_C(0xc000000d))
#define INVALID_HANDLE ((int32_t)UINT32_C(0xc0000008))
#define BUFFER_TOO_SMALL ((int32_t)UINT32_C(0xc0000023))
#define NOT_SUPPORTED ((int32_t)UINT32_C(0xc00000bb))
#define NO_MEMORY ((int32_t)UINT32_C(0xc0000017))
#define UNSUCCESSFUL ((int32_t)UINT32_C(0xc0000001))

enum { HMAC_FLAG = 8, HASH_OBJECT_BYTES = 64, HANDLE_COUNT = 256 };
struct algorithm {
    char id[8];
    const EVP_MD *(*digest)(void);
};

static const struct algorithm algorithms[] = {
    {"MD5", EVP_md5}, {"SHA1", EVP_sha1}, {"SHA256", EVP_sha256},
    {"SHA384", EVP_sha384}, {"SHA512", EVP_sha512}
};

struct hash_handle {
    uintptr_t id;
    const struct algorithm *algorithm;
    EVP_MD_CTX *md;
    EVP_MAC_CTX *mac;
    bool provider, keyed, finished;
};

static struct hash_handle handles[HANDLE_COUNT];
static uintptr_t next_handle = 0x74000001;
static pthread_mutex_t hash_lock = PTHREAD_MUTEX_INITIALIZER;

static bool wide_equals(const uint16_t *wide, const char *ascii)
{
    if (wide == NULL)
        return false;
    while (*ascii != '\0' && *wide == (unsigned char)*ascii) {
        ++wide;
        ++ascii;
    }
    return *ascii == '\0' && *wide == 0;
}

static struct hash_handle *find_handle(uintptr_t id)
{
    if (id != 0) {
        for (size_t i = 0; i < HANDLE_COUNT; ++i)
            if (handles[i].id == id)
                return &handles[i];
    }
    return NULL;
}

static struct hash_handle *new_handle(const struct algorithm *algorithm, bool provider, bool keyed)
{
    if (next_handle == UINTPTR_MAX)
        return NULL;
    for (size_t i = 0; i < HANDLE_COUNT; ++i) {
        if (handles[i].id == 0) {
            handles[i] = (struct hash_handle){
                .id = next_handle++, .algorithm = algorithm, .provider = provider, .keyed = keyed
            };
            return &handles[i];
        }
    }
    return NULL;
}

int32_t NT_MS_ABI nt_BCryptOpenAlgorithmProvider(uintptr_t *handle, const uint16_t *id,
                                                const uint16_t *implementation, uint32_t flags)
{
    if (handle == NULL || id == NULL)
        return INVALID_PARAMETER;
    if ((flags & ~HMAC_FLAG) != 0 ||
        (implementation != NULL && !wide_equals(implementation, "Microsoft Primitive Provider")))
        return NOT_SUPPORTED;
    const struct algorithm *algorithm = NULL;
    for (size_t i = 0; i < sizeof(algorithms) / sizeof(algorithms[0]); ++i) {
        if (wide_equals(id, algorithms[i].id)) {
            algorithm = &algorithms[i];
            break;
        }
    }
    if (algorithm == NULL)
        return NOT_SUPPORTED;
    pthread_mutex_lock(&hash_lock);
    const struct hash_handle *provider = new_handle(algorithm, true, (flags & HMAC_FLAG) != 0);
    if (provider != NULL)
        *handle = provider->id;
    pthread_mutex_unlock(&hash_lock);
    return provider != NULL ? 0 : NO_MEMORY;
}

int32_t NT_MS_ABI nt_BCryptGetProperty(uintptr_t handle, const uint16_t *property,
                                      unsigned char *buffer, uint32_t size, uint32_t *needed, uint32_t flags)
{
    if (property == NULL || needed == NULL || flags != 0)
        return INVALID_PARAMETER;
    pthread_mutex_lock(&hash_lock);
    const struct hash_handle *entry = find_handle(handle);
    const struct algorithm *algorithm = entry != NULL ? entry->algorithm : NULL;
    pthread_mutex_unlock(&hash_lock);
    if (algorithm == NULL)
        return INVALID_HANDLE;

    uint32_t number = 0;
    uint16_t name[sizeof(algorithm->id)];
    const void *value = &number;
    uint32_t length = sizeof(number);
    if (wide_equals(property, "ObjectLength"))
        number = HASH_OBJECT_BYTES;
    else if (wide_equals(property, "HashDigestLength"))
        number = (uint32_t)EVP_MD_get_size(algorithm->digest());
    else if (wide_equals(property, "HashBlockLength"))
        number = (uint32_t)EVP_MD_get_block_size(algorithm->digest());
    else if (wide_equals(property, "AlgorithmName")) {
        length = (uint32_t)((strlen(algorithm->id) + 1) * sizeof(*name));
        for (size_t i = 0; i < length / sizeof(*name); ++i)
            name[i] = (unsigned char)algorithm->id[i];
        value = name;
    } else {
        return NOT_SUPPORTED;
    }
    *needed = length;
    if (buffer == NULL)
        return 0;
    if (size < length)
        return BUFFER_TOO_SMALL;
    memcpy(buffer, value, length);
    return 0;
}

int32_t NT_MS_ABI nt_BCryptCloseAlgorithmProvider(uintptr_t handle, uint32_t flags)
{
    if (flags != 0)
        return INVALID_PARAMETER;
    pthread_mutex_lock(&hash_lock);
    struct hash_handle *entry = find_handle(handle);
    const bool valid = entry != NULL && entry->provider;
    if (valid)
        entry->id = 0;
    pthread_mutex_unlock(&hash_lock);
    return valid ? 0 : INVALID_HANDLE;
}

static bool init_hash(struct hash_handle *entry, const unsigned char *key, uint32_t size)
{
    if (!entry->keyed) {
        entry->md = EVP_MD_CTX_new();
        return entry->md != NULL && EVP_DigestInit_ex(entry->md, entry->algorithm->digest(), NULL) == 1;
    }
    EVP_MAC *hmac = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (hmac == NULL)
        return false;
    entry->mac = EVP_MAC_CTX_new(hmac);
    EVP_MAC_free(hmac);
    char name[sizeof(entry->algorithm->id)];
    memcpy(name, entry->algorithm->id, sizeof(name));
    const OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, name, 0),
        OSSL_PARAM_construct_end()
    };
    return entry->mac != NULL &&
           EVP_MAC_init(entry->mac, key != NULL ? key : (const unsigned char *)"", size, parameters) == 1;
}

static void release_hash(struct hash_handle *entry)
{
    EVP_MD_CTX_free(entry->md);
    EVP_MAC_CTX_free(entry->mac);
    *entry = (struct hash_handle){0};
}

int32_t NT_MS_ABI nt_BCryptCreateHash(uintptr_t algorithm, uintptr_t *handle, unsigned char *object,
                                      uint32_t object_size, const unsigned char *key, uint32_t key_size,
                                      uint32_t flags)
{
    if (handle == NULL || flags != 0 || (key == NULL && key_size != 0) ||
        (object == NULL && object_size != 0))
        return INVALID_PARAMETER;
    if (object != NULL && object_size < HASH_OBJECT_BYTES)
        return BUFFER_TOO_SMALL;
    pthread_mutex_lock(&hash_lock);
    struct hash_handle *provider = find_handle(algorithm);
    int32_t result = INVALID_HANDLE;
    if (provider != NULL && provider->provider) {
        if (!provider->keyed && (key != NULL || key_size != 0)) {
            result = INVALID_PARAMETER;
        } else {
            struct hash_handle *entry = new_handle(provider->algorithm, false, provider->keyed);
            result = NO_MEMORY;
            if (entry != NULL) {
                if (init_hash(entry, key, key_size)) {
                    *handle = entry->id;
                    result = 0;
                } else {
                    release_hash(entry);
                    result = UNSUCCESSFUL;
                }
            }
        }
    }
    pthread_mutex_unlock(&hash_lock);
    return result;
}

int32_t NT_MS_ABI nt_BCryptHashData(uintptr_t handle, const unsigned char *data, uint32_t size, uint32_t flags)
{
    if ((size != 0 && data == NULL) || flags != 0)
        return INVALID_PARAMETER;
    pthread_mutex_lock(&hash_lock);
    struct hash_handle *entry = find_handle(handle);
    int32_t result = INVALID_HANDLE;
    if (entry != NULL && !entry->provider && !entry->finished) {
        const bool ok = size == 0 || (entry->keyed ? EVP_MAC_update(entry->mac, data, size) :
                                                    EVP_DigestUpdate(entry->md, data, size)) == 1;
        result = ok ? 0 : UNSUCCESSFUL;
    }
    pthread_mutex_unlock(&hash_lock);
    return result;
}

int32_t NT_MS_ABI nt_BCryptFinishHash(uintptr_t handle, unsigned char *digest, uint32_t size, uint32_t flags)
{
    if (digest == NULL || flags != 0)
        return INVALID_PARAMETER;
    pthread_mutex_lock(&hash_lock);
    struct hash_handle *entry = find_handle(handle);
    int32_t result = INVALID_HANDLE;
    if (entry != NULL && !entry->provider && !entry->finished) {
        if (size != (uint32_t)EVP_MD_get_size(entry->algorithm->digest())) {
            result = INVALID_PARAMETER;
        } else {
            unsigned char value[EVP_MAX_MD_SIZE];
            size_t mac_size;
            unsigned int hash_size;
            const bool ok = entry->keyed ?
                EVP_MAC_final(entry->mac, value, &mac_size, sizeof(value)) == 1 && mac_size == size :
                EVP_DigestFinal_ex(entry->md, value, &hash_size) == 1 && hash_size == size;
            entry->finished = true;
            if (ok)
                memcpy(digest, value, size);
            result = ok ? 0 : UNSUCCESSFUL;
        }
    }
    pthread_mutex_unlock(&hash_lock);
    return result;
}

int32_t NT_MS_ABI nt_BCryptDestroyHash(uintptr_t handle)
{
    pthread_mutex_lock(&hash_lock);
    struct hash_handle *entry = find_handle(handle);
    const bool valid = entry != NULL && !entry->provider;
    if (valid)
        release_hash(entry);
    pthread_mutex_unlock(&hash_lock);
    return valid ? 0 : INVALID_HANDLE;
}
