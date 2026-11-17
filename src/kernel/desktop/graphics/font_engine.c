#include "font_engine.h"

#include <stddef.h>
#include <stdint.h>

#include "../../../include/heap.h"

/*
 * stb_truetype is a compact TrueType/OpenType parser and grayscale rasterizer.
 * VexOS supplies its own allocator, memory helpers, assertions, and math
 * functions so the engine remains usable in the freestanding kernel.
 */
static void *font_memcpy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; ++i)
        out[i] = in[i];
    return destination;
}

static void *font_memset(void *destination, int value, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < size; ++i)
        out[i] = (uint8_t)value;
    return destination;
}

static size_t font_strlen(const char *text) {
    size_t length = 0;
    if (text) {
        while (text[length])
            ++length;
    }
    return length;
}

static int font_floor_to_int(float value) {
    int result = (int)value;
    return ((float)result > value) ? result - 1 : result;
}

static int font_ceil_to_int(float value) {
    int result = (int)value;
    return ((float)result < value) ? result + 1 : result;
}

static float font_abs(float value) {
    return value < 0.0f ? -value : value;
}

static float font_sqrt(float value) {
    if (value <= 0.0f)
        return 0.0f;
    return __builtin_sqrtf(value);
}

/* stb_truetype uses pow only for cube roots in its optional SDF path. */
static float font_cuberoot(float value) {
    if (value == 0.0f)
        return 0.0f;
    float sign = value < 0.0f ? -1.0f : 1.0f;
    float x = font_abs(value);
    float guess = x > 1.0f ? x : 1.0f;
    for (uint32_t i = 0; i < 16u; ++i) {
        float square = guess * guess;
        if (square < 1.0e-20f)
            break;
        guess = (2.0f * guess + x / square) / 3.0f;
    }
    return sign * guess;
}

static float font_fmod(float value, float divisor) {
    if (divisor == 0.0f)
        return 0.0f;
    int quotient = (int)(value / divisor);
    return value - (float)quotient * divisor;
}

static float font_cos(float value) {
    const float two_pi = 6.2831853071795864769f;
    const float pi = 3.14159265358979323846f;
    while (value > pi) value -= two_pi;
    while (value < -pi) value += two_pi;

    float x2 = value * value;
    /* Minimax-like Taylor approximation over [-pi, pi]. */
    return 1.0f + x2 * (-0.5f + x2 * (0.041666638f +
           x2 * (-0.0013888378f + x2 * (0.0000247609f -
           x2 * 0.0000002605f))));
}

static float font_acos(float value) {
    const float half_pi = 1.57079632679489661923f;
    const float pi = 3.14159265358979323846f;
    if (value < -1.0f) value = -1.0f;
    if (value > 1.0f) value = 1.0f;
    float absolute = font_abs(value);
    float result = (((-0.0187293f * absolute + 0.0742610f) *
                     absolute - 0.2121144f) * absolute + 1.5707288f) *
                   font_sqrt(1.0f - absolute);
    return value < 0.0f ? pi - result : result;
}

#define STBTT_ifloor(x)       font_floor_to_int((float)(x))
#define STBTT_iceil(x)        font_ceil_to_int((float)(x))
#define STBTT_sqrt(x)         font_sqrt((float)(x))
#define STBTT_pow(x, y)       font_cuberoot((float)(x))
#define STBTT_fmod(x, y)      font_fmod((float)(x), (float)(y))
#define STBTT_cos(x)          font_cos((float)(x))
#define STBTT_acos(x)         font_acos((float)(x))
#define STBTT_fabs(x)         font_abs((float)(x))
#define STBTT_malloc(size, u) ((void)(u), kmalloc(size))
#define STBTT_free(ptr, u)    ((void)(u), kfree(ptr))
#define STBTT_assert(expr)    ((void)0)
#define STBTT_strlen(text)    font_strlen(text)
#define STBTT_memcpy          font_memcpy
#define STBTT_memset          font_memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

#define FONT_FAMILY_COUNT 4u
#define FONT_WEIGHT_COUNT 2u
#define FONT_CACHE_CAPACITY 256u
#define FONT_MIN_PIXEL_HEIGHT 6u
#define FONT_MAX_PIXEL_HEIGHT 96u
#define FONT_MAX_GLYPH_DIMENSION 128u

extern const uint8_t _binary_font_inter_regular_ttf_start[];
extern const uint8_t _binary_font_inter_regular_ttf_end[];
extern const uint8_t _binary_font_inter_bold_ttf_start[];
extern const uint8_t _binary_font_inter_bold_ttf_end[];
extern const uint8_t _binary_font_lato_regular_ttf_start[];
extern const uint8_t _binary_font_lato_regular_ttf_end[];
extern const uint8_t _binary_font_lato_bold_ttf_start[];
extern const uint8_t _binary_font_lato_bold_ttf_end[];
extern const uint8_t _binary_font_dejavu_sans_regular_ttf_start[];
extern const uint8_t _binary_font_dejavu_sans_regular_ttf_end[];
extern const uint8_t _binary_font_dejavu_sans_bold_ttf_start[];
extern const uint8_t _binary_font_dejavu_sans_bold_ttf_end[];
extern const uint8_t _binary_font_dejavu_mono_regular_ttf_start[];
extern const uint8_t _binary_font_dejavu_mono_regular_ttf_end[];
extern const uint8_t _binary_font_dejavu_mono_bold_ttf_start[];
extern const uint8_t _binary_font_dejavu_mono_bold_ttf_end[];

typedef struct {
    uint32_t codepoint;
    uint64_t last_used;
    uint16_t pixel_height;
    uint16_t width;
    uint16_t height;
    int16_t x_offset;
    int16_t y_offset;
    int16_t advance_x;
    uint8_t family;
    uint8_t bold;
    uint8_t valid;
    uint8_t *bitmap;
} FontCacheEntry;

static stbtt_fontinfo g_faces[FONT_FAMILY_COUNT][FONT_WEIGHT_COUNT];
static FontCacheEntry g_cache[FONT_CACHE_CAPACITY];
static uint64_t g_cache_clock;
static uint8_t g_engine_ready;

static const uint8_t *const g_font_data[FONT_FAMILY_COUNT][FONT_WEIGHT_COUNT] = {
    {
        _binary_font_inter_regular_ttf_start,
        _binary_font_inter_bold_ttf_start
    },
    {
        _binary_font_lato_regular_ttf_start,
        _binary_font_lato_bold_ttf_start
    },
    {
        _binary_font_dejavu_sans_regular_ttf_start,
        _binary_font_dejavu_sans_bold_ttf_start
    },
    {
        _binary_font_dejavu_mono_regular_ttf_start,
        _binary_font_dejavu_mono_bold_ttf_start
    }
};

static const uint8_t *const g_font_end[FONT_FAMILY_COUNT][FONT_WEIGHT_COUNT] = {
    {
        _binary_font_inter_regular_ttf_end,
        _binary_font_inter_bold_ttf_end
    },
    {
        _binary_font_lato_regular_ttf_end,
        _binary_font_lato_bold_ttf_end
    },
    {
        _binary_font_dejavu_sans_regular_ttf_end,
        _binary_font_dejavu_sans_bold_ttf_end
    },
    {
        _binary_font_dejavu_mono_regular_ttf_end,
        _binary_font_dejavu_mono_bold_ttf_end
    }
};

static uint64_t font_irq_lock(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void font_irq_unlock(uint64_t flags) {
    if (flags & (1ull << 9))
        __asm__ volatile("sti" ::: "memory");
}

static uint32_t font_clamp_family(uint8_t family) {
    return family < FONT_FAMILY_COUNT ? family : 0u;
}

static uint32_t font_clamp_bold(uint8_t bold) {
    return bold ? 1u : 0u;
}

static uint16_t font_clamp_height(uint16_t pixel_height) {
    if (pixel_height < FONT_MIN_PIXEL_HEIGHT)
        return FONT_MIN_PIXEL_HEIGHT;
    if (pixel_height > FONT_MAX_PIXEL_HEIGHT)
        return FONT_MAX_PIXEL_HEIGHT;
    return pixel_height;
}

static float font_scale_for_height(uint8_t family, uint8_t bold,
                                   uint16_t pixel_height) {
    stbtt_fontinfo *face = &g_faces[font_clamp_family(family)][font_clamp_bold(bold)];
    return stbtt_ScaleForPixelHeight(face, (float)font_clamp_height(pixel_height));
}

static uint32_t font_blend_rgb(uint32_t destination, uint32_t source,
                               uint8_t alpha) {
    uint32_t inverse = 255u - (uint32_t)alpha;
    uint32_t red = ((((destination >> 16) & 255u) * inverse) +
                    (((source >> 16) & 255u) * alpha)) / 255u;
    uint32_t green = ((((destination >> 8) & 255u) * inverse) +
                      (((source >> 8) & 255u) * alpha)) / 255u;
    uint32_t blue = (((destination & 255u) * inverse) +
                     ((source & 255u) * alpha)) / 255u;
    return (red << 16) | (green << 8) | blue;
}

int FontEngine_Init(void) {
    g_engine_ready = 0;
    for (uint32_t i = 0; i < FONT_CACHE_CAPACITY; ++i) {
        if (g_cache[i].bitmap)
            kfree(g_cache[i].bitmap);
        font_memset(&g_cache[i], 0, sizeof(g_cache[i]));
    }
    g_cache_clock = 0;

    for (uint32_t family = 0; family < FONT_FAMILY_COUNT; ++family) {
        for (uint32_t bold = 0; bold < FONT_WEIGHT_COUNT; ++bold) {
            const uint8_t *data = g_font_data[family][bold];
            const uint8_t *end = g_font_end[family][bold];
            if (!data || !end || end <= data)
                return 0;
            int offset = stbtt_GetFontOffsetForIndex(data, 0);
            if (offset < 0 || !stbtt_InitFont(&g_faces[family][bold],
                                               data, offset))
                return 0;
        }
    }

    g_engine_ready = 1;
    return 1;
}

int FontEngine_IsReady(void) {
    return g_engine_ready != 0;
}

void FontEngine_GetLineMetrics(uint8_t family, uint8_t bold,
                               uint16_t pixel_height,
                               int32_t *ascent, int32_t *descent,
                               int32_t *line_gap) {
    if (ascent) *ascent = 0;
    if (descent) *descent = 0;
    if (line_gap) *line_gap = 0;
    if (!g_engine_ready)
        return;

    family = (uint8_t)font_clamp_family(family);
    bold = (uint8_t)font_clamp_bold(bold);
    pixel_height = font_clamp_height(pixel_height);
    int raw_ascent = 0, raw_descent = 0, raw_gap = 0;
    stbtt_GetFontVMetrics(&g_faces[family][bold],
                          &raw_ascent, &raw_descent, &raw_gap);
    float scale = font_scale_for_height(family, bold, pixel_height);
    if (ascent) *ascent = (int32_t)(raw_ascent * scale + 0.5f);
    if (descent) *descent = (int32_t)(raw_descent * scale - 0.5f);
    if (line_gap) *line_gap = (int32_t)(raw_gap * scale + 0.5f);
}

int32_t FontEngine_GetKerning(uint8_t family, uint8_t bold,
                              uint16_t pixel_height,
                              uint32_t left_codepoint,
                              uint32_t right_codepoint) {
    if (!g_engine_ready || !left_codepoint || !right_codepoint)
        return 0;

    family = (uint8_t)font_clamp_family(family);
    bold = (uint8_t)font_clamp_bold(bold);
    pixel_height = font_clamp_height(pixel_height);
    int units = stbtt_GetCodepointKernAdvance(&g_faces[family][bold],
                                               (int)left_codepoint,
                                               (int)right_codepoint);
    float scale = font_scale_for_height(family, bold, pixel_height);
    float result = (float)units * scale;
    return result < 0.0f ? (int32_t)(result - 0.5f)
                         : (int32_t)(result + 0.5f);
}

static FontCacheEntry *font_cache_find(uint8_t family, uint8_t bold,
                                       uint16_t pixel_height,
                                       uint32_t codepoint) {
    for (uint32_t i = 0; i < FONT_CACHE_CAPACITY; ++i) {
        FontCacheEntry *entry = &g_cache[i];
        if (entry->valid && entry->family == family &&
            entry->bold == bold && entry->pixel_height == pixel_height &&
            entry->codepoint == codepoint)
            return entry;
    }
    return NULL;
}

static FontCacheEntry *font_cache_slot(void) {
    FontCacheEntry *oldest = &g_cache[0];
    for (uint32_t i = 0; i < FONT_CACHE_CAPACITY; ++i) {
        FontCacheEntry *entry = &g_cache[i];
        if (!entry->valid)
            return entry;
        if (entry->last_used < oldest->last_used)
            oldest = entry;
    }
    return oldest;
}

static FontCacheEntry *font_cache_create(uint8_t family, uint8_t bold,
                                         uint16_t pixel_height,
                                         uint32_t codepoint) {
    stbtt_fontinfo *face = &g_faces[family][bold];
    float scale = font_scale_for_height(family, bold, pixel_height);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int advance = 0, left_bearing = 0;

    stbtt_GetCodepointBitmapBox(face, scale, scale, (int)codepoint,
                                &x0, &y0, &x1, &y1);
    stbtt_GetCodepointHMetrics(face, (int)codepoint,
                               &advance, &left_bearing);

    int width = x1 - x0;
    int height = y1 - y0;
    if (width < 0 || height < 0 ||
        width > (int)FONT_MAX_GLYPH_DIMENSION ||
        height > (int)FONT_MAX_GLYPH_DIMENSION)
        return NULL;

    FontCacheEntry *entry = font_cache_slot();
    if (entry->bitmap)
        kfree(entry->bitmap);
    font_memset(entry, 0, sizeof(*entry));

    uint8_t *bitmap = NULL;
    if (width > 0 && height > 0) {
        size_t bytes = (size_t)width * (size_t)height;
        if (bytes / (size_t)width != (size_t)height)
            return NULL;
        bitmap = (uint8_t *)kmalloc(bytes);
        if (!bitmap)
            return NULL;
        font_memset(bitmap, 0, bytes);
        stbtt_MakeCodepointBitmap(face, bitmap, width, height, width,
                                  scale, scale, (int)codepoint);
    }

    float advance_pixels = (float)advance * scale;
    int32_t advance_x = advance_pixels < 0.0f
        ? (int32_t)(advance_pixels - 0.5f)
        : (int32_t)(advance_pixels + 0.5f);

    entry->codepoint = codepoint;
    entry->pixel_height = pixel_height;
    entry->width = (uint16_t)width;
    entry->height = (uint16_t)height;
    entry->x_offset = (int16_t)x0;
    entry->y_offset = (int16_t)y0;
    entry->advance_x = (int16_t)advance_x;
    entry->family = family;
    entry->bold = bold;
    entry->bitmap = bitmap;
    entry->valid = 1u;
    entry->last_used = ++g_cache_clock;
    return entry;
}

int32_t FontEngine_DrawGlyph(uint8_t family, uint8_t bold,
                             uint16_t pixel_height, uint32_t codepoint,
                             uint32_t *target, uint32_t pitch,
                             uint32_t screen_width, uint32_t screen_height,
                             int32_t pen_x, int32_t baseline_y,
                             uint32_t clip_x, uint32_t clip_y,
                             uint32_t clip_width, uint32_t clip_height,
                             uint32_t foreground,
                             FontEngineGlyphMetrics *metrics) {
    if (metrics)
        font_memset(metrics, 0, sizeof(*metrics));
    if (!g_engine_ready || codepoint > 0x10FFFFu)
        return 0;
    if (pitch < screen_width)
        return 0;

    family = (uint8_t)font_clamp_family(family);
    bold = (uint8_t)font_clamp_bold(bold);
    pixel_height = font_clamp_height(pixel_height);

    uint64_t irq_flags = font_irq_lock();
    FontCacheEntry *entry = font_cache_find(family, bold, pixel_height, codepoint);
    if (!entry)
        entry = font_cache_create(family, bold, pixel_height, codepoint);
    if (!entry) {
        font_irq_unlock(irq_flags);
        return 0;
    }
    entry->last_used = ++g_cache_clock;

    if (metrics) {
        metrics->advance_x = entry->advance_x;
        metrics->x_offset = entry->x_offset;
        metrics->y_offset = entry->y_offset;
        metrics->width = entry->width;
        metrics->height = entry->height;
    }

    if (target && entry->bitmap && entry->width && entry->height &&
        screen_width && screen_height && clip_width && clip_height) {
        int32_t glyph_left = pen_x + entry->x_offset;
        int32_t glyph_top = baseline_y + entry->y_offset;
        int32_t clip_left = clip_x > (uint32_t)INT32_MAX
            ? INT32_MAX : (int32_t)clip_x;
        int32_t clip_top = clip_y > (uint32_t)INT32_MAX
            ? INT32_MAX : (int32_t)clip_y;
        uint64_t clip_right_u = (uint64_t)clip_x + clip_width;
        uint64_t clip_bottom_u = (uint64_t)clip_y + clip_height;
        int32_t clip_right = clip_right_u > screen_width
            ? (int32_t)screen_width : (int32_t)clip_right_u;
        int32_t clip_bottom = clip_bottom_u > screen_height
            ? (int32_t)screen_height : (int32_t)clip_bottom_u;
        if (clip_left < 0) clip_left = 0;
        if (clip_top < 0) clip_top = 0;
        if (clip_right > (int32_t)screen_width)
            clip_right = (int32_t)screen_width;
        if (clip_bottom > (int32_t)screen_height)
            clip_bottom = (int32_t)screen_height;

        int32_t draw_left = glyph_left > clip_left ? glyph_left : clip_left;
        int32_t draw_top = glyph_top > clip_top ? glyph_top : clip_top;
        int32_t glyph_right = glyph_left + (int32_t)entry->width;
        int32_t glyph_bottom = glyph_top + (int32_t)entry->height;
        int32_t draw_right = glyph_right < clip_right ? glyph_right : clip_right;
        int32_t draw_bottom = glyph_bottom < clip_bottom ? glyph_bottom : clip_bottom;

        foreground &= 0x00FFFFFFu;
        for (int32_t y = draw_top; y < draw_bottom; ++y) {
            uint32_t source_y = (uint32_t)(y - glyph_top);
            uint32_t *row = &target[(uint32_t)y * pitch];
            const uint8_t *source_row =
                &entry->bitmap[source_y * entry->width];
            for (int32_t x = draw_left; x < draw_right; ++x) {
                uint32_t source_x = (uint32_t)(x - glyph_left);
                uint8_t alpha = source_row[source_x];
                if (alpha)
                    row[(uint32_t)x] =
                        font_blend_rgb(row[(uint32_t)x], foreground, alpha);
            }
        }
    }

    int32_t result = entry->advance_x;
    font_irq_unlock(irq_flags);
    return result;
}

int FontEngine_SelfTest(void) {
    if (!g_engine_ready)
        return 0;

    static uint32_t probe[64u * 64u];
    for (uint8_t family = 0; family < FONT_FAMILY_COUNT; ++family) {
        for (uint8_t bold = 0; bold < FONT_WEIGHT_COUNT; ++bold) {
            font_memset(probe, 0, sizeof(probe));
            int32_t ascent = 0, descent = 0, line_gap = 0;
            FontEngine_GetLineMetrics(family, bold, 24u,
                                      &ascent, &descent, &line_gap);
            if (ascent <= 0 || descent > 0)
                return 0;

            FontEngineGlyphMetrics metrics;
            int32_t advance = FontEngine_DrawGlyph(
                family, bold, 24u, (uint32_t)'A',
                probe, 64u, 64u, 64u,
                8, 40, 0, 0, 64u, 64u,
                0x00FFFFFFu, &metrics);
            if (advance <= 0 || !metrics.width || !metrics.height)
                return 0;

            uint32_t lit_pixels = 0;
            for (uint32_t i = 0; i < 64u * 64u; ++i) {
                if (probe[i] & 0x00FFFFFFu)
                    ++lit_pixels;
            }
            if (!lit_pixels)
                return 0;
        }
    }

    return 1;
}
