#include <openssl/ecdsa.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <string.h>
#include "crypt.h"

static const char crypt_prov_base_name[] = "Microsoft Base Cryptographic Provider v1.0";
static const char crypt_prov_enhanced_name[] = "Microsoft Enhanced Cryptographic Provider v1.0";
static const char crypt_prov_rsa_aes_name[] = "Microsoft Enhanced RSA and AES Cryptographic Provider";

static BOOL crypt_acquire_context(struct crypt_provider **out, DWORD prov_type, DWORD flags) {
    if(!out) { winerr_set_code(ERROR_INVALID_PARAMETER); return FALSE; }
    *out = NULL;

    struct crypt_provider *provider_type;
    switch(prov_type) {
        case PROV_RSA_FULL: provider_type = &crypt_prov_rsa_full; break;
        case PROV_RSA_AES: provider_type = &crypt_prov_rsa_aes; break;
        default: winerr_set_code(NTE_BAD_PROV_TYPE); return FALSE;
    }

    /* This provider implements hashing, RNG and caller-supplied symmetric keys.
     * It does not persist or manufacture private key containers. */
    if(flags & ~CRYPT_VERIFYCONTEXT) { winerr_set_code(NTE_BAD_FLAGS); return FALSE; }

    struct crypt_provider *prov = malloc(sizeof(*prov));
    if(!prov) { winerr_set_code(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    *prov = *provider_type;
    *out = prov;
    return TRUE;
}

static BOOL crypt_provider_name_valid_a(const char *name, DWORD prov_type) {
    if(!name) return TRUE;
    if(prov_type == PROV_RSA_FULL) {
        return strcmp(name, crypt_prov_base_name) == 0 || strcmp(name, crypt_prov_enhanced_name) == 0;
    }
    return prov_type == PROV_RSA_AES && strcmp(name, crypt_prov_rsa_aes_name) == 0;
}

static BOOL crypt_wstr_equals_ascii(const char16_t *wstr, const char *str) {
    while(*wstr && *str) {
        if(*wstr >= 0x80 || *wstr != (char16_t) (unsigned char) *str) return FALSE;
        wstr++;
        str++;
    }
    return *wstr == 0 && *str == '\0';
}

static BOOL crypt_provider_name_valid_w(const char16_t *name, DWORD prov_type) {
    if(!name) return TRUE;
    if(prov_type == PROV_RSA_FULL) {
        return crypt_wstr_equals_ascii(name, crypt_prov_base_name) || crypt_wstr_equals_ascii(name, crypt_prov_enhanced_name);
    }
    return prov_type == PROV_RSA_AES && crypt_wstr_equals_ascii(name, crypt_prov_rsa_aes_name);
}

__winfnc BOOL CryptAcquireContextA(struct crypt_provider **prov, const char *cont_name, const char *prov_name, DWORD prov_type, DWORD flags) {
    if(prov) *prov = NULL;
    if(!crypt_provider_name_valid_a(prov_name, prov_type)) { winerr_set_code(NTE_BAD_PROVIDER); return FALSE; }
    return crypt_acquire_context(prov, prov_type, flags);
}
WINAPI(CryptAcquireContextA)

__winfnc BOOL CryptAcquireContextW(struct crypt_provider **prov, const char16_t *cont_name, const char16_t *prov_name, DWORD prov_type, DWORD flags) {
    if(prov) *prov = NULL;
    if(!crypt_provider_name_valid_w(prov_name, prov_type)) { winerr_set_code(NTE_BAD_PROVIDER); return FALSE; }
    return crypt_acquire_context(prov, prov_type, flags);
}
WINAPI(CryptAcquireContextW)

__winfnc BOOL CryptReleaseContext(struct crypt_provider *prov, DWORD flags) {
    if(!prov) { winerr_set_code(NTE_INVALID_HANDLE); return FALSE; }
    if(flags != 0) { winerr_set_code(NTE_BAD_FLAGS); return FALSE; }
    free(prov);
    return TRUE;
}
WINAPI(CryptReleaseContext)

__winfnc BOOL CryptImportKey(struct crypt_provider *prov, const BLOBHEADER *data, DWORD data_len, struct crypt_key *pub_key, DWORD flags, struct crypt_key **out) {
    if(!prov->import_key) { winerr_set(); return FALSE; }
    if(pub_key) {
        log_warn("CryptImportKey called with encrypted key data!");
        winerr_set();
        return FALSE;
    }

    if(data_len < sizeof(BLOBHEADER)) { winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }

    //Allocate key
    struct crypt_key *key = (struct crypt_key*) malloc(sizeof(struct crypt_key));
    if(!key) { winerr_set_errno(); return FALSE; }
    key->prov = prov;
    key->key_data = NULL;
    memset(key->iv, 0, sizeof(key->iv));
    key->mode = CRYPT_MODE_CBC;
    key->padding = PKCS5_PADDING;

    if(data->bType == type_PLAINTEXTKEYBLOB) {
        //Copy plaintext data
        PLAINTEXTKEYBLOB *blob = (PLAINTEXTKEYBLOB*) data;
        if(data_len < sizeof(PLAINTEXTKEYBLOB) + blob->dwKeySize) { winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }

        key->plain_data = malloc(blob->dwKeySize);
        key->plain_size = blob->dwKeySize;
        if(!key->plain_data) {
            winerr_set_errno();
            free(key);
            return FALSE;
        }
        memcpy(key->plain_data, blob+1, blob->dwKeySize);
    } else {
        key->plain_data = NULL;
        key->plain_size = 0;
    }

    //Import key
    if(!prov->import_key(prov, data, data_len, &key->key_data)) {
        winerr_set_errno();
        free(key->plain_data);
        free(key);
        return FALSE;
    }

    *out = key;
    return TRUE;
}
WINAPI(CryptImportKey)

__winfnc BOOL CryptDestroyKey(struct crypt_key *key) {
    if(key->prov->destroy_key) key->prov->destroy_key(key->prov, key->key_data);
    free(key->plain_data);
    free(key);
    return TRUE;
}
WINAPI(CryptDestroyKey)

/* ---- CAPI symmetric (AES) crypto, backed by OpenSSL EVP ---------------------
 * Used by the HP "110" driver's secure-pairing handshake. The raw key bytes are
 * in key->plain_data (imported via CryptImportKey/PLAINTEXTKEYBLOB); IV and mode
 * come from CryptSetKeyParam. Default mode CBC, default padding PKCS#7. */

static const EVP_CIPHER *capi_aes_cipher(struct crypt_key *key) {
    int ecb = (key->mode == CRYPT_MODE_ECB);
    switch(key->plain_size) {
        case 16: return ecb ? EVP_aes_128_ecb() : EVP_aes_128_cbc();
        case 24: return ecb ? EVP_aes_192_ecb() : EVP_aes_192_cbc();
        case 32: return ecb ? EVP_aes_256_ecb() : EVP_aes_256_cbc();
        default: return NULL;
    }
}

__winfnc BOOL CryptSetKeyParam(struct crypt_key *key, DWORD param, const BYTE *data, DWORD flags) {
    switch(param) {
        case KP_IV:      if(data) memcpy(key->iv, data, sizeof(key->iv)); return TRUE;
        case KP_MODE:    if(data) key->mode = *(const DWORD*) data; return TRUE;
        case KP_PADDING: if(data) key->padding = *(const DWORD*) data; return TRUE;
        case KP_MODE_BITS: return TRUE;
        default: log_warn("CryptSetKeyParam: unsupported param %u", (unsigned) param); return TRUE;
    }
}
WINAPI(CryptSetKeyParam)

__winfnc BOOL CryptGetKeyParam(struct crypt_key *key, DWORD param, BYTE *data, DWORD *data_len, DWORD flags) {
    DWORD v;
    switch(param) {
        case KP_IV:
            if(!data) { *data_len = sizeof(key->iv); return TRUE; }
            if(*data_len < sizeof(key->iv)) { *data_len = sizeof(key->iv); winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
            memcpy(data, key->iv, sizeof(key->iv)); *data_len = sizeof(key->iv); return TRUE;
        case KP_BLOCKLEN: v = 128; goto ret_dword;
        case KP_KEYLEN:   v = (DWORD)(key->plain_size * 8); goto ret_dword;
        case KP_MODE:     v = key->mode; goto ret_dword;
        case KP_PADDING:  v = key->padding; goto ret_dword;
        default: log_warn("CryptGetKeyParam: unsupported param %u", (unsigned) param); winerr_set(); return FALSE;
    }
ret_dword:
    if(!data) { *data_len = sizeof(DWORD); return TRUE; }
    if(*data_len < sizeof(DWORD)) { *data_len = sizeof(DWORD); winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(data, &v, sizeof(DWORD)); *data_len = sizeof(DWORD); return TRUE;
}
WINAPI(CryptGetKeyParam)

__winfnc BOOL CryptEncrypt(struct crypt_key *key, struct crypt_hash *hash, BOOL final, DWORD flags, BYTE *data, DWORD *data_len, DWORD buf_len) {
    const int block = 16;
    if(hash) log_warn("CryptEncrypt: hHash unsupported, ignoring");
    const EVP_CIPHER *cipher = capi_aes_cipher(key);
    if(!cipher) { winerr_set(); return FALSE; }

    DWORD in_len = *data_len;
    DWORD out_needed = final ? ((in_len / block) + 1) * block : in_len;
    if(!data) { *data_len = out_needed; return TRUE; }
    if(final && buf_len < out_needed) { *data_len = out_needed; winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx) { winerr_set(); return FALSE; }
    if(EVP_EncryptInit_ex(ctx, cipher, NULL, (const unsigned char*) key->plain_data, key->mode == CRYPT_MODE_ECB ? NULL : key->iv) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    EVP_CIPHER_CTX_set_padding(ctx, final ? 1 : 0);

    int outl = 0, tmpl = 0;
    if(EVP_EncryptUpdate(ctx, data, &outl, data, (int) in_len) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    if(EVP_EncryptFinal_ex(ctx, data + outl, &tmpl) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    EVP_CIPHER_CTX_free(ctx);

    DWORD total = (DWORD)(outl + tmpl);
    if(!final && key->mode != CRYPT_MODE_ECB && total >= 16) memcpy(key->iv, data + total - 16, 16); /* CBC chaining */
    *data_len = total;
    return TRUE;
}
WINAPI(CryptEncrypt)

__winfnc BOOL CryptDecrypt(struct crypt_key *key, struct crypt_hash *hash, BOOL final, DWORD flags, BYTE *data, DWORD *data_len) {
    if(hash) log_warn("CryptDecrypt: hHash unsupported, ignoring");
    const EVP_CIPHER *cipher = capi_aes_cipher(key);
    if(!cipher) { winerr_set(); return FALSE; }

    DWORD in_len = *data_len;
    unsigned char next_iv[16];
    int chain = (!final && key->mode != CRYPT_MODE_ECB && in_len >= 16);
    if(chain) memcpy(next_iv, data + in_len - 16, 16);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if(!ctx) { winerr_set(); return FALSE; }
    if(EVP_DecryptInit_ex(ctx, cipher, NULL, (const unsigned char*) key->plain_data, key->mode == CRYPT_MODE_ECB ? NULL : key->iv) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    EVP_CIPHER_CTX_set_padding(ctx, final ? 1 : 0);

    int outl = 0, tmpl = 0;
    if(EVP_DecryptUpdate(ctx, data, &outl, data, (int) in_len) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    if(EVP_DecryptFinal_ex(ctx, data + outl, &tmpl) != 1) { EVP_CIPHER_CTX_free(ctx); winerr_set(); return FALSE; }
    EVP_CIPHER_CTX_free(ctx);

    if(chain) memcpy(key->iv, next_iv, 16); /* CBC chaining */
    *data_len = (DWORD)(outl + tmpl);
    return TRUE;
}
WINAPI(CryptDecrypt)

__winfnc BOOL CryptCreateHash(struct crypt_provider *prov, ALG_ID alg_id, struct crypt_key *key, DWORD flags, struct crypt_hash **out) {
    //Get the algorithm
    struct crypt_hash_algorithm *algo;
    switch(alg_id) {
        case CALG_SHA1: algo = (struct crypt_hash_algorithm*) &crypt_hash_algo_sha1; break;
        case CALG_SHA_256: algo = (struct crypt_hash_algorithm*) &crypt_hash_algo_sha256; break;
        case CALG_SHA_384: algo = (struct crypt_hash_algorithm*) &crypt_hash_algo_sha384; break;
        case CALG_SHA_512: algo = (struct crypt_hash_algorithm*) &crypt_hash_algo_sha512; break;
        case CALG_HMAC: algo = &crypt_hash_algo_hmac; break;
        default: {
            log_warn("CryptCreateHash | Couldn't find algorithm id 0x%x flags 0x%x", alg_id, flags);
            return FALSE;
        }
    }

    //Allocate and create hash
    struct crypt_hash *hash = (struct crypt_hash*) malloc(sizeof(struct crypt_hash));
    if(!hash) { winerr_set_errno(); return FALSE; }
    hash->algo = algo;
    hash->hash_data = NULL;

    if(!algo->create_hash(algo, key, &hash->hash_data)) {
        free(hash);
        return FALSE;
    }

    *out = hash;
    return TRUE;
}
WINAPI(CryptCreateHash)

__winfnc BOOL CryptDestroyHash(struct crypt_hash *hash) {
    if(hash->algo->destroy_hash) hash->algo->destroy_hash(hash->algo, hash->hash_data);
    free(hash);
    return TRUE;
}
WINAPI(CryptDestroyHash)

__winfnc BOOL CryptDuplicateHash(struct crypt_hash *hash, DWORD *reserved, DWORD flags, struct crypt_hash **out) {
    //Allocate and duplicate hash
    struct crypt_hash *nhash = (struct crypt_hash*) malloc(sizeof(struct crypt_hash));
    if(!nhash) { winerr_set_errno(); return FALSE; }
    nhash->algo = hash->algo;
    nhash->hash_data = NULL;

    if(!hash->algo->duplicate_hash(hash->algo, hash->hash_data, &nhash->hash_data)) {
        free(nhash);
        return FALSE;
    }

    *out = nhash;
    return TRUE;
}
WINAPI(CryptDuplicateHash)

__winfnc BOOL CryptGetHashParam(struct crypt_hash *hash, DWORD param, BYTE *data, DWORD *data_len, DWORD flags) {
    size_t buf_size = *data_len;
    BOOL suc = hash->algo->get_hash_param(hash->algo, hash->hash_data, param, data, &buf_size);
    *data_len = (size_t) buf_size;
    return suc;
}
WINAPI(CryptGetHashParam)

__winfnc BOOL CryptSetHashParam(struct crypt_hash *hash, DWORD param, const BYTE *data, DWORD flags) {
    return hash->algo->set_hash_param(hash->algo, hash->hash_data, param, data);
}
WINAPI(CryptSetHashParam)

__winfnc BOOL CryptHashData(struct crypt_hash *hash, const BYTE *data, DWORD data_len, DWORD flags) {
    return hash->algo->update_hash(hash->algo, hash->hash_data, data, data_len);
}
WINAPI(CryptHashData)

__winfnc BOOL CryptEncodeObject(DWORD cert_enc_type, const char *struct_type, void *struct_info, BYTE *enc, DWORD *enc_size) {
    //Handle integer constants
    if(!(((uintptr_t) struct_type) & (128llu << sizeof(uintptr_t)))) {
        switch((uintptr_t) struct_type) {
            case X509_ECC_SIGNATURE: {
                CERT_ECC_SIGNATURE *ecc_sig = (CERT_ECC_SIGNATURE*) struct_info;

                //Create signature
                ECDSA_SIG *sig;
                LIBCRYPTO_ERR(sig = ECDSA_SIG_new());

                BIGNUM *sig_r, *sig_s;
                LIBCRYPTO_ERR(sig_r = BN_new());
                LIBCRYPTO_ERR(sig_s = BN_new());
                LIBCRYPTO_ERR(BN_lebin2bn(ecc_sig->r.pbData, ecc_sig->r.cbData, sig_r));
                LIBCRYPTO_ERR(BN_lebin2bn(ecc_sig->s.pbData, ecc_sig->s.cbData, sig_s));
                LIBCRYPTO_ERR(ECDSA_SIG_set0(sig, sig_r, sig_s));

                //Encode signature
                int sig_size = i2d_ECDSA_SIG(sig, NULL);
                if(enc && *enc_size >= sig_size) {
                    i2d_ECDSA_SIG(sig, &enc);
                } else if(enc) {
                    ECDSA_SIG_free(sig);
                    winerr_set_code(ERROR_INSUFFICIENT_BUFFER);
                    return FALSE;
                }

                ECDSA_SIG_free(sig);
                *enc_size = sig_size;
                return TRUE;
            }
            default: {
                log_debug("CryptEncodeObject | Unsupported arguments! cert_enc_type %x struct_type %lx", cert_enc_type, (uintptr_t) struct_type);
                winerr_set();
                return FALSE;
            }
        }
    }

    log_debug("CryptEncodeObject | Unsupported arguments! cert_enc_type %x struct_type '%s'", cert_enc_type, struct_type);
    winerr_set();
    return FALSE;
}
WINAPI(CryptEncodeObject)

__winfnc BOOL CryptDecodeObject(DWORD cert_enc_type, const char *struct_type, const BYTE *enc, DWORD enc_size, DWORD flags, void *struct_info, DWORD *struct_info_size) {
    //Handle integer constants
    if(!(((uintptr_t) struct_type) & (128llu << sizeof(uintptr_t)))) {
        switch((uintptr_t) struct_type) {
            case X509_ECC_SIGNATURE: {
                //Decode signature
                ECDSA_SIG *sig;
                LIBCRYPTO_ERR(sig = d2i_ECDSA_SIG(NULL, &enc, (long) enc_size));

                //Store signature
                const BIGNUM *sig_r = ECDSA_SIG_get0_r(sig), *sig_s = ECDSA_SIG_get0_s(sig);
                int sig_size = sizeof(CERT_ECC_SIGNATURE) + BN_num_bytes(sig_r) + BN_num_bytes(sig_s);
                if(struct_info && *struct_info_size >= sig_size) {
                    CERT_ECC_SIGNATURE *ecc_sig = (CERT_ECC_SIGNATURE*) struct_info;
                    ecc_sig->r.cbData = BN_num_bytes(sig_r);
                    ecc_sig->r.pbData = (BYTE*) struct_info + sizeof(CERT_ECC_SIGNATURE);
                    ecc_sig->s.cbData = BN_num_bytes(sig_s);
                    ecc_sig->s.pbData = (BYTE*) struct_info + sizeof(CERT_ECC_SIGNATURE) + ecc_sig->r.cbData;
                    LIBCRYPTO_ERR(BN_bn2lebinpad(sig_r, ecc_sig->r.pbData, ecc_sig->r.cbData));
                    LIBCRYPTO_ERR(BN_bn2lebinpad(sig_s, ecc_sig->s.pbData, ecc_sig->s.cbData));
                } else if(struct_info) { winerr_set_code(ERROR_INSUFFICIENT_BUFFER); return FALSE; }

                *struct_info_size = sig_size;
                ECDSA_SIG_free(sig);
                return TRUE;
            }
            default: {
                log_debug("CryptDecodeObject | Unsupported arguments! cert_enc_type %x struct_type %lx", cert_enc_type, (uintptr_t) struct_type);
                winerr_set();
                return FALSE;
            }
        }
    }

    log_debug("CryptDecodeObject | Unsupported arguments! cert_enc_type %x struct_type '%s'", cert_enc_type, struct_type);
    winerr_set();
    return FALSE;
}
WINAPI(CryptDecodeObject)

__winfnc BOOL CryptProtectData(DATA_BLOB *in, const char16_t *descr, DATA_BLOB *entropy, void *reserved, void *prompt_struct, DWORD flags, DATA_BLOB *out) {
    out->cbData = in->cbData;
    out->pbData = (BYTE*) malloc(in->cbData);
    if(!out->pbData) { winerr_set_errno(); return FALSE; }
    memcpy(out->pbData, in->pbData, in->cbData);
    return TRUE;
}
WINAPI(CryptProtectData)

__winfnc BOOL CryptUnprotectData(DATA_BLOB *in, const char16_t *descr, DATA_BLOB *entropy, void *reserved, void *prompt_struct, DWORD flags, DATA_BLOB *out) {
    out->cbData = in->cbData;
    out->pbData = (BYTE*) malloc(in->cbData);
    if(!out->pbData) { winerr_set_errno(); return FALSE; }
    memcpy(out->pbData, in->pbData, in->cbData);
    return TRUE;
}
WINAPI(CryptUnprotectData)

__winfnc BOOL CryptProtectMemory(void *data, DWORD size, DWORD flags) {
    return TRUE;
}
WINAPI(CryptProtectMemory);

__winfnc BOOL CryptUnprotectMemory(void *data, DWORD size, DWORD flags) {
    return TRUE;
}
WINAPI(CryptUnprotectMemory);

__winfnc BOOL CryptGenRandom(struct crypt_provider *prov, DWORD len, BYTE *buf) {
    if(!prov) { winerr_set_code(NTE_INVALID_HANDLE); return FALSE; }
    if(len != 0 && !buf) { winerr_set_code(ERROR_INVALID_PARAMETER); return FALSE; }
    if(len == 0) return TRUE;
    if(RAND_bytes_ex(NULL, buf, (size_t) len, 0) != 1) {
        ERR_clear_error();
        winerr_set_code(NTE_FAIL);
        return FALSE;
    }
    return TRUE;
}
WINAPI(CryptGenRandom)
