/*-
 * Copyright 2005 Colin Percival
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * FreeBSD's sys/crypto/sha2/sha256.h, from the FreeBSD src repository at
 * commit 975e3605ebb15cbaf5a25c1c9d1f51aed41291d0, adapted to fsimg:
 * - the context and functions take the fsimg_ prefix and the context the
 *   _t typedef (SHA256_CTX is fsimg_sha256_t, SHA256_Init is
 *   fsimg_sha256_init, and so on), and the length macros the FSIMG_ prefix;
 * - the libmd symbol renames, the __BEGIN_DECLS wrapper, the kernel
 *   conditionals and the declarations of libmd's SHA256_End, SHA256_Data,
 *   SHA256_Fd, SHA256_FdChunk, SHA256_File and SHA256_FileChunk, which are
 *   not imported, are removed;
 * - <stddef.h> and <stdint.h> replace <sys/types.h>.
 *
 * Streaming SHA-256 (FIPS 180-4): fsimg_sha256_init() starts a digest,
 * fsimg_sha256_update() absorbs any number of byte ranges of any length (an
 * empty range may pass NULL), and fsimg_sha256_final(digest, ctx), output
 * first as upstream has it, writes the 32-byte digest and clears the
 * context, which must be initialized again before reuse. None of them
 * allocates or fails. The context counts message bits in 64 bits, so a
 * message is at most 2^61 - 1 bytes, the FIPS 180-4 limit for SHA-256.
 */

#ifndef FSIMG_SHA256_H
#define FSIMG_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define FSIMG_SHA256_BLOCK_LENGTH		64
#define FSIMG_SHA256_DIGEST_LENGTH		32
#define FSIMG_SHA256_DIGEST_STRING_LENGTH	(FSIMG_SHA256_DIGEST_LENGTH * 2 + 1)

typedef struct fsimg_sha256 {
	uint32_t state[8];
	uint64_t count;
	uint8_t buf[FSIMG_SHA256_BLOCK_LENGTH];
} fsimg_sha256_t;

void	fsimg_sha256_init(fsimg_sha256_t *);
void	fsimg_sha256_update(fsimg_sha256_t *, const void *, size_t);
void	fsimg_sha256_final(unsigned char [static FSIMG_SHA256_DIGEST_LENGTH],
    fsimg_sha256_t *);

#endif /* !FSIMG_SHA256_H */
