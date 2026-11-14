#include "png.h"
#include "heap.h"
#include <stdint.h>

static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
         (uint32_t)p[3];
}

static int mem_eq8(const uint8_t *a, const uint8_t *b, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (a[i] != b[i])
      return 0;
  }
  return 1;
}

typedef struct {
  const uint8_t *src;
  uint32_t src_len;
  uint32_t src_pos;
  uint32_t bitbuf;
  uint32_t bitcount;
} tinf_t;

static uint32_t tinf_get_bits(tinf_t *d, uint32_t n) {
  while (d->bitcount < n) {
    uint32_t b = 0;
    if (d->src_pos < d->src_len)
      b = d->src[d->src_pos++];
    d->bitbuf |= (b << d->bitcount);
    d->bitcount += 8;
  }
  uint32_t val = d->bitbuf & ((1u << n) - 1u);
  d->bitbuf >>= n;
  d->bitcount -= n;
  return val;
}

static void tinf_align_byte(tinf_t *d) {
  d->bitbuf = 0;
  d->bitcount = 0;
}

static int zlib_inflate_uncompressed(const uint8_t *z, uint32_t zlen,
                                    uint8_t **out, uint32_t *out_len) {
  if (!z || !out || !out_len || zlen < 7u)
    return 0;

  tinf_t st;
  st.src = z;
  st.src_len = zlen - 4u; /* Exclude the trailing Adler-32 checksum. */
  st.src_pos = 2u;       /* Skip the zlib header. */
  st.bitbuf = 0;
  st.bitcount = 0;

  uint32_t cap = 0;
  uint8_t *buf = NULL;
  uint32_t written = 0;

  for (;;) {
    if (st.src_pos >= st.src_len)
      goto fail;

    uint32_t bfinal = tinf_get_bits(&st, 1);
    uint32_t btype = tinf_get_bits(&st, 2);
    if (btype != 0u)
      goto fail; /* This small decoder currently supports stored DEFLATE blocks. */

    tinf_align_byte(&st);
    if (st.src_pos > st.src_len || st.src_len - st.src_pos < 4u)
      goto fail;

    uint32_t len = (uint32_t)st.src[st.src_pos] |
                   ((uint32_t)st.src[st.src_pos + 1u] << 8);
    uint32_t nlen = (uint32_t)st.src[st.src_pos + 2u] |
                    ((uint32_t)st.src[st.src_pos + 3u] << 8);
    st.src_pos += 4u;

    if ((len ^ 0xFFFFu) != nlen ||
        st.src_pos > st.src_len ||
        len > st.src_len - st.src_pos ||
        len > UINT32_MAX - written)
      goto fail;

    uint32_t needed = written + len;
    if (needed > cap) {
      uint32_t new_cap = cap ? cap : 4096u;
      while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2u) {
          new_cap = needed;
          break;
        }
        new_cap *= 2u;
      }

      uint8_t *next = (uint8_t *)kmalloc(new_cap);
      if (!next)
        goto fail;
      for (uint32_t i = 0; i < written; i++)
        next[i] = buf[i];
      if (buf)
        kfree(buf);
      buf = next;
      cap = new_cap;
    }

    for (uint32_t i = 0; i < len; i++)
      buf[written++] = st.src[st.src_pos++];

    if (bfinal)
      break;
  }

  *out = buf;
  *out_len = written;
  return 1;

fail:
  if (buf)
    kfree(buf);
  return 0;
}
static void unfilter_scanline(uint8_t *dst, const uint8_t *src, uint32_t len,
                              uint32_t bpp, const uint8_t *prev,
                              uint8_t filter) {
  if (filter == 0) {
    for (uint32_t i = 0; i < len; i++)
      dst[i] = src[i];
    return;
  }
  if (filter == 1) {
    for (uint32_t i = 0; i < len; i++) {
      uint8_t a = (i >= bpp) ? dst[i - bpp] : 0;
      dst[i] = (uint8_t)(src[i] + a);
    }
    return;
  }
  if (filter == 2) {
    for (uint32_t i = 0; i < len; i++) {
      uint8_t b = prev ? prev[i] : 0;
      dst[i] = (uint8_t)(src[i] + b);
    }
    return;
  }
  if (filter == 3) {
    for (uint32_t i = 0; i < len; i++) {
      uint8_t a = (i >= bpp) ? dst[i - bpp] : 0;
      uint8_t b = prev ? prev[i] : 0;
      uint8_t avg = (uint8_t)(((uint32_t)a + (uint32_t)b) / 2u);
      dst[i] = (uint8_t)(src[i] + avg);
    }
    return;
  }
  if (filter == 4) {
    for (uint32_t i = 0; i < len; i++) {
      int32_t a = (i >= bpp) ? dst[i - bpp] : 0;
      int32_t b = prev ? prev[i] : 0;
      int32_t c = (prev && i >= bpp) ? prev[i - bpp] : 0;
      int32_t p = a + b - c;
      int32_t pa = p - a;
      if (pa < 0)
        pa = -pa;
      int32_t pb = p - b;
      if (pb < 0)
        pb = -pb;
      int32_t pc = p - c;
      if (pc < 0)
        pc = -pc;
      int32_t pr = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      dst[i] = (uint8_t)(src[i] + (uint8_t)pr);
    }
    return;
  }
  for (uint32_t i = 0; i < len; i++)
    dst[i] = src[i];
}

int png_decode_rgba(const uint8_t *data, uint32_t size, uint32_t *out_w,
                    uint32_t *out_h, uint8_t **out_pixels) {
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  uint8_t *idat = NULL;
  uint8_t *raw = NULL;
  uint8_t *pixels = NULL;
  uint8_t *prev = NULL;
  uint8_t *cur = NULL;
  uint32_t idat_len = 0;
  uint32_t idat_cap = 0;
  uint32_t raw_len = 0;
  uint32_t w = 0;
  uint32_t h = 0;
  uint8_t bit_depth = 0;
  uint8_t color_type = 0;
  uint8_t interlace = 0;

  if (!data || size < 8u || !mem_eq8(data, sig, 8u))
    return 0;

  uint32_t pos = 8u;
  while (pos <= size && size - pos >= 12u) {
    uint32_t clen = rd_be32(&data[pos]);
    pos += 4u;
    const uint8_t *ctype = &data[pos];
    pos += 4u;

    if (clen > size - pos || size - pos - clen < 4u)
      goto fail;

    const uint8_t *cdata = &data[pos];
    if (ctype[0] == 'I' && ctype[1] == 'H' &&
        ctype[2] == 'D' && ctype[3] == 'R') {
      if (clen < 13u)
        goto fail;
      w = rd_be32(&cdata[0]);
      h = rd_be32(&cdata[4]);
      bit_depth = cdata[8];
      color_type = cdata[9];
      interlace = cdata[12];
    } else if (ctype[0] == 'I' && ctype[1] == 'D' &&
               ctype[2] == 'A' && ctype[3] == 'T') {
      if (clen > UINT32_MAX - idat_len)
        goto fail;
      uint32_t needed = idat_len + clen;
      if (needed > idat_cap) {
        uint32_t new_cap = idat_cap ? idat_cap : 4096u;
        while (new_cap < needed) {
          if (new_cap > UINT32_MAX / 2u) {
            new_cap = needed;
            break;
          }
          new_cap *= 2u;
        }
        uint8_t *next = (uint8_t *)kmalloc(new_cap);
        if (!next)
          goto fail;
        for (uint32_t i = 0; i < idat_len; i++)
          next[i] = idat[i];
        if (idat)
          kfree(idat);
        idat = next;
        idat_cap = new_cap;
      }
      for (uint32_t i = 0; i < clen; i++)
        idat[idat_len + i] = cdata[i];
      idat_len = needed;
    } else if (ctype[0] == 'I' && ctype[1] == 'E' &&
               ctype[2] == 'N' && ctype[3] == 'D') {
      pos += clen + 4u;
      break;
    }

    pos += clen + 4u; /* Chunk data + CRC. */
  }

  /* Bound decoded memory before calculating stride or allocating buffers. */
  if (w == 0u || h == 0u || w > 8192u || h > 8192u ||
      (uint64_t)w * (uint64_t)h > (2ull * 1024ull * 1024ull))
    goto fail;
  if (bit_depth != 8u || !(color_type == 2u || color_type == 6u) ||
      interlace != 0u || !idat || idat_len == 0u)
    goto fail;

  if (!zlib_inflate_uncompressed(idat, idat_len, &raw, &raw_len))
    goto fail;
  kfree(idat);
  idat = NULL;

  uint32_t bpp = (color_type == 6u) ? 4u : 3u;
  uint32_t stride = w * bpp;
  uint64_t expected = (uint64_t)h * (1ull + (uint64_t)stride);
  uint64_t pixel_bytes = (uint64_t)w * (uint64_t)h * 4ull;
  if (expected > UINT32_MAX || raw_len < (uint32_t)expected ||
      pixel_bytes > UINT32_MAX)
    goto fail;

  pixels = (uint8_t *)kmalloc((uint32_t)pixel_bytes);
  prev = (uint8_t *)kmalloc(stride);
  cur = (uint8_t *)kmalloc(stride);
  if (!pixels || !prev || !cur)
    goto fail;

  for (uint32_t i = 0; i < stride; i++)
    prev[i] = 0;

  for (uint32_t y = 0; y < h; y++) {
    uint8_t filter = raw[y * (1u + stride)];
    const uint8_t *src = &raw[y * (1u + stride) + 1u];
    if (filter > 4u)
      goto fail;

    unfilter_scanline(cur, src, stride, bpp, prev, filter);

    for (uint32_t x = 0; x < w; x++) {
      uint32_t si = x * bpp;
      uint32_t di = (y * w + x) * 4u;
      pixels[di + 0u] = cur[si + 0u];
      pixels[di + 1u] = cur[si + 1u];
      pixels[di + 2u] = cur[si + 2u];
      pixels[di + 3u] = (bpp == 4u) ? cur[si + 3u] : 255u;
    }

    uint8_t *tmp = prev;
    prev = cur;
    cur = tmp;
  }

  if (out_w)
    *out_w = w;
  if (out_h)
    *out_h = h;
  if (out_pixels)
    *out_pixels = pixels;
  else
    kfree(pixels);
  pixels = NULL;

  kfree(raw);
  raw = NULL;
  kfree(prev);
  prev = NULL;
  kfree(cur);
  cur = NULL;
  return 1;

fail:
  if (cur)
    kfree(cur);
  if (prev)
    kfree(prev);
  if (pixels)
    kfree(pixels);
  if (raw)
    kfree(raw);
  if (idat)
    kfree(idat);
  return 0;
}
