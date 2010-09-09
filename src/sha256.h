#ifndef _SHA256_H_
#define _SHA256_h_

#include <sys/types.h>
#include <stdint.h>

typedef struct {
  /// Buffer to hold the final result and a temporary buffer for SHA256.
  union {
    uint8_t u8[64];
    uint32_t u32[16];
    uint64_t u64[8];
  } buffer;

  struct {
    uint32_t state[8];	// Internal state
    uint64_t size;	// Size of the message excluding padding
  } sha256;
} SHA256_CTX;

#define SHA256_DIGEST_LENGTH 32

void SHA256_Init(SHA256_CTX *ctx);
void SHA256_Update(SHA256_CTX *ctx, const uint8_t *buf, size_t size);
void SHA256_Final(unsigned char out[SHA256_DIGEST_LENGTH], SHA256_CTX *ctx);

#endif /* _SHA256_H_ */
