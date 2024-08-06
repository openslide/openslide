#ifndef OPENSLIDE_OPENSLIDE_DECODE_PBKDF2_H_
#define OPENSLIDE_OPENSLIDE_DECODE_PBKDF2_H_

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <openssl/evp.h>

#define SECRET_MAXSIZE 256
#define SALT_MAXSIZE 32
typedef struct tagRfc2898DeriveBytes {
  uint8_t pSecret[SECRET_MAXSIZE];
  uint8_t pSalt[SALT_MAXSIZE];
  size_t cbSecret;
  size_t cbSalt;
  size_t bytesAlreadyGotten;
} tRfc2898DeriveBytes;

tRfc2898DeriveBytes *_openslide_Rfc2898DeriveBytes_Init(const uint8_t *pSecret, uint32_t cbSecret, const uint8_t *pSalt, uint32_t cbSalt); // Returns a malloc'd structure. It is the responsibility of the caller to free this when no longer needed
uint8_t *_openslide_Rfc2898DeriveBytes_GetBytes(tRfc2898DeriveBytes *p, uint32_t byteCount); // Returns a malloc'd buffer. It is the responsibility of the caller to free this when no longer needed

#endif
