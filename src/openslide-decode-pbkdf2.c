
/*
 * This file is basically copied from https://github.com/lacchain/openssl-pqe-engine/tree/61d0fe530720f6b7e646db786c79f3db716133f3/ibrand_service/RFC2898DeriveBytes.c
 */

// References used...
//   https://stackoverflow.com/questions/55015935/equivalent-of-rfc2898derivebytes-in-c-without-using-clr
//   https://stackoverflow.com/questions/9771212/how-to-use-pkcs5-pbkdf2-hmac-sha1

#include "openslide-decode-pbkdf2.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#define PKCS5_PBKDF2_HMAC_ITERATIONS 2000 // Reference code
// #define PKCS5_PBKDF2_HMAC_ITERATIONS 317771 // A nice big prime number - just for fun, but a large impact on performance
// #define PKCS5_PBKDF2_HMAC_ITERATIONS 11113 // A smaller prime number - still just for fun.

tRfc2898DeriveBytes *_openslide_Rfc2898DeriveBytes_Init(const uint8_t *pSecret, uint32_t cbSecret, const uint8_t *pSalt, uint32_t cbSalt) {
  tRfc2898DeriveBytes *p;

  if (cbSecret > SECRET_MAXSIZE) {
    return NULL;
  }
  if (cbSalt > SALT_MAXSIZE) {
    return NULL;
  }
  p = malloc(sizeof(tRfc2898DeriveBytes));
  if (!p) {
    return NULL;
  }
  memcpy(p->pSecret, pSecret, cbSecret);
  p->cbSecret = cbSecret;
  if (pSalt == NULL) {
    uint8_t *pGeneratedSalt = malloc(cbSalt);
    if (!pGeneratedSalt) {
      free(p);
      return NULL;
    }
    for (size_t ii = 0; ii < cbSalt; ii++) {
      pGeneratedSalt[ii] = (uint8_t)rand(); // TODO - use IB randomness
    }
    memcpy(p->pSalt, pGeneratedSalt, cbSalt);
    p->cbSalt = cbSalt;
    free(pGeneratedSalt);
  } else {
    memcpy(p->pSalt, pSalt, cbSalt);
    p->cbSalt = cbSalt;
  }
  // Keep a record of how many bytes have been requested through getBytes
  // so that we don't return the same bytes twice.
  p->bytesAlreadyGotten = 0;

  return p; // Malloc'd structure. It is the responsibility of the caller to free this when no longer needed
}

uint8_t *_openslide_Rfc2898DeriveBytes_GetBytes(tRfc2898DeriveBytes *p, uint32_t byteCount) {
  // https://docs.microsoft.com/en-us/dotnet/api/system.security.cryptography.rfc2898derivebytes.getbytes?view=netcore-3.1
  // Says...
  // Repeated calls to this method will not generate the same key;
  // instead, appending two calls of the GetBytes method with a [byteCount] parameter value of 20 is
  // the equivalent of calling the GetBytes method once with a [byteCount] parameter value of 40.

  // This is was not true for this implementation, which was copied from the XQMsg CPP api
  // but is now fixed here with the addition of bytesAlreadyGotten and associated code.
  // TODO - this needs to be fixed in the CPP api

  int rc;

  if (!p) {
    return NULL;
  }

  // Allocate enough storage for all of the previously requested bytes, and a chunk of new bytes
  size_t bytesToGetFromPBKDF2 = p->bytesAlreadyGotten + byteCount;
  uint8_t *pBigBuffer = malloc(bytesToGetFromPBKDF2);
  if (pBigBuffer == NULL) {
    return NULL;
  }

  rc = PKCS5_PBKDF2_HMAC((const char *)p->pSecret,
                         (int)p->cbSecret,
                         p->pSalt,
                         (int)p->cbSalt,
                         PKCS5_PBKDF2_HMAC_ITERATIONS,
                         EVP_sha1(),
                         (int)bytesToGetFromPBKDF2, // Get all of the previous bytes, and a chunk of new bytes
                         pBigBuffer);
  if (rc != 1) {
    free(pBigBuffer);
    return NULL;
  }

  // Create a new buffer of the size requested, and copy in the trailing "new chunk" of data
  uint8_t *pResultBuffer = malloc(byteCount);
  if (pResultBuffer == NULL) {
    free(pBigBuffer);
    return NULL;
  }
  memcpy(pResultBuffer, pBigBuffer + p->bytesAlreadyGotten, byteCount);
  // Increase the number of bytes returned so that we don't return them again on a future call to GetBytes().
  p->bytesAlreadyGotten += byteCount;
  free(pBigBuffer);

  return pResultBuffer; // Malloc'd buffer. It is the responsibility of the caller to free this when no longer needed
}
