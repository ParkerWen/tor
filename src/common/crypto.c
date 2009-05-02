/* Copyright (c) 2001, Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2009, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file crypto.c
 * \brief Wrapper functions to present a consistent interface to
 * public-key and symmetric cryptography operations from OpenSSL.
 **/

#include "orconfig.h"

#ifdef MS_WINDOWS
#define WIN32_WINNT 0x400
#define _WIN32_WINNT 0x400
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
/* Windows defines this; so does openssl 0.9.8h and later. We don't actually
 * use either definition. */
#undef OCSP_RESPONSE
#endif

#include <openssl/err.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/opensslv.h>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/conf.h>
#include <openssl/hmac.h>

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_FCNTL_H
#include <sys/fcntl.h>
#endif

#define CRYPTO_PRIVATE
#include "crypto.h"
#include "log.h"
#include "aes.h"
#include "util.h"
#include "container.h"
#include "compat.h"

#if OPENSSL_VERSION_NUMBER < 0x00907000l
#error "We require openssl >= 0.9.7"
#endif

#include <openssl/engine.h>

/** Macro: is k a valid RSA public or private key? */
#define PUBLIC_KEY_OK(k) ((k) && (k)->key && (k)->key->n)
/** Macro: is k a valid RSA private key? */
#define PRIVATE_KEY_OK(k) ((k) && (k)->key && (k)->key->p)

#ifdef TOR_IS_MULTITHREADED
/** A number of prealloced mutexes for use by openssl. */
static tor_mutex_t **_openssl_mutexes = NULL;
/** How many mutexes have we allocated for use by openssl? */
static int _n_openssl_mutexes = 0;
#endif

/** A public key, or a public/private keypair. */
struct crypto_pk_env_t
{
  int refs; /* reference counting so we don't have to copy keys */
  RSA *key;
};

/** Key and stream information for a stream cipher. */
struct crypto_cipher_env_t
{
  char key[CIPHER_KEY_LEN];
  aes_cnt_cipher_t *cipher;
};

/** A structure to hold the first half (x, g^x) of a Diffie-Hellman handshake
 * while we're waiting for the second.*/
struct crypto_dh_env_t {
  DH *dh;
};

static int setup_openssl_threading(void);
static int tor_check_dh_key(BIGNUM *bn);

/** Return the number of bytes added by padding method <b>padding</b>.
 */
static INLINE int
crypto_get_rsa_padding_overhead(int padding)
{
  switch (padding)
    {
    case RSA_NO_PADDING: return 0;
    case RSA_PKCS1_OAEP_PADDING: return 42;
    case RSA_PKCS1_PADDING: return 11;
    default: tor_assert(0); return -1;
    }
}

/** Given a padding method <b>padding</b>, return the correct OpenSSL constant.
 */
static INLINE int
crypto_get_rsa_padding(int padding)
{
  switch (padding)
    {
    case PK_NO_PADDING: return RSA_NO_PADDING;
    case PK_PKCS1_PADDING: return RSA_PKCS1_PADDING;
    case PK_PKCS1_OAEP_PADDING: return RSA_PKCS1_OAEP_PADDING;
    default: tor_assert(0); return -1;
    }
}

/** Boolean: has OpenSSL's crypto been initialized? */
static int _crypto_global_initialized = 0;

/** Log all pending crypto errors at level <b>severity</b>.  Use
 * <b>doing</b> to describe our current activities.
 */
static void
crypto_log_errors(int severity, const char *doing)
{
  unsigned long err;
  const char *msg, *lib, *func;
  while ((err = ERR_get_error()) != 0) {
    msg = (const char*)ERR_reason_error_string(err);
    lib = (const char*)ERR_lib_error_string(err);
    func = (const char*)ERR_func_error_string(err);
    if (!msg) msg = "(null)";
    if (!lib) lib = "(null)";
    if (!func) func = "(null)";
    if (doing) {
      log(severity, LD_CRYPTO, "crypto error while %s: %s (in %s:%s)",
          doing, msg, lib, func);
    } else {
      log(severity, LD_CRYPTO, "crypto error: %s (in %s:%s)", msg, lib, func);
    }
  }
}

/** Log any OpenSSL engines we're using at NOTICE. */
static void
log_engine(const char *fn, ENGINE *e)
{
  if (e) {
    const char *name, *id;
    name = ENGINE_get_name(e);
    id = ENGINE_get_id(e);
    log(LOG_NOTICE, LD_CRYPTO, "Using OpenSSL engine %s [%s] for %s",
        name?name:"?", id?id:"?", fn);
  } else {
    log(LOG_INFO, LD_CRYPTO, "Using default implementation for %s", fn);
  }
}

/** Initialize the crypto library.  Return 0 on success, -1 on failure.
 */
int
crypto_global_init(int useAccel)
{
  if (!_crypto_global_initialized) {
    ERR_load_crypto_strings();
    OpenSSL_add_all_algorithms();
    _crypto_global_initialized = 1;
    setup_openssl_threading();
    /* XXX the below is a bug, since we can't know if we're supposed
     * to be using hardware acceleration or not. we should arrange
     * for this function to be called before init_keys. But make it
     * not complain loudly, at least until we make acceleration work. */
    if (useAccel < 0) {
      log_info(LD_CRYPTO, "Initializing OpenSSL via tor_tls_init().");
    }
    if (useAccel > 0) {
      log_info(LD_CRYPTO, "Initializing OpenSSL engine support.");
      ENGINE_load_builtin_engines();
      if (!ENGINE_register_all_complete())
        return -1;

      /* XXXX make sure this isn't leaking. */
      log_engine("RSA", ENGINE_get_default_RSA());
      log_engine("DH", ENGINE_get_default_DH());
      log_engine("RAND", ENGINE_get_default_RAND());
      log_engine("SHA1", ENGINE_get_digest_engine(NID_sha1));
      log_engine("3DES", ENGINE_get_cipher_engine(NID_des_ede3_ecb));
      log_engine("AES", ENGINE_get_cipher_engine(NID_aes_128_ecb));
    }
    return crypto_seed_rng(1);
  }
  return 0;
}

/** Free crypto resources held by this thread. */
void
crypto_thread_cleanup(void)
{
  ERR_remove_state(0);
}

/** Uninitialize the crypto library. Return 0 on success, -1 on failure.
 */
int
crypto_global_cleanup(void)
{
  EVP_cleanup();
  ERR_remove_state(0);
  ERR_free_strings();
  ENGINE_cleanup();
  CONF_modules_unload(1);
  CRYPTO_cleanup_all_ex_data();
#ifdef TOR_IS_MULTITHREADED
  if (_n_openssl_mutexes) {
    int n = _n_openssl_mutexes;
    tor_mutex_t **ms = _openssl_mutexes;
    int i;
    _openssl_mutexes = NULL;
    _n_openssl_mutexes = 0;
    for (i=0;i<n;++i) {
      tor_mutex_free(ms[i]);
    }
    tor_free(ms);
  }
#endif
  return 0;
}

/** used by tortls.c: wrap an RSA* in a crypto_pk_env_t. */
crypto_pk_env_t *
_crypto_new_pk_env_rsa(RSA *rsa)
{
  crypto_pk_env_t *env;
  tor_assert(rsa);
  env = tor_malloc(sizeof(crypto_pk_env_t));
  env->refs = 1;
  env->key = rsa;
  return env;
}

/** used by tortls.c: wrap the RSA from an evp_pkey in a crypto_pk_env_t.
 * returns NULL if this isn't an RSA key. */
crypto_pk_env_t *
_crypto_new_pk_env_evp_pkey(EVP_PKEY *pkey)
{
  RSA *rsa;
  if (!(rsa = EVP_PKEY_get1_RSA(pkey)))
    return NULL;
  return _crypto_new_pk_env_rsa(rsa);
}

/** Helper, used by tor-checkkey.c.  Return the RSA from a crypto_pk_env_t. */
RSA *
_crypto_pk_env_get_rsa(crypto_pk_env_t *env)
{
  return env->key;
}

/** used by tortls.c: get an equivalent EVP_PKEY* for a crypto_pk_env_t.  Iff
 * private is set, include the private-key portion of the key. */
EVP_PKEY *
_crypto_pk_env_get_evp_pkey(crypto_pk_env_t *env, int private)
{
  RSA *key = NULL;
  EVP_PKEY *pkey = NULL;
  tor_assert(env->key);
  if (private) {
    if (!(key = RSAPrivateKey_dup(env->key)))
      goto error;
  } else {
    if (!(key = RSAPublicKey_dup(env->key)))
      goto error;
  }
  if (!(pkey = EVP_PKEY_new()))
    goto error;
  if (!(EVP_PKEY_assign_RSA(pkey, key)))
    goto error;
  return pkey;
 error:
  if (pkey)
    EVP_PKEY_free(pkey);
  if (key)
    RSA_free(key);
  return NULL;
}

/** Used by tortls.c: Get the DH* from a crypto_dh_env_t.
 */
DH *
_crypto_dh_env_get_dh(crypto_dh_env_t *dh)
{
  return dh->dh;
}

/** Allocate and return storage for a public key.  The key itself will not yet
 * be set.
 */
crypto_pk_env_t *
crypto_new_pk_env(void)
{
  RSA *rsa;

  rsa = RSA_new();
  if (!rsa) return NULL;
  return _crypto_new_pk_env_rsa(rsa);
}

/** Release a reference to an asymmetric key; when all the references
 * are released, free the key.
 */
void
crypto_free_pk_env(crypto_pk_env_t *env)
{
  tor_assert(env);

  if (--env->refs > 0)
    return;

  if (env->key)
    RSA_free(env->key);

  tor_free(env);
}

/** Create a new symmetric cipher for a given key and encryption flag
 * (1=encrypt, 0=decrypt).  Return the crypto object on success; NULL
 * on failure.
 */
crypto_cipher_env_t *
crypto_create_init_cipher(const char *key, int encrypt_mode)
{
  int r;
  crypto_cipher_env_t *crypto = NULL;

  if (! (crypto = crypto_new_cipher_env())) {
    log_warn(LD_CRYPTO, "Unable to allocate crypto object");
    return NULL;
  }

  if (crypto_cipher_set_key(crypto, key)) {
    crypto_log_errors(LOG_WARN, "setting symmetric key");
    goto error;
  }

  if (encrypt_mode)
    r = crypto_cipher_encrypt_init_cipher(crypto);
  else
    r = crypto_cipher_decrypt_init_cipher(crypto);

  if (r)
    goto error;
  return crypto;

 error:
  if (crypto)
    crypto_free_cipher_env(crypto);
  return NULL;
}

/** Allocate and return a new symmetric cipher.
 */
crypto_cipher_env_t *
crypto_new_cipher_env(void)
{
  crypto_cipher_env_t *env;

  env = tor_malloc_zero(sizeof(crypto_cipher_env_t));
  env->cipher = aes_new_cipher();
  return env;
}

/** Free a symmetric cipher.
 */
void
crypto_free_cipher_env(crypto_cipher_env_t *env)
{
  tor_assert(env);

  tor_assert(env->cipher);
  aes_free_cipher(env->cipher);
  memset(env, 0, sizeof(crypto_cipher_env_t));
  tor_free(env);
}

/* public key crypto */

/** Generate a new public/private keypair in <b>env</b>.  Return 0 on
 * success, -1 on failure.
 */
int
crypto_pk_generate_key(crypto_pk_env_t *env)
{
  tor_assert(env);

  if (env->key)
    RSA_free(env->key);
#if OPENSSL_VERSION_NUMBER < 0x00908000l
  /* In openssl 0.9.7, RSA_generate_key is all we have. */
  env->key = RSA_generate_key(PK_BYTES*8,65537, NULL, NULL);
#else
  /* In openssl 0.9.8, RSA_generate_key is deprecated. */
  {
    BIGNUM *e = BN_new();
    RSA *r = NULL;
    if (!e)
      goto done;
    if (! BN_set_word(e, 65537))
      goto done;
    r = RSA_new();
    if (!r)
      goto done;
    if (RSA_generate_key_ex(r, PK_BYTES*8, e, NULL) == -1)
      goto done;

    env->key = r;
    r = NULL;
  done:
    if (e)
      BN_free(e);
    if (r)
      RSA_free(r);
    }
#endif
  if (!env->key) {
    crypto_log_errors(LOG_WARN, "generating RSA key");
    return -1;
  }

  return 0;
}

/** Read a PEM-encoded private key from the string <b>s</b> into <b>env</b>.
 * Return 0 on success, -1 on failure.
 */
/* Used here, and used for testing. */
int
crypto_pk_read_private_key_from_string(crypto_pk_env_t *env,
                                       const char *s)
{
  BIO *b;

  tor_assert(env);
  tor_assert(s);

  /* Create a read-only memory BIO, backed by the nul-terminated string 's' */
  b = BIO_new_mem_buf((char*)s, -1);

  if (env->key)
    RSA_free(env->key);

  env->key = PEM_read_bio_RSAPrivateKey(b,NULL,NULL,NULL);

  BIO_free(b);

  if (!env->key) {
    crypto_log_errors(LOG_WARN, "Error parsing private key");
    return -1;
  }
  return 0;
}

/** Read a PEM-encoded private key from the file named by
 * <b>keyfile</b> into <b>env</b>.  Return 0 on success, -1 on failure.
 */
int
crypto_pk_read_private_key_from_filename(crypto_pk_env_t *env,
                                         const char *keyfile)
{
  char *contents;
  int r;

  /* Read the file into a string. */
  contents = read_file_to_str(keyfile, 0, NULL);
  if (!contents) {
    log_warn(LD_CRYPTO, "Error reading private key from \"%s\"", keyfile);
    return -1;
  }

  /* Try to parse it. */
  r = crypto_pk_read_private_key_from_string(env, contents);
  tor_free(contents);
  if (r)
    return -1; /* read_private_key_from_string already warned, so we don't.*/

  /* Make sure it's valid. */
  if (crypto_pk_check_key(env) <= 0)
    return -1;

  return 0;
}

/** Helper function to implement crypto_pk_write_*_key_to_string. */
static int
crypto_pk_write_key_to_string_impl(crypto_pk_env_t *env, char **dest,
                                   size_t *len, int is_public)
{
  BUF_MEM *buf;
  BIO *b;
  int r;

  tor_assert(env);
  tor_assert(env->key);
  tor_assert(dest);

  b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

  /* Now you can treat b as if it were a file.  Just use the
   * PEM_*_bio_* functions instead of the non-bio variants.
   */
  if (is_public)
    r = PEM_write_bio_RSAPublicKey(b, env->key);
  else
    r = PEM_write_bio_RSAPrivateKey(b, env->key, NULL,NULL,0,NULL,NULL);

  if (!r) {
    crypto_log_errors(LOG_WARN, "writing RSA key to string");
    BIO_free(b);
    return -1;
  }

  BIO_get_mem_ptr(b, &buf);
  (void)BIO_set_close(b, BIO_NOCLOSE); /* so BIO_free doesn't free buf */
  BIO_free(b);

  tor_assert(buf->length >= 0);
  *dest = tor_malloc(buf->length+1);
  memcpy(*dest, buf->data, buf->length);
  (*dest)[buf->length] = 0; /* nul terminate it */
  *len = buf->length;
  BUF_MEM_free(buf);

  return 0;
}

/** PEM-encode the public key portion of <b>env</b> and write it to a
 * newly allocated string.  On success, set *<b>dest</b> to the new
 * string, *<b>len</b> to the string's length, and return 0.  On
 * failure, return -1.
 */
int
crypto_pk_write_public_key_to_string(crypto_pk_env_t *env, char **dest,
                                     size_t *len)
{
  return crypto_pk_write_key_to_string_impl(env, dest, len, 1);
}

/** PEM-encode the private key portion of <b>env</b> and write it to a
 * newly allocated string.  On success, set *<b>dest</b> to the new
 * string, *<b>len</b> to the string's length, and return 0.  On
 * failure, return -1.
 */
int
crypto_pk_write_private_key_to_string(crypto_pk_env_t *env, char **dest,
                                     size_t *len)
{
  return crypto_pk_write_key_to_string_impl(env, dest, len, 0);
}

/** Read a PEM-encoded public key from the first <b>len</b> characters of
 * <b>src</b>, and store the result in <b>env</b>.  Return 0 on success, -1 on
 * failure.
 */
int
crypto_pk_read_public_key_from_string(crypto_pk_env_t *env, const char *src,
                                      size_t len)
{
  BIO *b;

  tor_assert(env);
  tor_assert(src);
  tor_assert(len<INT_MAX);

  b = BIO_new(BIO_s_mem()); /* Create a memory BIO */

  BIO_write(b, src, (int)len);

  if (env->key)
    RSA_free(env->key);
  env->key = PEM_read_bio_RSAPublicKey(b, NULL, NULL, NULL);
  BIO_free(b);
  if (!env->key) {
    crypto_log_errors(LOG_WARN, "reading public key from string");
    return -1;
  }

  return 0;
}

/** Write the private key from <b>env</b> into the file named by <b>fname</b>,
 * PEM-encoded.  Return 0 on success, -1 on failure.
 */
int
crypto_pk_write_private_key_to_filename(crypto_pk_env_t *env,
                                        const char *fname)
{
  BIO *bio;
  char *cp;
  long len;
  char *s;
  int r;

  tor_assert(PRIVATE_KEY_OK(env));

  if (!(bio = BIO_new(BIO_s_mem())))
    return -1;
  if (PEM_write_bio_RSAPrivateKey(bio, env->key, NULL,NULL,0,NULL,NULL)
      == 0) {
    crypto_log_errors(LOG_WARN, "writing private key");
    BIO_free(bio);
    return -1;
  }
  len = BIO_get_mem_data(bio, &cp);
  tor_assert(len >= 0);
  s = tor_malloc(len+1);
  memcpy(s, cp, len);
  s[len]='\0';
  r = write_str_to_file(fname, s, 0);
  BIO_free(bio);
  tor_free(s);
  return r;
}

/** Return true iff <b>env</b> has a valid key.
 */
int
crypto_pk_check_key(crypto_pk_env_t *env)
{
  int r;
  tor_assert(env);

  r = RSA_check_key(env->key);
  if (r <= 0)
    crypto_log_errors(LOG_WARN,"checking RSA key");
  return r;
}

/** Return true iff <b>key</b> contains the private-key portion of the RSA
 * key. */
int
crypto_pk_key_is_private(const crypto_pk_env_t *key)
{
  tor_assert(key);
  return PRIVATE_KEY_OK(key);
}

/** Compare the public-key components of a and b.  Return -1 if a\<b, 0
 * if a==b, and 1 if a\>b.
 */
int
crypto_pk_cmp_keys(crypto_pk_env_t *a, crypto_pk_env_t *b)
{
  int result;

  if (!a || !b)
    return -1;

  if (!a->key || !b->key)
    return -1;

  tor_assert(PUBLIC_KEY_OK(a));
  tor_assert(PUBLIC_KEY_OK(b));
  result = BN_cmp((a->key)->n, (b->key)->n);
  if (result)
    return result;
  return BN_cmp((a->key)->e, (b->key)->e);
}

/** Return the size of the public key modulus in <b>env</b>, in bytes. */
size_t
crypto_pk_keysize(crypto_pk_env_t *env)
{
  tor_assert(env);
  tor_assert(env->key);

  return (size_t) RSA_size(env->key);
}

/** Increase the reference count of <b>env</b>, and return it.
 */
crypto_pk_env_t *
crypto_pk_dup_key(crypto_pk_env_t *env)
{
  tor_assert(env);
  tor_assert(env->key);

  env->refs++;
  return env;
}

/** Make a real honest-to-goodness copy of <b>env</b>, and return it. */
crypto_pk_env_t *
crypto_pk_copy_full(crypto_pk_env_t *env)
{
  RSA *new_key;
  tor_assert(env);
  tor_assert(env->key);

  if (PRIVATE_KEY_OK(env)) {
    new_key = RSAPrivateKey_dup(env->key);
  } else {
    new_key = RSAPublicKey_dup(env->key);
  }

  return _crypto_new_pk_env_rsa(new_key);
}

/** Encrypt <b>fromlen</b> bytes from <b>from</b> with the public key
 * in <b>env</b>, using the padding method <b>padding</b>.  On success,
 * write the result to <b>to</b>, and return the number of bytes
 * written.  On failure, return -1.
 */
int
crypto_pk_public_encrypt(crypto_pk_env_t *env, char *to,
                         const char *from, size_t fromlen, int padding)
{
  int r;
  tor_assert(env);
  tor_assert(from);
  tor_assert(to);
  tor_assert(fromlen<INT_MAX);

  r = RSA_public_encrypt((int)fromlen,
                         (unsigned char*)from, (unsigned char*)to,
                         env->key, crypto_get_rsa_padding(padding));
  if (r<0) {
    crypto_log_errors(LOG_WARN, "performing RSA encryption");
    return -1;
  }
  return r;
}

/** Decrypt <b>fromlen</b> bytes from <b>from</b> with the private key
 * in <b>env</b>, using the padding method <b>padding</b>.  On success,
 * write the result to <b>to</b>, and return the number of bytes
 * written.  On failure, return -1.
 */
int
crypto_pk_private_decrypt(crypto_pk_env_t *env, char *to,
                          const char *from, size_t fromlen,
                          int padding, int warnOnFailure)
{
  int r;
  tor_assert(env);
  tor_assert(from);
  tor_assert(to);
  tor_assert(env->key);
  tor_assert(fromlen<INT_MAX);
  if (!env->key->p)
    /* Not a private key */
    return -1;

  r = RSA_private_decrypt((int)fromlen,
                          (unsigned char*)from, (unsigned char*)to,
                          env->key, crypto_get_rsa_padding(padding));

  if (r<0) {
    crypto_log_errors(warnOnFailure?LOG_WARN:LOG_DEBUG,
                      "performing RSA decryption");
    return -1;
  }
  return r;
}

/** Check the signature in <b>from</b> (<b>fromlen</b> bytes long) with the
 * public key in <b>env</b>, using PKCS1 padding.  On success, write the
 * signed data to <b>to</b>, and return the number of bytes written.
 * On failure, return -1.
 */
int
crypto_pk_public_checksig(crypto_pk_env_t *env, char *to,
                          const char *from, size_t fromlen)
{
  int r;
  tor_assert(env);
  tor_assert(from);
  tor_assert(to);
  tor_assert(fromlen < INT_MAX);
  r = RSA_public_decrypt((int)fromlen,
                         (unsigned char*)from, (unsigned char*)to,
                         env->key, RSA_PKCS1_PADDING);

  if (r<0) {
    crypto_log_errors(LOG_WARN, "checking RSA signature");
    return -1;
  }
  return r;
}

/** Check a siglen-byte long signature at <b>sig</b> against
 * <b>datalen</b> bytes of data at <b>data</b>, using the public key
 * in <b>env</b>. Return 0 if <b>sig</b> is a correct signature for
 * SHA1(data).  Else return -1.
 */
int
crypto_pk_public_checksig_digest(crypto_pk_env_t *env, const char *data,
                               size_t datalen, const char *sig, size_t siglen)
{
  char digest[DIGEST_LEN];
  char *buf;
  int r;

  tor_assert(env);
  tor_assert(data);
  tor_assert(sig);

  if (crypto_digest(digest,data,datalen)<0) {
    log_warn(LD_BUG, "couldn't compute digest");
    return -1;
  }
  buf = tor_malloc(crypto_pk_keysize(env)+1);
  r = crypto_pk_public_checksig(env,buf,sig,siglen);
  if (r != DIGEST_LEN) {
    log_warn(LD_CRYPTO, "Invalid signature");
    tor_free(buf);
    return -1;
  }
  if (memcmp(buf, digest, DIGEST_LEN)) {
    log_warn(LD_CRYPTO, "Signature mismatched with digest.");
    tor_free(buf);
    return -1;
  }
  tor_free(buf);

  return 0;
}

/** Sign <b>fromlen</b> bytes of data from <b>from</b> with the private key in
 * <b>env</b>, using PKCS1 padding.  On success, write the signature to
 * <b>to</b>, and return the number of bytes written.  On failure, return
 * -1.
 */
int
crypto_pk_private_sign(crypto_pk_env_t *env, char *to,
                       const char *from, size_t fromlen)
{
  int r;
  tor_assert(env);
  tor_assert(from);
  tor_assert(to);
  tor_assert(fromlen < INT_MAX);
  if (!env->key->p)
    /* Not a private key */
    return -1;

  r = RSA_private_encrypt((int)fromlen,
                          (unsigned char*)from, (unsigned char*)to,
                          env->key, RSA_PKCS1_PADDING);
  if (r<0) {
    crypto_log_errors(LOG_WARN, "generating RSA signature");
    return -1;
  }
  return r;
}

/** Compute a SHA1 digest of <b>fromlen</b> bytes of data stored at
 * <b>from</b>; sign the data with the private key in <b>env</b>, and
 * store it in <b>to</b>.  Return the number of bytes written on
 * success, and -1 on failure.
 */
int
crypto_pk_private_sign_digest(crypto_pk_env_t *env, char *to,
                              const char *from, size_t fromlen)
{
  int r;
  char digest[DIGEST_LEN];
  if (crypto_digest(digest,from,fromlen)<0)
    return -1;
  r = crypto_pk_private_sign(env,to,digest,DIGEST_LEN);
  memset(digest, 0, sizeof(digest));
  return r;
}

/** Perform a hybrid (public/secret) encryption on <b>fromlen</b>
 * bytes of data from <b>from</b>, with padding type 'padding',
 * storing the results on <b>to</b>.
 *
 * If no padding is used, the public key must be at least as large as
 * <b>from</b>.
 *
 * Returns the number of bytes written on success, -1 on failure.
 *
 * The encrypted data consists of:
 *   - The source data, padded and encrypted with the public key, if the
 *     padded source data is no longer than the public key, and <b>force</b>
 *     is false, OR
 *   - The beginning of the source data prefixed with a 16-byte symmetric key,
 *     padded and encrypted with the public key; followed by the rest of
 *     the source data encrypted in AES-CTR mode with the symmetric key.
 */
int
crypto_pk_public_hybrid_encrypt(crypto_pk_env_t *env,
                                char *to,
                                const char *from,
                                size_t fromlen,
                                int padding, int force)
{
  int overhead, outlen, r;
  size_t pkeylen, symlen;
  crypto_cipher_env_t *cipher = NULL;
  char *buf = NULL;

  tor_assert(env);
  tor_assert(from);
  tor_assert(to);

  overhead = crypto_get_rsa_padding_overhead(crypto_get_rsa_padding(padding));
  pkeylen = crypto_pk_keysize(env);

  if (padding == PK_NO_PADDING && fromlen < pkeylen)
    return -1;

  if (!force && fromlen+overhead <= pkeylen) {
    /* It all fits in a single encrypt. */
    return crypto_pk_public_encrypt(env,to,from,fromlen,padding);
  }
  cipher = crypto_new_cipher_env();
  if (!cipher) return -1;
  if (crypto_cipher_generate_key(cipher)<0)
    goto err;
  /* You can't just run around RSA-encrypting any bitstream: if it's
   * greater than the RSA key, then OpenSSL will happily encrypt, and
   * later decrypt to the wrong value.  So we set the first bit of
   * 'cipher->key' to 0 if we aren't padding.  This means that our
   * symmetric key is really only 127 bits.
   */
  if (padding == PK_NO_PADDING)
    cipher->key[0] &= 0x7f;
  if (crypto_cipher_encrypt_init_cipher(cipher)<0)
    goto err;
  buf = tor_malloc(pkeylen+1);
  memcpy(buf, cipher->key, CIPHER_KEY_LEN);
  memcpy(buf+CIPHER_KEY_LEN, from, pkeylen-overhead-CIPHER_KEY_LEN);

  /* Length of symmetrically encrypted data. */
  symlen = fromlen-(pkeylen-overhead-CIPHER_KEY_LEN);

  outlen = crypto_pk_public_encrypt(env,to,buf,pkeylen-overhead,padding);
  if (outlen!=(int)pkeylen) {
    goto err;
  }
  r = crypto_cipher_encrypt(cipher, to+outlen,
                            from+pkeylen-overhead-CIPHER_KEY_LEN, symlen);

  if (r<0) goto err;
  memset(buf, 0, pkeylen);
  tor_free(buf);
  crypto_free_cipher_env(cipher);
  tor_assert(outlen+symlen < INT_MAX);
  return (int)(outlen + symlen);
 err:
  if (buf) {
    memset(buf, 0, pkeylen);
    tor_free(buf);
  }
  if (cipher) crypto_free_cipher_env(cipher);
  return -1;
}

/** Invert crypto_pk_public_hybrid_encrypt. */
int
crypto_pk_private_hybrid_decrypt(crypto_pk_env_t *env,
                                 char *to,
                                 const char *from,
                                 size_t fromlen,
                                 int padding, int warnOnFailure)
{
  int outlen, r;
  size_t pkeylen;
  crypto_cipher_env_t *cipher = NULL;
  char *buf = NULL;

  pkeylen = crypto_pk_keysize(env);

  if (fromlen <= pkeylen) {
    return crypto_pk_private_decrypt(env,to,from,fromlen,padding,
                                     warnOnFailure);
  }
  buf = tor_malloc(pkeylen+1);
  outlen = crypto_pk_private_decrypt(env,buf,from,pkeylen,padding,
                                     warnOnFailure);
  if (outlen<0) {
    log_fn(warnOnFailure?LOG_WARN:LOG_DEBUG, LD_CRYPTO,
           "Error decrypting public-key data");
    goto err;
  }
  if (outlen < CIPHER_KEY_LEN) {
    log_fn(warnOnFailure?LOG_WARN:LOG_INFO, LD_CRYPTO,
           "No room for a symmetric key");
    goto err;
  }
  cipher = crypto_create_init_cipher(buf, 0);
  if (!cipher) {
    goto err;
  }
  memcpy(to,buf+CIPHER_KEY_LEN,outlen-CIPHER_KEY_LEN);
  outlen -= CIPHER_KEY_LEN;
  r = crypto_cipher_decrypt(cipher, to+outlen, from+pkeylen, fromlen-pkeylen);
  if (r<0)
    goto err;
  memset(buf,0,pkeylen);
  tor_free(buf);
  crypto_free_cipher_env(cipher);
  tor_assert(outlen + fromlen < INT_MAX);
  return (int)(outlen + (fromlen-pkeylen));
 err:
  memset(buf,0,pkeylen);
  tor_free(buf);
  if (cipher) crypto_free_cipher_env(cipher);
  return -1;
}

/** ASN.1-encode the public portion of <b>pk</b> into <b>dest</b>.
 * Return -1 on error, or the number of characters used on success.
 */
int
crypto_pk_asn1_encode(crypto_pk_env_t *pk, char *dest, size_t dest_len)
{
  int len;
  unsigned char *buf, *cp;
  len = i2d_RSAPublicKey(pk->key, NULL);
  if (len < 0 || (size_t)len > dest_len)
    return -1;
  cp = buf = tor_malloc(len+1);
  len = i2d_RSAPublicKey(pk->key, &cp);
  if (len < 0) {
    crypto_log_errors(LOG_WARN,"encoding public key");
    tor_free(buf);
    return -1;
  }
  /* We don't encode directly into 'dest', because that would be illegal
   * type-punning.  (C99 is smarter than me, C99 is smarter than me...)
   */
  memcpy(dest,buf,len);
  tor_free(buf);
  return len;
}

/** Decode an ASN.1-encoded public key from <b>str</b>; return the result on
 * success and NULL on failure.
 */
crypto_pk_env_t *
crypto_pk_asn1_decode(const char *str, size_t len)
{
  RSA *rsa;
  unsigned char *buf;
  /* This ifdef suppresses a type warning.  Take out the first case once
   * everybody is using openssl 0.9.7 or later.
   */
  const unsigned char *cp;
  cp = buf = tor_malloc(len);
  memcpy(buf,str,len);
  rsa = d2i_RSAPublicKey(NULL, &cp, len);
  tor_free(buf);
  if (!rsa) {
    crypto_log_errors(LOG_WARN,"decoding public key");
    return NULL;
  }
  return _crypto_new_pk_env_rsa(rsa);
}

/** Given a private or public key <b>pk</b>, put a SHA1 hash of the
 * public key into <b>digest_out</b> (must have DIGEST_LEN bytes of space).
 * Return 0 on success, -1 on failure.
 */
int
crypto_pk_get_digest(crypto_pk_env_t *pk, char *digest_out)
{
  unsigned char *buf, *bufp;
  int len;

  len = i2d_RSAPublicKey(pk->key, NULL);
  if (len < 0)
    return -1;
  buf = bufp = tor_malloc(len+1);
  len = i2d_RSAPublicKey(pk->key, &bufp);
  if (len < 0) {
    crypto_log_errors(LOG_WARN,"encoding public key");
    tor_free(buf);
    return -1;
  }
  if (crypto_digest(digest_out, (char*)buf, len) < 0) {
    tor_free(buf);
    return -1;
  }
  tor_free(buf);
  return 0;
}

/** Copy <b>in</b> to the <b>outlen</b>-byte buffer <b>out</b>, adding spaces
 * every four spaces. */
/* static */ void
add_spaces_to_fp(char *out, size_t outlen, const char *in)
{
  int n = 0;
  char *end = out+outlen;
  while (*in && out<end) {
    *out++ = *in++;
    if (++n == 4 && *in && out<end) {
      n = 0;
      *out++ = ' ';
    }
  }
  tor_assert(out<end);
  *out = '\0';
}

/** Given a private or public key <b>pk</b>, put a fingerprint of the
 * public key into <b>fp_out</b> (must have at least FINGERPRINT_LEN+1 bytes of
 * space).  Return 0 on success, -1 on failure.
 *
 * Fingerprints are computed as the SHA1 digest of the ASN.1 encoding
 * of the public key, converted to hexadecimal, in upper case, with a
 * space after every four digits.
 *
 * If <b>add_space</b> is false, omit the spaces.
 */
int
crypto_pk_get_fingerprint(crypto_pk_env_t *pk, char *fp_out, int add_space)
{
  char digest[DIGEST_LEN];
  char hexdigest[HEX_DIGEST_LEN+1];
  if (crypto_pk_get_digest(pk, digest)) {
    return -1;
  }
  base16_encode(hexdigest,sizeof(hexdigest),digest,DIGEST_LEN);
  if (add_space) {
    add_spaces_to_fp(fp_out, FINGERPRINT_LEN+1, hexdigest);
  } else {
    strncpy(fp_out, hexdigest, HEX_DIGEST_LEN+1);
  }
  return 0;
}

/** Return true iff <b>s</b> is in the correct format for a fingerprint.
 */
int
crypto_pk_check_fingerprint_syntax(const char *s)
{
  int i;
  for (i = 0; i < FINGERPRINT_LEN; ++i) {
    if ((i%5) == 4) {
      if (!TOR_ISSPACE(s[i])) return 0;
    } else {
      if (!TOR_ISXDIGIT(s[i])) return 0;
    }
  }
  if (s[FINGERPRINT_LEN]) return 0;
  return 1;
}

/* symmetric crypto */

/** Generate a new random key for the symmetric cipher in <b>env</b>.
 * Return 0 on success, -1 on failure.  Does not initialize the cipher.
 */
int
crypto_cipher_generate_key(crypto_cipher_env_t *env)
{
  tor_assert(env);

  return crypto_rand(env->key, CIPHER_KEY_LEN);
}

/** Set the symmetric key for the cipher in <b>env</b> to the first
 * CIPHER_KEY_LEN bytes of <b>key</b>. Does not initialize the cipher.
 * Return 0 on success, -1 on failure.
 */
int
crypto_cipher_set_key(crypto_cipher_env_t *env, const char *key)
{
  tor_assert(env);
  tor_assert(key);

  if (!env->key)
    return -1;

  memcpy(env->key, key, CIPHER_KEY_LEN);
  return 0;
}

/** Generate an initialization vector for our AES-CTR cipher; store it
 * in the first CIPHER_IV_LEN bytes of <b>iv_out</b>. */
void
crypto_cipher_generate_iv(char *iv_out)
{
  crypto_rand(iv_out, CIPHER_IV_LEN);
}

/** Adjust the counter of <b>env</b> to point to the first byte of the block
 * corresponding to the encryption of the CIPHER_IV_LEN bytes at
 * <b>iv</b>.  */
int
crypto_cipher_set_iv(crypto_cipher_env_t *env, const char *iv)
{
  tor_assert(env);
  tor_assert(iv);
  aes_set_iv(env->cipher, iv);
  return 0;
}

/** Return a pointer to the key set for the cipher in <b>env</b>.
 */
const char *
crypto_cipher_get_key(crypto_cipher_env_t *env)
{
  return env->key;
}

/** Initialize the cipher in <b>env</b> for encryption.  Return 0 on
 * success, -1 on failure.
 */
int
crypto_cipher_encrypt_init_cipher(crypto_cipher_env_t *env)
{
  tor_assert(env);

  aes_set_key(env->cipher, env->key, CIPHER_KEY_LEN*8);
  return 0;
}

/** Initialize the cipher in <b>env</b> for decryption. Return 0 on
 * success, -1 on failure.
 */
int
crypto_cipher_decrypt_init_cipher(crypto_cipher_env_t *env)
{
  tor_assert(env);

  aes_set_key(env->cipher, env->key, CIPHER_KEY_LEN*8);
  return 0;
}

/** Encrypt <b>fromlen</b> bytes from <b>from</b> using the cipher
 * <b>env</b>; on success, store the result to <b>to</b> and return 0.
 * On failure, return -1.
 */
int
crypto_cipher_encrypt(crypto_cipher_env_t *env, char *to,
                      const char *from, size_t fromlen)
{
  tor_assert(env);
  tor_assert(env->cipher);
  tor_assert(from);
  tor_assert(fromlen);
  tor_assert(to);

  aes_crypt(env->cipher, from, fromlen, to);
  return 0;
}

/** Decrypt <b>fromlen</b> bytes from <b>from</b> using the cipher
 * <b>env</b>; on success, store the result to <b>to</b> and return 0.
 * On failure, return -1.
 */
int
crypto_cipher_decrypt(crypto_cipher_env_t *env, char *to,
                      const char *from, size_t fromlen)
{
  tor_assert(env);
  tor_assert(from);
  tor_assert(to);

  aes_crypt(env->cipher, from, fromlen, to);
  return 0;
}

/** Encrypt <b>len</b> bytes on <b>from</b> using the cipher in <b>env</b>;
 * on success, return 0.  On failure, return -1.
 */
int
crypto_cipher_crypt_inplace(crypto_cipher_env_t *env, char *buf, size_t len)
{
  aes_crypt_inplace(env->cipher, buf, len);
  return 0;
}

/** Encrypt <b>fromlen</b> bytes (at least 1) from <b>from</b> with the key in
 * <b>cipher</b> to the buffer in <b>to</b> of length
 * <b>tolen</b>. <b>tolen</b> must be at least <b>fromlen</b> plus
 * CIPHER_IV_LEN bytes for the initialization vector. On success, return the
 * number of bytes written, on failure, return -1.
 *
 * This function adjusts the current position of the counter in <b>cipher</b>
 * to immediately after the encrypted data.
 */
int
crypto_cipher_encrypt_with_iv(crypto_cipher_env_t *cipher,
                              char *to, size_t tolen,
                              const char *from, size_t fromlen)
{
  tor_assert(cipher);
  tor_assert(from);
  tor_assert(to);
  tor_assert(fromlen < INT_MAX);

  if (fromlen < 1)
    return -1;
  if (tolen < fromlen + CIPHER_IV_LEN)
    return -1;

  crypto_cipher_generate_iv(to);
  if (crypto_cipher_set_iv(cipher, to)<0)
    return -1;
  crypto_cipher_encrypt(cipher, to+CIPHER_IV_LEN, from, fromlen);
  return (int)(fromlen + CIPHER_IV_LEN);
}

/** Decrypt <b>fromlen</b> bytes (at least 1+CIPHER_IV_LEN) from <b>from</b>
 * with the key in <b>cipher</b> to the buffer in <b>to</b> of length
 * <b>tolen</b>. <b>tolen</b> must be at least <b>fromlen</b> minus
 * CIPHER_IV_LEN bytes for the initialization vector. On success, return the
 * number of bytes written, on failure, return -1.
 *
 * This function adjusts the current position of the counter in <b>cipher</b>
 * to immediately after the decrypted data.
 */
int
crypto_cipher_decrypt_with_iv(crypto_cipher_env_t *cipher,
                              char *to, size_t tolen,
                              const char *from, size_t fromlen)
{
  tor_assert(cipher);
  tor_assert(from);
  tor_assert(to);
  tor_assert(fromlen < INT_MAX);

  if (fromlen <= CIPHER_IV_LEN)
    return -1;
  if (tolen < fromlen - CIPHER_IV_LEN)
    return -1;

  if (crypto_cipher_set_iv(cipher, from)<0)
    return -1;
  crypto_cipher_encrypt(cipher, to, from+CIPHER_IV_LEN, fromlen-CIPHER_IV_LEN);
  return (int)(fromlen - CIPHER_IV_LEN);
}

/* SHA-1 */

/** Compute the SHA1 digest of <b>len</b> bytes in data stored in
 * <b>m</b>.  Write the DIGEST_LEN byte result into <b>digest</b>.
 * Return 0 on success, -1 on failure.
 */
int
crypto_digest(char *digest, const char *m, size_t len)
{
  tor_assert(m);
  tor_assert(digest);
  return (SHA1((const unsigned char*)m,len,(unsigned char*)digest) == NULL);
}

/** Intermediate information about the digest of a stream of data. */
struct crypto_digest_env_t {
  SHA_CTX d;
};

/** Allocate and return a new digest object.
 */
crypto_digest_env_t *
crypto_new_digest_env(void)
{
  crypto_digest_env_t *r;
  r = tor_malloc(sizeof(crypto_digest_env_t));
  SHA1_Init(&r->d);
  return r;
}

/** Deallocate a digest object.
 */
void
crypto_free_digest_env(crypto_digest_env_t *digest)
{
  memset(digest, 0, sizeof(crypto_digest_env_t));
  tor_free(digest);
}

/** Add <b>len</b> bytes from <b>data</b> to the digest object.
 */
void
crypto_digest_add_bytes(crypto_digest_env_t *digest, const char *data,
                        size_t len)
{
  tor_assert(digest);
  tor_assert(data);
  /* Using the SHA1_*() calls directly means we don't support doing
   * sha1 in hardware. But so far the delay of getting the question
   * to the hardware, and hearing the answer, is likely higher than
   * just doing it ourselves. Hashes are fast.
   */
  SHA1_Update(&digest->d, (void*)data, len);
}

/** Compute the hash of the data that has been passed to the digest
 * object; write the first out_len bytes of the result to <b>out</b>.
 * <b>out_len</b> must be \<= DIGEST_LEN.
 */
void
crypto_digest_get_digest(crypto_digest_env_t *digest,
                         char *out, size_t out_len)
{
  unsigned char r[DIGEST_LEN];
  SHA_CTX tmpctx;
  tor_assert(digest);
  tor_assert(out);
  tor_assert(out_len <= DIGEST_LEN);
  /* memcpy into a temporary ctx, since SHA1_Final clears the context */
  memcpy(&tmpctx, &digest->d, sizeof(SHA_CTX));
  SHA1_Final(r, &tmpctx);
  memcpy(out, r, out_len);
  memset(r, 0, sizeof(r));
}

/** Allocate and return a new digest object with the same state as
 * <b>digest</b>
 */
crypto_digest_env_t *
crypto_digest_dup(const crypto_digest_env_t *digest)
{
  crypto_digest_env_t *r;
  tor_assert(digest);
  r = tor_malloc(sizeof(crypto_digest_env_t));
  memcpy(r,digest,sizeof(crypto_digest_env_t));
  return r;
}

/** Replace the state of the digest object <b>into</b> with the state
 * of the digest object <b>from</b>.
 */
void
crypto_digest_assign(crypto_digest_env_t *into,
                     const crypto_digest_env_t *from)
{
  tor_assert(into);
  tor_assert(from);
  memcpy(into,from,sizeof(crypto_digest_env_t));
}

/** Compute the HMAC-SHA-1 of the <b>msg_len</b> bytes in <b>msg</b>, using
 * the <b>key</b> of length <b>key_len</b>.  Store the DIGEST_LEN-byte result
 * in <b>hmac_out</b>.
 */
void
crypto_hmac_sha1(char *hmac_out,
                 const char *key, size_t key_len,
                 const char *msg, size_t msg_len)
{
  tor_assert(key_len < INT_MAX);
  tor_assert(msg_len < INT_MAX);
  HMAC(EVP_sha1(), key, (int)key_len, (unsigned char*)msg, (int)msg_len,
       (unsigned char*)hmac_out, NULL);
}

/* DH */

/** Shared P parameter for our DH key exchanged. */
static BIGNUM *dh_param_p = NULL;
/** Shared G parameter for our DH key exchanges. */
static BIGNUM *dh_param_g = NULL;

/** Initialize dh_param_p and dh_param_g if they are not already
 * set. */
static void
init_dh_param(void)
{
  BIGNUM *p, *g;
  int r;
  if (dh_param_p && dh_param_g)
    return;

  p = BN_new();
  g = BN_new();
  tor_assert(p);
  tor_assert(g);

  /* This is from rfc2409, section 6.2.  It's a safe prime, and
     supposedly it equals:
        2^1024 - 2^960 - 1 + 2^64 * { [2^894 pi] + 129093 }.
  */
  r = BN_hex2bn(&p,
                "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E08"
                "8A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B"
                "302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9"
                "A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE6"
                "49286651ECE65381FFFFFFFFFFFFFFFF");
  tor_assert(r);

  r = BN_set_word(g, 2);
  tor_assert(r);
  dh_param_p = p;
  dh_param_g = g;
}

#define DH_PRIVATE_KEY_BITS 320

/** Allocate and return a new DH object for a key exchange.
 */
crypto_dh_env_t *
crypto_dh_new(void)
{
  crypto_dh_env_t *res = tor_malloc_zero(sizeof(crypto_dh_env_t));

  if (!dh_param_p)
    init_dh_param();

  if (!(res->dh = DH_new()))
    goto err;

  if (!(res->dh->p = BN_dup(dh_param_p)))
    goto err;

  if (!(res->dh->g = BN_dup(dh_param_g)))
    goto err;

  res->dh->length = DH_PRIVATE_KEY_BITS;

  return res;
 err:
  crypto_log_errors(LOG_WARN, "creating DH object");
  if (res->dh) DH_free(res->dh); /* frees p and g too */
  tor_free(res);
  return NULL;
}

/** Return the length of the DH key in <b>dh</b>, in bytes.
 */
int
crypto_dh_get_bytes(crypto_dh_env_t *dh)
{
  tor_assert(dh);
  return DH_size(dh->dh);
}

/** Generate \<x,g^x\> for our part of the key exchange.  Return 0 on
 * success, -1 on failure.
 */
int
crypto_dh_generate_public(crypto_dh_env_t *dh)
{
 again:
  if (!DH_generate_key(dh->dh)) {
    crypto_log_errors(LOG_WARN, "generating DH key");
    return -1;
  }
  if (tor_check_dh_key(dh->dh->pub_key)<0) {
    log_warn(LD_CRYPTO, "Weird! Our own DH key was invalid.  I guess once-in-"
             "the-universe chances really do happen.  Trying again.");
    /* Free and clear the keys, so openssl will actually try again. */
    BN_free(dh->dh->pub_key);
    BN_free(dh->dh->priv_key);
    dh->dh->pub_key = dh->dh->priv_key = NULL;
    goto again;
  }
  return 0;
}

/** Generate g^x as necessary, and write the g^x for the key exchange
 * as a <b>pubkey_len</b>-byte value into <b>pubkey</b>. Return 0 on
 * success, -1 on failure.  <b>pubkey_len</b> must be \>= DH_BYTES.
 */
int
crypto_dh_get_public(crypto_dh_env_t *dh, char *pubkey, size_t pubkey_len)
{
  int bytes;
  tor_assert(dh);
  if (!dh->dh->pub_key) {
    if (crypto_dh_generate_public(dh)<0)
      return -1;
  }

  tor_assert(dh->dh->pub_key);
  bytes = BN_num_bytes(dh->dh->pub_key);
  tor_assert(bytes >= 0);
  if (pubkey_len < (size_t)bytes) {
    log_warn(LD_CRYPTO,
             "Weird! pubkey_len (%d) was smaller than DH_BYTES (%d)",
             (int) pubkey_len, bytes);
    return -1;
  }

  memset(pubkey, 0, pubkey_len);
  BN_bn2bin(dh->dh->pub_key, (unsigned char*)(pubkey+(pubkey_len-bytes)));

  return 0;
}

/** Check for bad diffie-hellman public keys (g^x).  Return 0 if the key is
 * okay (in the subgroup [2,p-2]), or -1 if it's bad.
 * See http://www.cl.cam.ac.uk/ftp/users/rja14/psandqs.ps.gz for some tips.
 */
static int
tor_check_dh_key(BIGNUM *bn)
{
  BIGNUM *x;
  char *s;
  tor_assert(bn);
  x = BN_new();
  tor_assert(x);
  if (!dh_param_p)
    init_dh_param();
  BN_set_word(x, 1);
  if (BN_cmp(bn,x)<=0) {
    log_warn(LD_CRYPTO, "DH key must be at least 2.");
    goto err;
  }
  BN_copy(x,dh_param_p);
  BN_sub_word(x, 1);
  if (BN_cmp(bn,x)>=0) {
    log_warn(LD_CRYPTO, "DH key must be at most p-2.");
    goto err;
  }
  BN_free(x);
  return 0;
 err:
  BN_free(x);
  s = BN_bn2hex(bn);
  log_warn(LD_CRYPTO, "Rejecting insecure DH key [%s]", s);
  OPENSSL_free(s);
  return -1;
}

#undef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
/** Given a DH key exchange object, and our peer's value of g^y (as a
 * <b>pubkey_len</b>-byte value in <b>pubkey</b>) generate
 * <b>secret_bytes_out</b> bytes of shared key material and write them
 * to <b>secret_out</b>.  Return the number of bytes generated on success,
 * or -1 on failure.
 *
 * (We generate key material by computing
 *         SHA1( g^xy || "\x00" ) || SHA1( g^xy || "\x01" ) || ...
 * where || is concatenation.)
 */
ssize_t
crypto_dh_compute_secret(crypto_dh_env_t *dh,
                         const char *pubkey, size_t pubkey_len,
                         char *secret_out, size_t secret_bytes_out)
{
  char *secret_tmp = NULL;
  BIGNUM *pubkey_bn = NULL;
  size_t secret_len=0;
  int result=0;
  tor_assert(dh);
  tor_assert(secret_bytes_out/DIGEST_LEN <= 255);
  tor_assert(pubkey_len < INT_MAX);

  if (!(pubkey_bn = BN_bin2bn((const unsigned char*)pubkey,
                              (int)pubkey_len, NULL)))
    goto error;
  if (tor_check_dh_key(pubkey_bn)<0) {
    /* Check for invalid public keys. */
    log_warn(LD_CRYPTO,"Rejected invalid g^x");
    goto error;
  }
  secret_tmp = tor_malloc(crypto_dh_get_bytes(dh));
  result = DH_compute_key((unsigned char*)secret_tmp, pubkey_bn, dh->dh);
  if (result < 0) {
    log_warn(LD_CRYPTO,"DH_compute_key() failed.");
    goto error;
  }
  secret_len = result;
  if (crypto_expand_key_material(secret_tmp, secret_len,
                                 secret_out, secret_bytes_out)<0)
    goto error;
  secret_len = secret_bytes_out;

  goto done;
 error:
  result = -1;
 done:
  crypto_log_errors(LOG_WARN, "completing DH handshake");
  if (pubkey_bn)
    BN_free(pubkey_bn);
  tor_free(secret_tmp);
  if (result < 0)
    return result;
  else
    return secret_len;
}

/** Given <b>key_in_len</b> bytes of negotiated randomness in <b>key_in</b>
 * ("K"), expand it into <b>key_out_len</b> bytes of negotiated key material in
 * <b>key_out</b> by taking the first <b>key_out_len</b> bytes of
 *    H(K | [00]) | H(K | [01]) | ....
 *
 * Return 0 on success, -1 on failure.
 */
int
crypto_expand_key_material(const char *key_in, size_t key_in_len,
                           char *key_out, size_t key_out_len)
{
  int i;
  char *cp, *tmp = tor_malloc(key_in_len+1);
  char digest[DIGEST_LEN];

  /* If we try to get more than this amount of key data, we'll repeat blocks.*/
  tor_assert(key_out_len <= DIGEST_LEN*256);

  memcpy(tmp, key_in, key_in_len);
  for (cp = key_out, i=0; cp < key_out+key_out_len;
       ++i, cp += DIGEST_LEN) {
    tmp[key_in_len] = i;
    if (crypto_digest(digest, tmp, key_in_len+1))
      goto err;
    memcpy(cp, digest, MIN(DIGEST_LEN, key_out_len-(cp-key_out)));
  }
  memset(tmp, 0, key_in_len+1);
  tor_free(tmp);
  memset(digest, 0, sizeof(digest));
  return 0;

 err:
  memset(tmp, 0, key_in_len+1);
  tor_free(tmp);
  memset(digest, 0, sizeof(digest));
  return -1;
}

/** Free a DH key exchange object.
 */
void
crypto_dh_free(crypto_dh_env_t *dh)
{
  tor_assert(dh);
  tor_assert(dh->dh);
  DH_free(dh->dh);
  tor_free(dh);
}

/* random numbers */

/* This is how much entropy OpenSSL likes to add right now, so maybe it will
 * work for us too. */
#define ADD_ENTROPY 32

/* Use RAND_poll if openssl is 0.9.6 release or later.  (The "f" means
   "release".)  */
#define HAVE_RAND_POLL (OPENSSL_VERSION_NUMBER >= 0x0090600fl)

/* Versions of openssl prior to 0.9.7k and 0.9.8c had a bug where RAND_poll
 * would allocate an fd_set on the stack, open a new file, and try to FD_SET
 * that fd without checking whether it fit in the fd_set.  Thus, if the
 * system has not just been started up, it is unsafe to call */
#define RAND_POLL_IS_SAFE                       \
  ((OPENSSL_VERSION_NUMBER >= 0x009070afl &&    \
    OPENSSL_VERSION_NUMBER <= 0x00907fffl) ||   \
   (OPENSSL_VERSION_NUMBER >= 0x0090803fl))

/** Seed OpenSSL's random number generator with bytes from the operating
 * system.  <b>startup</b> should be true iff we have just started Tor and
 * have not yet allocated a bunch of fds.  Return 0 on success, -1 on failure.
 */
int
crypto_seed_rng(int startup)
{
  char buf[ADD_ENTROPY];
  int rand_poll_status = 0;

  /* local variables */
#ifdef MS_WINDOWS
  static int provider_set = 0;
  static HCRYPTPROV provider;
#else
  static const char *filenames[] = {
    "/dev/srandom", "/dev/urandom", "/dev/random", NULL
  };
  int fd, i;
  size_t n;
#endif

#if HAVE_RAND_POLL
  /* OpenSSL 0.9.6 adds a RAND_poll function that knows about more kinds of
   * entropy than we do.  We'll try calling that, *and* calling our own entropy
   * functions.  If one succeeds, we'll accept the RNG as seeded. */
  if (startup || RAND_POLL_IS_SAFE) {
    rand_poll_status = RAND_poll();
    if (rand_poll_status == 0)
      log_warn(LD_CRYPTO, "RAND_poll() failed.");
  }
#endif

#ifdef MS_WINDOWS
  if (!provider_set) {
    if (!CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT)) {
      if ((unsigned long)GetLastError() != (unsigned long)NTE_BAD_KEYSET) {
        log_warn(LD_CRYPTO, "Can't get CryptoAPI provider [1]");
        return rand_poll_status ? 0 : -1;
      }
    }
    provider_set = 1;
  }
  if (!CryptGenRandom(provider, sizeof(buf), buf)) {
    log_warn(LD_CRYPTO, "Can't get entropy from CryptoAPI.");
    return rand_poll_status ? 0 : -1;
  }
  RAND_seed(buf, sizeof(buf));
  memset(buf, 0, sizeof(buf));
  return 0;
#else
  for (i = 0; filenames[i]; ++i) {
    fd = open(filenames[i], O_RDONLY, 0);
    if (fd<0) continue;
    log_info(LD_CRYPTO, "Seeding RNG from \"%s\"", filenames[i]);
    n = read_all(fd, buf, sizeof(buf), 0);
    close(fd);
    if (n != sizeof(buf)) {
      log_warn(LD_CRYPTO,
               "Error reading from entropy source (read only %lu bytes).",
               (unsigned long)n);
      return -1;
    }
    RAND_seed(buf, (int)sizeof(buf));
    memset(buf, 0, sizeof(buf));
    return 0;
  }

  log_warn(LD_CRYPTO, "Cannot seed RNG -- no entropy source found.");
  return rand_poll_status ? 0 : -1;
#endif
}

/** Write <b>n</b> bytes of strong random data to <b>to</b>. Return 0 on
 * success, -1 on failure.
 */
int
crypto_rand(char *to, size_t n)
{
  int r;
  tor_assert(n < INT_MAX);
  tor_assert(to);
  r = RAND_bytes((unsigned char*)to, (int)n);
  if (r == 0)
    crypto_log_errors(LOG_WARN, "generating random data");
  return (r == 1) ? 0 : -1;
}

/** Return a pseudorandom integer, chosen uniformly from the values
 * between 0 and <b>max</b>-1. */
int
crypto_rand_int(unsigned int max)
{
  unsigned int val;
  unsigned int cutoff;
  tor_assert(max < UINT_MAX);
  tor_assert(max > 0); /* don't div by 0 */

  /* We ignore any values that are >= 'cutoff,' to avoid biasing the
   * distribution with clipping at the upper end of unsigned int's
   * range.
   */
  cutoff = UINT_MAX - (UINT_MAX%max);
  while (1) {
    crypto_rand((char*)&val, sizeof(val));
    if (val < cutoff)
      return val % max;
  }
}

/** Return a pseudorandom 64-bit integer, chosen uniformly from the values
 * between 0 and <b>max</b>-1. */
uint64_t
crypto_rand_uint64(uint64_t max)
{
  uint64_t val;
  uint64_t cutoff;
  tor_assert(max < UINT64_MAX);
  tor_assert(max > 0); /* don't div by 0 */

  /* We ignore any values that are >= 'cutoff,' to avoid biasing the
   * distribution with clipping at the upper end of unsigned int's
   * range.
   */
  cutoff = UINT64_MAX - (UINT64_MAX%max);
  while (1) {
    crypto_rand((char*)&val, sizeof(val));
    if (val < cutoff)
      return val % max;
  }
}

/** Generate and return a new random hostname starting with <b>prefix</b>,
 * ending with <b>suffix</b>, and containing no less than
 * <b>min_rand_len</b> and no more than <b>max_rand_len</b> random base32
 * characters between. */
char *
crypto_random_hostname(int min_rand_len, int max_rand_len, const char *prefix,
                       const char *suffix)
{
  char *result, *rand_bytes;
  int randlen, rand_bytes_len;
  size_t resultlen, prefixlen;

  tor_assert(max_rand_len >= min_rand_len);
  randlen = min_rand_len + crypto_rand_int(max_rand_len - min_rand_len + 1);
  prefixlen = strlen(prefix);
  resultlen = prefixlen + strlen(suffix) + randlen + 16;

  rand_bytes_len = ((randlen*5)+7)/8;
  if (rand_bytes_len % 5)
    rand_bytes_len += 5 - (rand_bytes_len%5);
  rand_bytes = tor_malloc(rand_bytes_len);
  crypto_rand(rand_bytes, rand_bytes_len);

  result = tor_malloc(resultlen);
  memcpy(result, prefix, prefixlen);
  base32_encode(result+prefixlen, resultlen-prefixlen,
                rand_bytes, rand_bytes_len);
  tor_free(rand_bytes);
  strlcpy(result+prefixlen+randlen, suffix, resultlen-(prefixlen+randlen));

  return result;
}

/** Return a randomly chosen element of <b>sl</b>; or NULL if <b>sl</b>
 * is empty. */
void *
smartlist_choose(const smartlist_t *sl)
{
  int len = smartlist_len(sl);
  if (len)
    return smartlist_get(sl,crypto_rand_int(len));
  return NULL; /* no elements to choose from */
}

/** Scramble the elements of <b>sl</b> into a random order. */
void
smartlist_shuffle(smartlist_t *sl)
{
  int i;
  /* From the end of the list to the front, choose at random from the
     positions we haven't looked at yet, and swap that position into the
     current position.  Remember to give "no swap" the same probability as
     any other swap. */
  for (i = smartlist_len(sl)-1; i > 0; --i) {
    int j = crypto_rand_int(i+1);
    smartlist_swap(sl, i, j);
  }
}

/** Base-64 encode <b>srclen</b> bytes of data from <b>src</b>.  Write
 * the result into <b>dest</b>, if it will fit within <b>destlen</b>
 * bytes.  Return the number of bytes written on success; -1 if
 * destlen is too short, or other failure.
 */
int
base64_encode(char *dest, size_t destlen, const char *src, size_t srclen)
{
  /* FFFF we might want to rewrite this along the lines of base64_decode, if
   * it ever shows up in the profile. */
  EVP_ENCODE_CTX ctx;
  int len, ret;
  tor_assert(srclen < INT_MAX);

  /* 48 bytes of input -> 64 bytes of output plus newline.
     Plus one more byte, in case I'm wrong.
  */
  if (destlen < ((srclen/48)+1)*66)
    return -1;
  if (destlen > SIZE_T_CEILING)
    return -1;

  EVP_EncodeInit(&ctx);
  EVP_EncodeUpdate(&ctx, (unsigned char*)dest, &len,
                   (unsigned char*)src, (int)srclen);
  EVP_EncodeFinal(&ctx, (unsigned char*)(dest+len), &ret);
  ret += len;
  return ret;
}

#define X 255
#define SP 64
#define PAD 65
/** Internal table mapping byte values to what they represent in base64.
 * Numbers 0..63 are 6-bit integers.  SPs are spaces, and should be
 * skipped.  Xs are invalid and must not appear in base64. PAD indicates
 * end-of-string. */
static const uint8_t base64_decode_table[256] = {
  X, X, X, X, X, X, X, X, X, SP, SP, SP, X, SP, X, X, /* */
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  SP, X, X, X, X, X, X, X, X, X, X, 62, X, X, X, 63,
  52, 53, 54, 55, 56, 57, 58, 59, 60, 61, X, X, X, PAD, X, X,
  X, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
  15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, X, X, X, X, X,
  X, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
  41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
  X, X, X, X, X, X, X, X, X, X, X, X, X, X, X, X,
};

/** Base-64 decode <b>srclen</b> bytes of data from <b>src</b>.  Write
 * the result into <b>dest</b>, if it will fit within <b>destlen</b>
 * bytes.  Return the number of bytes written on success; -1 if
 * destlen is too short, or other failure.
 *
 * NOTE 1: destlen is checked conservatively, as though srclen contained no
 * spaces or padding.
 *
 * NOTE 2: This implementation does not check for the correct number of
 * padding "=" characters at the end of the string, and does not check
 * for internal padding characters.
 */
int
base64_decode(char *dest, size_t destlen, const char *src, size_t srclen)
{
#ifdef USE_OPENSSL_BASE64
  EVP_ENCODE_CTX ctx;
  int len, ret;
  /* 64 bytes of input -> *up to* 48 bytes of output.
     Plus one more byte, in case I'm wrong.
  */
  if (destlen < ((srclen/64)+1)*49)
    return -1;
  if (destlen > SIZE_T_CEILING)
    return -1;

  EVP_DecodeInit(&ctx);
  EVP_DecodeUpdate(&ctx, (unsigned char*)dest, &len,
                   (unsigned char*)src, srclen);
  EVP_DecodeFinal(&ctx, (unsigned char*)dest, &ret);
  ret += len;
  return ret;
#else
  const char *eos = src+srclen;
  uint32_t n=0;
  int n_idx=0;
  char *dest_orig = dest;

  /* Max number of bits == srclen*6.
   * Number of bytes required to hold all bits == (srclen*6)/8.
   * Yes, we want to round down: anything that hangs over the end of a
   * byte is padding. */
  if (destlen < (srclen*3)/4)
    return -1;
  if (destlen > SIZE_T_CEILING)
    return -1;

  /* Iterate over all the bytes in src.  Each one will add 0 or 6 bits to the
   * value we're decoding.  Accumulate bits in <b>n</b>, and whenever we have
   * 24 bits, batch them into 3 bytes and flush those bytes to dest.
   */
  for ( ; src < eos; ++src) {
    unsigned char c = (unsigned char) *src;
    uint8_t v = base64_decode_table[c];
    switch (v) {
      case X:
        /* This character isn't allowed in base64. */
        return -1;
      case SP:
        /* This character is whitespace, and has no effect. */
        continue;
      case PAD:
        /* We've hit an = character: the data is over. */
        goto end_of_loop;
      default:
        /* We have an actual 6-bit value.  Append it to the bits in n. */
        n = (n<<6) | v;
        if ((++n_idx) == 4) {
          /* We've accumulated 24 bits in n. Flush them. */
          *dest++ = (n>>16);
          *dest++ = (n>>8) & 0xff;
          *dest++ = (n) & 0xff;
          n_idx = 0;
          n = 0;
        }
    }
  }
 end_of_loop:
  /* If we have leftover bits, we need to cope. */
  switch (n_idx) {
    case 0:
    default:
      /* No leftover bits.  We win. */
      break;
    case 1:
      /* 6 leftover bits. That's invalid; we can't form a byte out of that. */
      return -1;
    case 2:
      /* 12 leftover bits: The last 4 are padding and the first 8 are data. */
      *dest++ = n >> 4;
      break;
    case 3:
      /* 18 leftover bits: The last 2 are padding and the first 16 are data. */
      *dest++ = n >> 10;
      *dest++ = n >> 2;
  }

  tor_assert((dest-dest_orig) <= (ssize_t)destlen);
  tor_assert((dest-dest_orig) <= INT_MAX);

  return (int)(dest-dest_orig);
#endif
}
#undef X
#undef SP
#undef PAD

/** Base-64 encode DIGEST_LINE bytes from <b>digest</b>, remove the trailing =
 * and newline characters, and store the nul-terminated result in the first
 * BASE64_DIGEST_LEN+1 bytes of <b>d64</b>.  */
int
digest_to_base64(char *d64, const char *digest)
{
  char buf[256];
  base64_encode(buf, sizeof(buf), digest, DIGEST_LEN);
  buf[BASE64_DIGEST_LEN] = '\0';
  memcpy(d64, buf, BASE64_DIGEST_LEN+1);
  return 0;
}

/** Given a base-64 encoded, nul-terminated digest in <b>d64</b> (without
 * trailing newline or = characters), decode it and store the result in the
 * first DIGEST_LEN bytes at <b>digest</b>. */
int
digest_from_base64(char *digest, const char *d64)
{
#ifdef USE_OPENSSL_BASE64
  char buf_in[BASE64_DIGEST_LEN+3];
  char buf[256];
  if (strlen(d64) != BASE64_DIGEST_LEN)
    return -1;
  memcpy(buf_in, d64, BASE64_DIGEST_LEN);
  memcpy(buf_in+BASE64_DIGEST_LEN, "=\n\0", 3);
  if (base64_decode(buf, sizeof(buf), buf_in, strlen(buf_in)) != DIGEST_LEN)
    return -1;
  memcpy(digest, buf, DIGEST_LEN);
  return 0;
#else
  if (base64_decode(digest, DIGEST_LEN, d64, strlen(d64)) == DIGEST_LEN)
    return 0;
  else
    return -1;
#endif
}

/** Implements base32 encoding as in rfc3548.  Limitation: Requires
 * that srclen*8 is a multiple of 5.
 */
void
base32_encode(char *dest, size_t destlen, const char *src, size_t srclen)
{
  unsigned int i, bit, v, u;
  size_t nbits = srclen * 8;

  tor_assert((nbits%5) == 0); /* We need an even multiple of 5 bits. */
  tor_assert((nbits/5)+1 <= destlen); /* We need enough space. */
  tor_assert(destlen < SIZE_T_CEILING);

  for (i=0,bit=0; bit < nbits; ++i, bit+=5) {
    /* set v to the 16-bit value starting at src[bits/8], 0-padded. */
    v = ((uint8_t)src[bit/8]) << 8;
    if (bit+5<nbits) v += (uint8_t)src[(bit/8)+1];
    /* set u to the 5-bit value at the bit'th bit of src. */
    u = (v >> (11-(bit%8))) & 0x1F;
    dest[i] = BASE32_CHARS[u];
  }
  dest[i] = '\0';
}

/** Implements base32 decoding as in rfc3548.  Limitation: Requires
 * that srclen*5 is a multiple of 8. Returns 0 if successful, -1 otherwise.
 */
int
base32_decode(char *dest, size_t destlen, const char *src, size_t srclen)
{
  /* XXXX we might want to rewrite this along the lines of base64_decode, if
   * it ever shows up in the profile. */
  unsigned int i, j, bit;
  size_t nbits;
  char *tmp;
  nbits = srclen * 5;

  tor_assert((nbits%8) == 0); /* We need an even multiple of 8 bits. */
  tor_assert((nbits/8) <= destlen); /* We need enough space. */
  tor_assert(destlen < SIZE_T_CEILING);

  /* Convert base32 encoded chars to the 5-bit values that they represent. */
  tmp = tor_malloc_zero(srclen);
  for (j = 0; j < srclen; ++j) {
    if (src[j] > 0x60 && src[j] < 0x7B) tmp[j] = src[j] - 0x61;
    else if (src[j] > 0x31 && src[j] < 0x38) tmp[j] = src[j] - 0x18;
    else if (src[j] > 0x40 && src[j] < 0x5B) tmp[j] = src[j] - 0x41;
    else {
      log_warn(LD_BUG, "illegal character in base32 encoded string");
      tor_free(tmp);
      return -1;
    }
  }

  /* Assemble result byte-wise by applying five possible cases. */
  for (i = 0, bit = 0; bit < nbits; ++i, bit += 8) {
    switch (bit % 40) {
    case 0:
      dest[i] = (((uint8_t)tmp[(bit/5)]) << 3) +
                (((uint8_t)tmp[(bit/5)+1]) >> 2);
      break;
    case 8:
      dest[i] = (((uint8_t)tmp[(bit/5)]) << 6) +
                (((uint8_t)tmp[(bit/5)+1]) << 1) +
                (((uint8_t)tmp[(bit/5)+2]) >> 4);
      break;
    case 16:
      dest[i] = (((uint8_t)tmp[(bit/5)]) << 4) +
                (((uint8_t)tmp[(bit/5)+1]) >> 1);
      break;
    case 24:
      dest[i] = (((uint8_t)tmp[(bit/5)]) << 7) +
                (((uint8_t)tmp[(bit/5)+1]) << 2) +
                (((uint8_t)tmp[(bit/5)+2]) >> 3);
      break;
    case 32:
      dest[i] = (((uint8_t)tmp[(bit/5)]) << 5) +
                ((uint8_t)tmp[(bit/5)+1]);
      break;
    }
  }

  memset(tmp, 0, srclen);
  tor_free(tmp);
  tmp = NULL;
  return 0;
}

/** Implement RFC2440-style iterated-salted S2K conversion: convert the
 * <b>secret_len</b>-byte <b>secret</b> into a <b>key_out_len</b> byte
 * <b>key_out</b>.  As in RFC2440, the first 8 bytes of s2k_specifier
 * are a salt; the 9th byte describes how much iteration to do.
 * Does not support <b>key_out_len</b> &gt; DIGEST_LEN.
 */
void
secret_to_key(char *key_out, size_t key_out_len, const char *secret,
              size_t secret_len, const char *s2k_specifier)
{
  crypto_digest_env_t *d;
  uint8_t c;
  size_t count, tmplen;
  char *tmp;
  tor_assert(key_out_len < SIZE_T_CEILING);

#define EXPBIAS 6
  c = s2k_specifier[8];
  count = ((uint32_t)16 + (c & 15)) << ((c >> 4) + EXPBIAS);
#undef EXPBIAS

  tor_assert(key_out_len <= DIGEST_LEN);

  d = crypto_new_digest_env();
  tmplen = 8+secret_len;
  tmp = tor_malloc(tmplen);
  memcpy(tmp,s2k_specifier,8);
  memcpy(tmp+8,secret,secret_len);
  secret_len += 8;
  while (count) {
    if (count >= secret_len) {
      crypto_digest_add_bytes(d, tmp, secret_len);
      count -= secret_len;
    } else {
      crypto_digest_add_bytes(d, tmp, count);
      count = 0;
    }
  }
  crypto_digest_get_digest(d, key_out, key_out_len);
  memset(tmp, 0, tmplen);
  tor_free(tmp);
  crypto_free_digest_env(d);
}

#ifdef TOR_IS_MULTITHREADED
/** Helper: openssl uses this callback to manipulate mutexes. */
static void
_openssl_locking_cb(int mode, int n, const char *file, int line)
{
  (void)file;
  (void)line;
  if (!_openssl_mutexes)
    /* This is not a really good  fix for the
     * "release-freed-lock-from-separate-thread-on-shutdown" problem, but
     * it can't hurt. */
    return;
  if (mode & CRYPTO_LOCK)
    tor_mutex_acquire(_openssl_mutexes[n]);
  else
    tor_mutex_release(_openssl_mutexes[n]);
}

/** OpenSSL helper type: wraps a Tor mutex so that openssl can  */
struct CRYPTO_dynlock_value {
  tor_mutex_t *lock;
};

/** Openssl callback function to allocate a lock: see CRYPTO_set_dynlock_*
 * documentation in OpenSSL's docs for more info. */
static struct CRYPTO_dynlock_value *
_openssl_dynlock_create_cb(const char *file, int line)
{
  struct CRYPTO_dynlock_value *v;
  (void)file;
  (void)line;
  v = tor_malloc(sizeof(struct CRYPTO_dynlock_value));
  v->lock = tor_mutex_new();
  return v;
}

/** Openssl callback function to acquire or release a lock: see
 * CRYPTO_set_dynlock_* documentation in OpenSSL's docs for more info. */
static void
_openssl_dynlock_lock_cb(int mode, struct CRYPTO_dynlock_value *v,
                         const char *file, int line)
{
  (void)file;
  (void)line;
  if (mode & CRYPTO_LOCK)
    tor_mutex_acquire(v->lock);
  else
    tor_mutex_release(v->lock);
}

/** Openssl callback function to free a lock: see CRYPTO_set_dynlock_*
 * documentation in OpenSSL's docs for more info. */
static void
_openssl_dynlock_destroy_cb(struct CRYPTO_dynlock_value *v,
                            const char *file, int line)
{
  (void)file;
  (void)line;
  tor_mutex_free(v->lock);
  tor_free(v);
}

/** Helper: Construct mutexes, and set callbacks to help OpenSSL handle being
 * multithreaded. */
static int
setup_openssl_threading(void)
{
  int i;
  int n = CRYPTO_num_locks();
  _n_openssl_mutexes = n;
  _openssl_mutexes = tor_malloc(n*sizeof(tor_mutex_t *));
  for (i=0; i < n; ++i)
    _openssl_mutexes[i] = tor_mutex_new();
  CRYPTO_set_locking_callback(_openssl_locking_cb);
  CRYPTO_set_id_callback(tor_get_thread_id);
  CRYPTO_set_dynlock_create_callback(_openssl_dynlock_create_cb);
  CRYPTO_set_dynlock_lock_callback(_openssl_dynlock_lock_cb);
  CRYPTO_set_dynlock_destroy_callback(_openssl_dynlock_destroy_cb);
  return 0;
}
#else
static int
setup_openssl_threading(void)
{
  return 0;
}
#endif

