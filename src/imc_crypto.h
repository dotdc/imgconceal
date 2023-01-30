#ifndef _IMC_CRYPTO_H
#define _IMC_CRYPTO_H

#include "imc_includes.h"

// Password hashing parameters
#define IMC_OPSLIMIT 3          // Amount of operations
#define IMC_MEMLIMIT 4096000    // Amount of memory
#define IMC_MEMLIMIT_REHASH 8192    // Memory used when re-hashing

// Amount of bytes that will be added to the encrypted stream, in relation to the unencrypted data
// This value includes the 17 bytes that libsodium adds, plus the 4 characters signature, 4 bytes
// for the version number, and 4 bytes for storing the size of the stream following it.
#define IMC_HEADER_OVERHEAD 12
#define IMC_CRYPTO_OVERHEAD crypto_secretstream_xchacha20poly1305_ABYTES + IMC_HEADER_OVERHEAD

// Signature that this program will add to the beginning of the data stream that was hidden
#define IMC_CRYPTO_MAGIC "imcl"

// Stores the secret key for encryption and the seed of the pseudorandom number generator
typedef struct CryptoContext
{
    uint8_t xcc20_key[crypto_secretstream_xchacha20poly1305_KEYBYTES];
    uint64_t bbs_seed;
    uint64_t bbs_mod;
} CryptoContext;

// Generate cryptographic secrets key from a password
int imc_crypto_context_create(const char *password, CryptoContext **out);

// Pseudorandom number generator using the Blum Blum Shub algorithm
// It writes a given amount of bytes to the output, while taking into account the endianness of the system.
void imc_crypto_prng(CryptoContext *state, size_t num_bytes, uint8_t *output);

// Generate an unsigned 64-bit integer that can be up to the 'max' value (inclusive)
uint64_t imc_crypto_prng_uint64(CryptoContext *state, uint64_t max);

// Randomize the order of the elements in an array of pointers
void imc_crypto_shuffle_ptr(CryptoContext *state, uintptr_t *array, size_t num_elements);

// Encrypt a data stream
int imc_crypto_encrypt(
    CryptoContext *state,
    const uint8_t *const data,
    unsigned long long data_len,
    uint8_t *output,
    unsigned long long *output_len
);

// Free the memory used by the cryptographic secrets
void imc_crypto_context_destroy(CryptoContext *state);

#endif  // _IMC_CRYPTO_H