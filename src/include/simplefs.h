#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include <stdint.h>
#include <stddef.h>

int simplefs_init(void);
void simplefs_list(void);
int simplefs_write_file(const char *name, const void *data, uint32_t len);
int simplefs_read_file(const char *name, void *out, uint32_t maxlen,
                       uint32_t *out_len);
int simplefs_file_size(const char *name, uint32_t *out_size);

// UI/helpers
uint32_t simplefs_file_count(void);
int simplefs_file_name_at(uint32_t index, char *out_name, uint32_t out_cap);
uint32_t simplefs_list_to_buffer(char *out, uint32_t out_cap);

#endif
