#ifndef OPENSLIDE_OPENSLIDE_DECODE_AES_H_
#define OPENSLIDE_OPENSLIDE_DECODE_AES_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  AES_CYPHER_128,
  AES_CYPHER_192,
  AES_CYPHER_256,
} AES_CYPHER_T;

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

uint8_t aes_sub_sbox(uint8_t val);
uint32_t aes_sub_dword(uint32_t val);
uint32_t aes_rot_dword(uint32_t val);
uint32_t aes_swap_dword(uint32_t val);
void aes_key_expansion(AES_CYPHER_T mode, uint8_t *key, uint8_t *round);
void aes_add_round_key(AES_CYPHER_T mode, uint8_t *state, uint8_t *round, int nr);
void aes_sub_bytes(AES_CYPHER_T mode, uint8_t *state);
void aes_shift_rows(AES_CYPHER_T mode, uint8_t *state);
uint8_t aes_xtime(uint8_t x);
uint8_t aes_xtimes(uint8_t x, int ts);
uint8_t aes_mul(uint8_t x, uint8_t y);
void aes_mix_columns(AES_CYPHER_T mode, uint8_t *state);
void inv_shift_rows(AES_CYPHER_T mode, uint8_t *state);
uint8_t inv_sub_sbox(uint8_t val);
void inv_sub_bytes(AES_CYPHER_T mode, uint8_t *state);
void inv_mix_columns(AES_CYPHER_T mode, uint8_t *state);

int _openslide_aes_decode_cbc(AES_CYPHER_T mode, uint8_t *data, int len, uint8_t *key, uint8_t *iv);

#ifdef __cplusplus
};
#endif

#endif
