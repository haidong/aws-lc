/* Copyright (c) 2022, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/evp.h>

#include <openssl/bytestring.h>
#include <openssl/err.h>
#include <openssl/hkdf.h>
#include <openssl/kdf.h>
#include <openssl/mem.h>

#include "../internal.h"
#include "internal.h"
#include "../fipsmodule/evp/internal.h"


typedef struct {
  int mode;
  const EVP_MD *md;
  uint8_t *key;
  size_t key_len;
  uint8_t *salt;
  size_t salt_len;
  CBB info;
} HKDF_PKEY_CTX;

static int pkey_hkdf_init(EVP_PKEY_CTX *ctx) {
  HKDF_PKEY_CTX *hctx = OPENSSL_malloc(sizeof(HKDF_PKEY_CTX));
  if (hctx == NULL) {
    OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
    return 0;
  }

  OPENSSL_memset(hctx, 0, sizeof(HKDF_PKEY_CTX));
  if (!CBB_init(&hctx->info, 0)) {
    OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
    OPENSSL_free(hctx);
    return 0;
  }

  ctx->data = hctx;
  return 1;
}

static int pkey_hkdf_copy(EVP_PKEY_CTX *dst, EVP_PKEY_CTX *src) {
  if (!pkey_hkdf_init(dst)) {
    return 0;
  }

  HKDF_PKEY_CTX *hctx_dst = dst->data;
  const HKDF_PKEY_CTX *hctx_src = src->data;
  hctx_dst->mode = hctx_src->mode;
  hctx_dst->md = hctx_src->md;

  if (hctx_src->key_len != 0) {
    hctx_dst->key = OPENSSL_memdup(hctx_src->key, hctx_src->key_len);
    if (hctx_src->key == NULL) {
      OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
      return 0;
    }
    hctx_dst->key_len = hctx_src->key_len;
  }

  if (hctx_src->salt_len != 0) {
    hctx_dst->salt = OPENSSL_memdup(hctx_src->salt, hctx_src->salt_len);
    if (hctx_src->salt == NULL) {
      OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
      return 0;
    }
    hctx_dst->salt_len = hctx_src->salt_len;
  }

  if (!CBB_add_bytes(&hctx_dst->info, CBB_data(&hctx_src->info),
                     CBB_len(&hctx_src->info))) {
    OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
    return 0;
  }

  return 1;
}

static void pkey_hkdf_cleanup(EVP_PKEY_CTX *ctx) {
  HKDF_PKEY_CTX *hctx = ctx->data;
  if (hctx != NULL) {
    OPENSSL_free(hctx->key);
    OPENSSL_free(hctx->salt);
    CBB_cleanup(&hctx->info);
    OPENSSL_free(hctx);
    ctx->data = NULL;
  }
}

static int pkey_hkdf_derive(EVP_PKEY_CTX *ctx, uint8_t *out, size_t *out_len) {
  HKDF_PKEY_CTX *hctx = ctx->data;
  if (hctx->md == NULL) {
    OPENSSL_PUT_ERROR(EVP, EVP_R_MISSING_PARAMETERS);
    return 0;
  }
  if (hctx->key_len == 0) {
    OPENSSL_PUT_ERROR(EVP, EVP_R_NO_KEY_SET);
    return 0;
  }

  if (out == NULL) {
    if (hctx->mode == EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY) {
      *out_len = EVP_MD_size(hctx->md);
    }
    // HKDF-Expand is variable-length and returns |*out_len| bytes. "Output" the
    // input length by leaving it alone.
    return 1;
  }

  switch (hctx->mode) {
    case EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND:
      return HKDF(out, *out_len, hctx->md, hctx->key, hctx->key_len, hctx->salt,
                  hctx->salt_len, CBB_data(&hctx->info), CBB_len(&hctx->info));

    case EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY:
      if (*out_len < EVP_MD_size(hctx->md)) {
        OPENSSL_PUT_ERROR(EVP, EVP_R_BUFFER_TOO_SMALL);
        return 0;
      }
      return HKDF_extract(out, out_len, hctx->md, hctx->key, hctx->key_len,
                          hctx->salt, hctx->salt_len);

    case EVP_PKEY_HKDEF_MODE_EXPAND_ONLY:
      return HKDF_expand(out, *out_len, hctx->md, hctx->key, hctx->key_len,
                         CBB_data(&hctx->info), CBB_len(&hctx->info));
  }
  OPENSSL_PUT_ERROR(EVP, ERR_R_INTERNAL_ERROR);
  return 0;
}

static int pkey_hkdf_ctrl(EVP_PKEY_CTX *ctx, int type, int p1, void *p2) {
  HKDF_PKEY_CTX *hctx = ctx->data;
  switch (type) {
    case EVP_PKEY_CTRL_HKDF_MODE:
      if (p1 != EVP_PKEY_HKDEF_MODE_EXTRACT_AND_EXPAND &&
          p1 != EVP_PKEY_HKDEF_MODE_EXTRACT_ONLY &&
          p1 != EVP_PKEY_HKDEF_MODE_EXPAND_ONLY) {
        OPENSSL_PUT_ERROR(EVP, EVP_R_INVALID_OPERATION);
        return 0;
      }
      hctx->mode = p1;
      return 1;
    case EVP_PKEY_CTRL_HKDF_MD:
      hctx->md = p2;
      return 1;
    case EVP_PKEY_CTRL_HKDF_KEY: {
      const CBS *key = p2;
      if (!CBS_stow(key, &hctx->key, &hctx->key_len)) {
        OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
        return 0;
      }
      return 1;
    }
    case EVP_PKEY_CTRL_HKDF_SALT: {
      const CBS *salt = p2;
      if (!CBS_stow(salt, &hctx->salt, &hctx->salt_len)) {
        OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
        return 0;
      }
      return 1;
    }
    case EVP_PKEY_CTRL_HKDF_INFO: {
      const CBS *info = p2;
      // |EVP_PKEY_CTX_add1_hkdf_info| appends to the info string, rather than
      // replacing it.
      if (!CBB_add_bytes(&hctx->info, CBS_data(info), CBS_len(info))) {
        OPENSSL_PUT_ERROR(EVP, ERR_R_MALLOC_FAILURE);
        return 0;
      }
      return 1;
    }
    default:
      OPENSSL_PUT_ERROR(EVP, EVP_R_COMMAND_NOT_SUPPORTED);
      return 0;
  }
}

const EVP_PKEY_METHOD hkdf_pkey_meth = {
    EVP_PKEY_HKDF,
    pkey_hkdf_init,
    pkey_hkdf_copy,
    pkey_hkdf_cleanup,
    /*keygen=*/NULL,
    /*sign_init=*/NULL,
    /*sign=*/NULL,
    /*sign_message=*/NULL,
    /*verify_init=*/NULL,
    /*verify=*/NULL,
    /*verify_message=*/NULL,
    /*verify_recover=*/NULL,
    /*encrypt=*/NULL,
    /*decrypt=*/NULL,
    pkey_hkdf_derive,
    /*paramgen=*/NULL,
    pkey_hkdf_ctrl,
};

int EVP_PKEY_CTX_hkdf_mode(EVP_PKEY_CTX *ctx, int mode) {
  return EVP_PKEY_CTX_ctrl(ctx, EVP_PKEY_HKDF, EVP_PKEY_OP_DERIVE,
                           EVP_PKEY_CTRL_HKDF_MODE, mode, NULL);
}

int EVP_PKEY_CTX_set_hkdf_md(EVP_PKEY_CTX *ctx, const EVP_MD *md) {
  return EVP_PKEY_CTX_ctrl(ctx, EVP_PKEY_HKDF, EVP_PKEY_OP_DERIVE,
                           EVP_PKEY_CTRL_HKDF_MD, 0, (void *)md);
}

int EVP_PKEY_CTX_set1_hkdf_key(EVP_PKEY_CTX *ctx, const uint8_t *key,
                               size_t key_len) {
  CBS cbs;
  CBS_init(&cbs, key, key_len);
  return EVP_PKEY_CTX_ctrl(ctx, EVP_PKEY_HKDF, EVP_PKEY_OP_DERIVE,
                           EVP_PKEY_CTRL_HKDF_KEY, 0, &cbs);
}

int EVP_PKEY_CTX_set1_hkdf_salt(EVP_PKEY_CTX *ctx, const uint8_t *salt,
                                size_t salt_len) {
  CBS cbs;
  CBS_init(&cbs, salt, salt_len);
  return EVP_PKEY_CTX_ctrl(ctx, EVP_PKEY_HKDF, EVP_PKEY_OP_DERIVE,
                           EVP_PKEY_CTRL_HKDF_SALT, 0, &cbs);
}

int EVP_PKEY_CTX_add1_hkdf_info(EVP_PKEY_CTX *ctx, const uint8_t *info,
                                size_t info_len) {
  CBS cbs;
  CBS_init(&cbs, info, info_len);
  return EVP_PKEY_CTX_ctrl(ctx, EVP_PKEY_HKDF, EVP_PKEY_OP_DERIVE,
                           EVP_PKEY_CTRL_HKDF_INFO, 0, &cbs);
}
