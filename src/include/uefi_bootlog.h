#ifndef VEXOS_UEFI_BOOTLOG_H
#define VEXOS_UEFI_BOOTLOG_H

#include <stdint.h>

#define VEXOS_BOOTLOG_MAGIC 0x56424C47u
#define VEXOS_BOOTLOG_VERSION 1u
#define VEXOS_BOOTLOG_MAX_PAYLOAD 2048u
#define VEXOS_BOOTLOG_VARIABLE_ATTRIBUTES 0x00000007u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t payload_size;
    uint32_t checksum;
} VexOSBootLogHeader;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t Data4[8];
} VexOSBootLogGuid;

static const uint16_t VEXOS_BOOTLOG_VARIABLE_NAME[] = {
    'V', 'e', 'x', 'O', 'S', 'B', 'o', 'o', 't', 'L', 'o', 'g', 0
};

static const VexOSBootLogGuid VEXOS_BOOTLOG_VENDOR_GUID = {
    0x7A2E6D3Bu,
    0x91F0u,
    0x4D52u,
    {0xA7u, 0x26u, 0x56u, 0x45u, 0x58u, 0x4Fu, 0x53u, 0x01u}
};

static inline uint32_t vexos_bootlog_checksum(const void *data,
                                               uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

#endif
