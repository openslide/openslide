/* This is a public domain base64 implementation written by WEI Zhicheng. */

#include "openslide-decode-base64.h"

#define BASE64_PAD '='
#define BASE64DE_FIRST '+'
#define BASE64DE_LAST 'z'

/* ASCII order for BASE 64 decode, 255 in unused character */
static const unsigned char base64de[] = {
	/* nul, soh, stx, etx, eot, enq, ack, bel, */
	255, 255, 255, 255, 255, 255, 255, 255,

	/*  bs,  ht,  nl,  vt,  np,  cr,  so,  si, */
	255, 255, 255, 255, 255, 255, 255, 255,

	/* dle, dc1, dc2, dc3, dc4, nak, syn, etb, */
	255, 255, 255, 255, 255, 255, 255, 255,

	/* can,  em, sub, esc,  fs,  gs,  rs,  us, */
	255, 255, 255, 255, 255, 255, 255, 255,

	/*  sp, '!', '"', '#', '$', '%', '&', ''', */
	255, 255, 255, 255, 255, 255, 255, 255,

	/* '(', ')', '*', '+', ',', '-', '.', '/', */
	255, 255, 255, 62, 255, 255, 255, 63,

	/* '0', '1', '2', '3', '4', '5', '6', '7', */
	52, 53, 54, 55, 56, 57, 58, 59,

	/* '8', '9', ':', ';', '<', '=', '>', '?', */
	60, 61, 255, 255, 255, 255, 255, 255,

	/* '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', */
	255, 0, 1, 2, 3, 4, 5, 6,

	/* 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', */
	7, 8, 9, 10, 11, 12, 13, 14,

	/* 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', */
	15, 16, 17, 18, 19, 20, 21, 22,

	/* 'X', 'Y', 'Z', '[', '\', ']', '^', '_', */
	23, 24, 25, 255, 255, 255, 255, 255,

	/* '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', */
	255, 26, 27, 28, 29, 30, 31, 32,

	/* 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', */
	33, 34, 35, 36, 37, 38, 39, 40,

	/* 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', */
	41, 42, 43, 44, 45, 46, 47, 48,

	/* 'x', 'y', 'z', '{', '|', '}', '~', del, */
	49, 50, 51, 255, 255, 255, 255, 255};

unsigned int
_openslide_base64_decode(const char *in, unsigned int inlen, unsigned char *out)
{
	unsigned int i;
	unsigned int j;
	unsigned char c;

	if (inlen & 0x3)
	{
		return 0;
	}

	for (i = j = 0; i < inlen; i++)
	{
		if (in[i] == BASE64_PAD)
		{
			break;
		}
		if (in[i] < BASE64DE_FIRST || in[i] > BASE64DE_LAST)
		{
			return 0;
		}

		c = base64de[(unsigned char)in[i]];
		if (c == 255)
		{
			return 0;
		}

		switch (i & 0x3)
		{
		case 0:
			out[j] = (c << 2) & 0xFF;
			break;
		case 1:
			out[j++] |= (c >> 4) & 0x3;
			out[j] = (c & 0xF) << 4;
			break;
		case 2:
			out[j++] |= (c >> 2) & 0xF;
			out[j] = (c & 0x3) << 6;
			break;
		case 3:
			out[j++] |= c;
			break;
		}
	}

	return j;
}

char *
_openslide_remove_newline(char *in)
{
	char *s = in;
	char *d = s;
	while ((*d = *s))
	{
		s++;
		if (*d == '\r' || *d == '\n')
			continue;
		d++;
	}
	*d = '\0';
	return in;
}

int _openslide_get_trim_length(char *in, int inlen)
{
	int outlen = 0;
	while (inlen > 0)
	{
		if (*in == 0)
			return outlen;
		outlen++;
		inlen--;
		in++;
	}
	return outlen;
}
