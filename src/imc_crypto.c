/* Encryption, decryption, and pseudo-random number generation. */

#include "imc_includes.h"

// Generate cryptographic secrets key from a password
int imc_crypto_context_create(const PassBuff *password, CryptoContext **out)
{
    // Salt for generating a secret key from a password
    uint8_t salt[crypto_pwhash_SALTBYTES];
    memset(salt, 0, sizeof(salt));
    size_t salt_len = strlen(IMC_SALT);
    if (salt_len > crypto_pwhash_SALTBYTES) salt_len = crypto_pwhash_SALTBYTES;
    memcpy(salt, IMC_SALT, salt_len);
    
    // Storage for the secret key and the state of the pseudorandom number generator (PRNG)
    CryptoContext *context = sodium_malloc(sizeof(CryptoContext));
    if (!context) return IMC_ERR_NO_MEMORY;
    sodium_memzero(context, sizeof(CryptoContext));

    // Seed for the PRNG
    uint64_t prng_seed[4];
    sodium_mlock(prng_seed, sizeof(prng_seed));
    
    // Storage for the password hash
    const size_t key_size = sizeof(context->xcc20_key);
    const size_t seed_size = sizeof(prng_seed);
    const size_t out_len = key_size + seed_size;
    uint8_t output[out_len];
    sodium_mlock(output, sizeof(output));
    
    // Password hashing: generate enough bytes for both the secret key and the PRNG seed
    int status = crypto_pwhash(
        (uint8_t * const)&output,   // Output buffer for the hash
        sizeof(output),             // Size in bytes of the output buffer
        password->buffer,           // Input buffer with the password
        password->length,           // Size in bytes of the input buffer
        salt,                       // Salt to be appended to the password
        IMC_OPSLIMIT,               // Amount of times that the hashing is repeated
        IMC_MEMLIMIT,               // Amount of memory used for hashing
        crypto_pwhash_ALG_ARGON2ID13    // Hashing algorithm
    );
    if (status < 0) return IMC_ERR_NO_MEMORY;

    // The lower bytes are used for the key (32 bytes)
    memcpy(&context->xcc20_key, &output[0], key_size);

    // The upper bytes are used for the seed: four 64-bit unsigned integers (32 bytes)
    memcpy(prng_seed, &output[key_size], seed_size);

    // Invert the byte order if on a big-endian system
    for (size_t i = 0; i < 4; i++)
    {
        prng_seed[i] = le64toh(prng_seed[i]);
    }

    // Initialize the PRNG
    prng_init(&context->shishua_state, prng_seed);
    prng_gen(&context->shishua_state, context->prng_buffer.buf, IMC_PRNG_BUFFER);
    
    // Release the unecessary memory and store the output
    sodium_munlock(prng_seed, sizeof(prng_seed));
    sodium_munlock(output, sizeof(output));
    *out = context;

    return IMC_SUCCESS;
}

// Pseudorandom number generator using the SHISHUA algorithm
// It writes a given amount of bytes to the output.
void imc_crypto_prng(CryptoContext *state, size_t num_bytes, uint8_t *output)
{
    for (size_t i = 0; i < num_bytes; i++)
    {
        // Fill the output with the generated bytes
        output[i] = state->prng_buffer.buf[state->prng_buffer.pos++];
        
        // Refill the PRNG buffer when we get to the end of it
        if (state->prng_buffer.pos == IMC_PRNG_BUFFER)
        {
            prng_gen(&state->shishua_state, state->prng_buffer.buf, IMC_PRNG_BUFFER);
            state->prng_buffer.pos = 0;
        }
    }
}

// Generate a pseudo-random unsigned 64-bit integer (from zero to its maximum possible value)
uint64_t imc_crypto_prng_uint64(CryptoContext *state)
{
    uint64_t random_num = 0;
    imc_crypto_prng(state, sizeof(uint64_t), (uint8_t *)&random_num);
    random_num = le64toh(random_num);   // Invert the byte order on big endian systems

    return random_num;
}

// Randomize the order of the elements in an array of pointers
void imc_crypto_shuffle_ptr(CryptoContext *state, uintptr_t *array, size_t num_elements, bool print_status)
{
    if (num_elements <= 1) return;
    
    // Fisher-Yates shuffle algorithm:
    // Each element 'E[i]' is swapped with a random element of index smaller or equal than 'i'.
    // Explanation of why not just swapping by any other element: https://blog.codinghorror.com/the-danger-of-naivete/
    for (size_t i = num_elements-1; i > 0; i--)
    {
        // A pseudorandom index smaller or equal than the current index
        size_t new_i = imc_crypto_prng_uint64(state) % i;
        if (new_i == i) continue;

        // Swap the current element with the element on the element on the random index
        array[i] ^= array[new_i];
        array[new_i] ^= array[i];
        array[i] ^= array[new_i];

        if (print_status && (i % 4096 == 0))
        {
            // Print the progress if we are on "verbose" mode
            // Note: For performance reasons, we are printing it once every 4096 steps.
            //       The compiler can optimize (i % 4096) to (i & 4095), because 4096 is a power of 2.
            const double percent = ((double)(num_elements - i) / (double)num_elements) * 100.0;
            printf("Shuffling carrier's read/write order... %.1f %%\r", percent);
        }
    }
    
    if (print_status)
    {
        printf("Shuffling carrier's read/write order... Done!  \n");
    }
}

// Encrypt a data stream
int imc_crypto_encrypt(
    CryptoContext *state,
    const uint8_t *const data,
    unsigned long long data_len,
    uint8_t *output,
    unsigned long long *output_len
)
{
    // Header used for decryption
    // (will be generated automatically, and needs to be stored with the encrypted stream)
    uint8_t crypto_header[crypto_secretstream_xchacha20poly1305_HEADERBYTES];
    memset(crypto_header, 0, sizeof(crypto_header));
    
    // Initialize the encryption
    crypto_secretstream_xchacha20poly1305_state encryption_state;
    int status = crypto_secretstream_xchacha20poly1305_init_push(
        &encryption_state,
        crypto_header,
        state->xcc20_key
    );

    if (status < 0) return status;

    // Encrypt the data
    status = crypto_secretstream_xchacha20poly1305_push(
        &encryption_state,  // Parameters for encryption
        &output[IMC_HEADER_OVERHEAD],   // Output buffer for the ciphertext (skip the space for the metadata that we are going to prepend)
        output_len,         // Stores the amount of bytes written to the output buffer
        data,               // Data to be encrypted
        data_len,           // Size in bytes of the data
        NULL,               // Additional data (nothing in our case)
        0,                  // Size in bytes of the additional data
        crypto_secretstream_xchacha20poly1305_TAG_FINAL // Tag that this is the last data of the stream
    );

    if (status < 0) return status;

    // Add the bytes needed by libsodium's header
    *output_len += crypto_secretstream_xchacha20poly1305_HEADERBYTES;

    // Pointers to the adresses where the version and size will be written
    uint32_t *version = (uint32_t *)&output[4];
    uint32_t *c_size = (uint32_t *)&output[8];
    
    // Write the metadata to the beginning of the buffer
    memcpy(&output[0], IMC_CRYPTO_MAGIC, 4);             // Add the file signature (magic bytes)
    *version = htole32( (uint32_t)IMC_CRYPTO_VERSION );  // Version of the current encryption process
    *c_size = htole32( (uint32_t)(*output_len) );        // Amount of bytes that follow until the end of the stream

    // Write the libsodium's header to before the encrypted stream
    uint8_t *header_dest = (uint8_t *)&output[12];
    memcpy(header_dest, crypto_header, crypto_secretstream_xchacha20poly1305_HEADERBYTES);

    // Add the bytes used by imgconceal
    *output_len += IMC_HEADER_OVERHEAD;

    return status;
}

// Decrypt a data stream
int imc_crypto_decrypt(
    CryptoContext *state,
    uint8_t header[crypto_secretstream_xchacha20poly1305_HEADERBYTES],
    const uint8_t *const data,
    unsigned long long data_len,
    uint8_t *output,
    unsigned long long *output_len
)
{
    // Initialize the decryption
    crypto_secretstream_xchacha20poly1305_state decryption_state;
    int status = crypto_secretstream_xchacha20poly1305_init_pull(
        &decryption_state,
        header,
        state->xcc20_key
    );

    if (status < 0) return status;

    unsigned char tag = 0;

    // Decrypt the data
    status = crypto_secretstream_xchacha20poly1305_pull(
        &decryption_state,  // Parameters for decryption
        output,             // Output buffer for the decrypted data
        output_len,         // Size in bytes of the output buffer (on success, the function stores here how many bytes were written)
        &tag,               // Output for the tag attached to the data (in the current version, it should be tagged as FINAL)
        data,               // Input buffer with the encrypted data
        data_len,           // Size in bytes of the input buffer
        NULL,               // Buffer for the additional data (we are not using it)
        0                   // Size of the buffer for additional data
    );

    if (status < 0)
    {
        // Decryption failed
        return status;
    }
    else
    {
        if (tag == crypto_secretstream_xchacha20poly1305_TAG_FINAL)
        {
            // Decryption worked
            return status;
        }
        else
        {
            // Theoretically, this branch is unreachable because (in this version) the encryption always tags the data as FINAL.
            // But the check for the tag is here "just in case".
            sodium_memzero(output, *output_len);
            return -1;
        }
    }
}

// Free the memory used by the cryptographic secrets
void imc_crypto_context_destroy(CryptoContext *state)
{
    sodium_free(state);
}