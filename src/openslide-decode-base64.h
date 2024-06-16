#ifndef OPENSLIDE_OPENSLIDE_DECODE_BASE64_H_
#define OPENSLIDE_OPENSLIDE_DECODE_BASE64_H_

#define BASE64_ENCODE_OUT_SIZE(s) ((unsigned int)((((s) + 2) / 3) * 4 + 1))
#define BASE64_DECODE_OUT_SIZE(s) ((unsigned int)(((s) / 4) * 3))

/*
 * return values is out length
 */
unsigned int
_openslide_base64_decode(const char *in, unsigned int inlen, unsigned char *out);

/*
 * RemoveCRLF
 */
char *
_openslide_remove_newline(char *in);


int _openslide_get_trim_length(char *in, int inlen);

#endif /* BASE64_H */
