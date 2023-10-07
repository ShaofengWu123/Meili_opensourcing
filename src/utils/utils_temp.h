/*
 * Copyright (c) 2021-2022 NVIDIA CORPORATION & AFFILIATES, ALL RIGHTS RESERVED.
 *
 * This software product is a proprietary product of NVIDIA CORPORATION &
 * AFFILIATES (the "Company") and all right, title, and interest in and to the
 * software product, including all associated intellectual property rights, are
 * and shall remain exclusively with the Company.
 *
 * This software product is governed by the End User License Agreement
 * provided with the software product.
 *
 */

#ifndef _INCLUDE_UTILS_TEMP_H_
#define _INCLUDE_UTILS_TEMP_H_

#include <stdint.h>
#include <string.h>
#include "./cJSON/cJSON.h"

#define WARNING_MARKER "\n******************************************************************\n"

#define MASK_UPPER_32  0xFFFFFFFF00000000LL
#define MASK_LOWER_32  0xFFFFFFFFLL

#define IS_NULL_OR_EMPTY_STRING(s) ((s) == NULL || strncmp(s, "", 1) == 0 ? 1 : 0)

int util_load_file_to_buffer(const char *file, char **buf, uint64_t *buf_len, uint32_t max_len);

char *util_trim_whitespace(char *input);

int util_str_to_dec(char *str, long *output, int max_bytes);

/* json file processing */
cJSON* until_parse_json_file(const char* filename);
int json_get_item_count(cJSON* config); 

static inline void
util_store_64_bit_as_2_32(uint32_t *dst, uint64_t val)
{
	dst[0] = (uint32_t)((val & MASK_UPPER_32) >> 32);
	dst[1] = (uint32_t)(val & MASK_LOWER_32);
}

static inline uint64_t
util_get_64_bit_from_2_32(uint32_t *src)
{

	return ((uint64_t)src[0]) << 32 | src[1];
}

#endif /* _INCLUDE_UTILS_TEMP_H_ */
