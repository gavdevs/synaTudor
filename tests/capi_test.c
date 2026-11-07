#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <openssl/rand.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "winapi/crypt/crypt.h"

__winfnc BOOL CryptAcquireContextA(struct crypt_provider **prov, const char *cont_name, const char *prov_name, DWORD prov_type, DWORD flags);
__winfnc BOOL CryptAcquireContextW(struct crypt_provider **prov, const char16_t *cont_name, const char16_t *prov_name, DWORD prov_type, DWORD flags);
__winfnc BOOL CryptReleaseContext(struct crypt_provider *prov, DWORD flags);
__winfnc BOOL CryptGenRandom(struct crypt_provider *prov, DWORD len, BYTE *buf);
__winfnc BOOL CryptCreateHash(struct crypt_provider *prov, ALG_ID alg_id, struct crypt_key *key, DWORD flags, struct crypt_hash **out);
__winfnc BOOL CryptHashData(struct crypt_hash *hash, const BYTE *data, DWORD data_len, DWORD flags);
__winfnc BOOL CryptGetHashParam(struct crypt_hash *hash, DWORD param, BYTE *data, DWORD *data_len, DWORD flags);
__winfnc BOOL CryptDestroyHash(struct crypt_hash *hash);

//Standalone tests provide only the process services used by the CAPI sources.
static DWORD test_last_error;
void __register_windows_api(struct __winapi_descr *descr) { (void) descr; }
void winerr_clear() { test_last_error = ERROR_SUCCESS; }
void winerr_set() { test_last_error = WINERR_SET_CODE; }
void winerr_set_code(int code) { test_last_error = (DWORD) code; }
void winerr_set_errno() { test_last_error = (DWORD) errno; }
int winerr_from_errno() { return errno; }
static __winfnc DWORD test_get_last_error() { return test_last_error; }
pthread_mutex_t LOG_LOCK = PTHREAD_MUTEX_INITIALIZER;
enum log_level LOG_LEVEL = LOG_ERROR;

static int random_calls, fail_random;
int __wrap_RAND_bytes_ex(OSSL_LIB_CTX *ctx, unsigned char *buf, size_t len, unsigned int strength) {
    assert(ctx == NULL);
    assert(strength == 0);
    random_calls++;
    if(fail_random) return 0;
    for(size_t i = 0; i < len; i++) buf[i] = (unsigned char) i;
    return 1;
}

typedef BOOL (__winfnc *acquire_a_fnc)(struct crypt_provider **, const char *, const char *, DWORD, DWORD);
typedef BOOL (__winfnc *acquire_w_fnc)(struct crypt_provider **, const char16_t *, const char16_t *, DWORD, DWORD);
typedef BOOL (__winfnc *release_fnc)(struct crypt_provider *, DWORD);
typedef BOOL (__winfnc *random_fnc)(struct crypt_provider *, DWORD, BYTE *);
typedef BOOL (__winfnc *create_hash_fnc)(struct crypt_provider *, ALG_ID, struct crypt_key *, DWORD, struct crypt_hash **);
typedef BOOL (__winfnc *hash_data_fnc)(struct crypt_hash *, const BYTE *, DWORD, DWORD);
typedef BOOL (__winfnc *get_hash_fnc)(struct crypt_hash *, DWORD, BYTE *, DWORD *, DWORD);
typedef BOOL (__winfnc *destroy_hash_fnc)(struct crypt_hash *);
typedef DWORD (__winfnc *get_error_fnc)(void);

struct capi_api {
    acquire_a_fnc acquire_a;
    acquire_w_fnc acquire_w;
    release_fnc release;
    random_fnc random;
    create_hash_fnc create_hash;
    hash_data_fnc hash_data;
    get_hash_fnc get_hash;
    destroy_hash_fnc destroy_hash;
    get_error_fnc get_error;
};

static void *resolved_symbol(void *resolver, void *library, const char *name) {
    void *(*resolve)(const char *);
    memcpy(&resolve, &resolver, sizeof(resolve));
    void *resolved = resolve(name);
    if(!resolved) fprintf(stderr, "unresolved Windows API export: %s\n", name);
    assert(resolved != NULL);
    assert(resolved == dlsym(library, name));
    return resolved;
}

#define LOAD_MEMBER(api, member, type, export_name, resolver, library) do { \
    void *symbol = resolved_symbol((resolver), (library), (export_name)); \
    (api)->member = (type) symbol; \
} while(0)

static void check_capi(const struct capi_api *api, int wrapped_random) {
    struct crypt_provider *prov = (struct crypt_provider *) (uintptr_t) 1;
    const char16_t unknown_provider[] = { 'N', 0xe9, 0 };

    assert(!api->acquire_a(&prov, NULL, NULL, PROV_DSS, 0));
    assert(prov == NULL);
    assert(api->get_error() == NTE_BAD_PROV_TYPE);
    assert(!api->acquire_w(&prov, u"SYNA", unknown_provider, PROV_RSA_FULL, 0));
    assert(prov == NULL);
    assert(api->get_error() == NTE_BAD_PROVIDER);
    assert(!api->acquire_w(&prov, u"SYNA", NULL, PROV_RSA_FULL, CRYPT_NEWKEYSET));
    assert(prov == NULL);
    assert(api->get_error() == NTE_BAD_FLAGS);
    assert(!api->acquire_w(NULL, NULL, NULL, PROV_RSA_FULL, 0));
    assert(api->get_error() == ERROR_INVALID_PARAMETER);

    //Exact adapter path: SYNA + default PROV_RSA_FULL + SHA-1 of 32 bytes.
    assert(api->acquire_w(&prov, u"SYNA", NULL, PROV_RSA_FULL, 0));
    assert(prov != NULL);
    struct crypt_hash *hash = NULL;
    assert(api->create_hash(prov, CALG_SHA1, NULL, 0, &hash));
    assert(hash != NULL);
    BYTE input[32] = {0};
    assert(api->hash_data(hash, input, sizeof(input), 0));
    DWORD digest_len = 0;
    assert(api->get_hash(hash, HP_HASHVAL, NULL, &digest_len, 0));
    assert(digest_len == 20);
    BYTE digest[20];
    static const BYTE expected_digest[20] = {
        0xde, 0x8a, 0x84, 0x7b, 0xff, 0x8c, 0x34, 0x3d, 0x69, 0xb8,
        0x53, 0xa2, 0x15, 0xe6, 0xee, 0x77, 0x5e, 0xf2, 0xef, 0x96
    };
    assert(api->get_hash(hash, HP_HASHVAL, digest, &digest_len, 0));
    assert(digest_len == sizeof(digest));
    assert(memcmp(digest, expected_digest, sizeof(digest)) == 0);
    assert(api->destroy_hash(hash));
    assert(api->release(prov, 0));

    assert(!api->release(NULL, 0));
    assert(api->get_error() == NTE_INVALID_HANDLE);
    assert(api->acquire_w(&prov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT));
    assert(!api->release(prov, CRYPT_DELETEKEYSET));
    assert(api->get_error() == NTE_BAD_FLAGS);
    assert(api->release(prov, 0));

    //Sensor path and independent provider lifetimes.
    struct crypt_provider *aes_prov = NULL;
    assert(api->acquire_a(&aes_prov, "VFS key container", NULL, PROV_RSA_AES, 0));
    assert(aes_prov != NULL);
    assert(api->acquire_w(&prov, u"SYNA", u"Microsoft Enhanced RSA and AES Cryptographic Provider", PROV_RSA_AES, 0));
    assert(prov != NULL && prov != aes_prov);

    BYTE random_buf[32], canary[16];
    memset(canary, 0xa5, sizeof(canary));
    memset(random_buf, 0xa5, sizeof(random_buf));
    assert(api->random(aes_prov, sizeof(random_buf), random_buf));
    if(wrapped_random) {
        for(size_t i = 0; i < sizeof(random_buf); i++) assert(random_buf[i] == (BYTE) i);
    }
    for(size_t i = 0; i < sizeof(canary); i++) assert(canary[i] == 0xa5);

    int calls_before = random_calls;
    assert(api->random(aes_prov, 0, NULL));
    if(wrapped_random) assert(random_calls == calls_before);
    assert(!api->random(aes_prov, 1, NULL));
    assert(api->get_error() == ERROR_INVALID_PARAMETER);
    assert(!api->random(NULL, 1, random_buf));
    assert(api->get_error() == NTE_INVALID_HANDLE);

    if(wrapped_random) {
        fail_random = 1;
        assert(!api->random(aes_prov, sizeof(random_buf), random_buf));
        assert(api->get_error() == NTE_FAIL);
        fail_random = 0;
        assert(api->random(aes_prov, sizeof(random_buf), random_buf));
    }

    assert(api->release(prov, 0));
    assert(api->release(aes_prov, 0));
}

static const char *const imported_capi[] = {
    "CryptAcquireContextA", "CryptAcquireContextW", "CryptReleaseContext",
    "CryptDestroyKey", "CryptSetKeyParam", "CryptSetHashParam",
    "CryptGetHashParam", "CryptGenRandom", "CryptImportKey", "CryptEncrypt",
    "CryptDecrypt", "CryptCreateHash", "CryptHashData", "CryptDestroyHash",
    "CryptDuplicateHash"
};

static void check_real_library(const char *path) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    assert(library != NULL);
    void *resolver = dlsym(library, "resolve_windows_api");
    assert(resolver != NULL);
    for(size_t i = 0; i < sizeof(imported_capi) / sizeof(imported_capi[0]); i++) {
        resolved_symbol(resolver, library, imported_capi[i]);
    }

    struct capi_api api;
    LOAD_MEMBER(&api, acquire_a, acquire_a_fnc, "CryptAcquireContextA", resolver, library);
    LOAD_MEMBER(&api, acquire_w, acquire_w_fnc, "CryptAcquireContextW", resolver, library);
    LOAD_MEMBER(&api, release, release_fnc, "CryptReleaseContext", resolver, library);
    LOAD_MEMBER(&api, random, random_fnc, "CryptGenRandom", resolver, library);
    LOAD_MEMBER(&api, create_hash, create_hash_fnc, "CryptCreateHash", resolver, library);
    LOAD_MEMBER(&api, hash_data, hash_data_fnc, "CryptHashData", resolver, library);
    LOAD_MEMBER(&api, get_hash, get_hash_fnc, "CryptGetHashParam", resolver, library);
    LOAD_MEMBER(&api, destroy_hash, destroy_hash_fnc, "CryptDestroyHash", resolver, library);
    void *symbol = resolved_symbol(resolver, library, "GetLastError");
    api.get_error = (get_error_fnc) symbol;

    check_capi(&api, 0);
    assert(dlclose(library) == 0);
    puts("real libtudor: imported CAPI resolution, SHA-1, Unicode, lifecycle, and RNG passed");
}

int main(int argc, char **argv) {
    _Static_assert(sizeof(DWORD) == 4, "CAPI DWORD ABI");
    _Static_assert(sizeof(BOOL) == 4, "CAPI BOOL ABI");
    _Static_assert(sizeof(char16_t) == 2, "CAPI wide string ABI");
    _Static_assert(sizeof(struct crypt_provider *) == sizeof(uintptr_t), "CAPI handle ABI");

    struct capi_api api = {
        .acquire_a = CryptAcquireContextA,
        .acquire_w = CryptAcquireContextW,
        .release = CryptReleaseContext,
        .random = CryptGenRandom,
        .create_hash = CryptCreateHash,
        .hash_data = CryptHashData,
        .get_hash = CryptGetHashParam,
        .destroy_hash = CryptDestroyHash,
        .get_error = test_get_last_error
    };
    check_capi(&api, 1);
    puts("standalone CAPI: deterministic SHA-1/RNG and error paths passed");

    if(argc == 2) check_real_library(argv[1]);
    return 0;
}
