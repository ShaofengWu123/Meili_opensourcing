#include <stdio.h>
#include <stdlib.h>

#include "../lib/log/meili_log.h"
#include "./str_helpers.h"


/* Read all or max_len bytes of file into buf, returning length in buf_len. */
int
util_load_file_to_buffer(const char *file, char **buf, uint64_t *buf_len, uint32_t max_len)
{
	uint64_t data_len;
	FILE *data_file;
	size_t read_len;
	long file_size;
	int ret;

	data_file = fopen(file, "r");
	if (!data_file) {
		MEILI_LOG_ERR("Failed to read file: %s.", file);
		return -ENOTSUP;
	}

	if (fseek(data_file, 0L, SEEK_END)) {
		MEILI_LOG_ERR("Read error on file: %s.", file);
		ret = -EINVAL;
		goto error_file;
	}

	file_size = ftell(data_file);
	if (file_size < 0) {
		MEILI_LOG_ERR("Text stream error from file: %s.", file);
		ret = -EINVAL;
		goto error_file;
	}

	if (max_len > file_size) {
		MEILI_LOG_ERR("Requested %u bytes but %lu detected in file %s.", max_len, file_size, file);
		ret = -EINVAL;
		goto error_file;
	}

	data_len = max_len ? max_len : file_size;
	*buf = rte_malloc(NULL, sizeof(char) * (data_len + 1), 4096);
	if (!*buf) {
		MEILI_LOG_ERR("Memory failure when loading file.");
		ret = -ENOMEM;
		goto error_file;
	}

	if (fseek(data_file, 0L, SEEK_SET)) {
		ret = -EINVAL;
		goto error_buf;
	}

	read_len = fread(*buf, sizeof(char), data_len, data_file);
	if (read_len != (unsigned long)data_len) {
		ret = -EINVAL;
		goto error_buf;
	}

	(*buf)[data_len] = '\0';

	fclose(data_file);
	*buf_len = data_len;

	return 0;

error_buf:
	rte_free(*buf);
error_file:
	fclose(data_file);

	return ret;
}

char *
util_trim_whitespace(char *input)
{
	char *end;

	while (isspace(*input))
		input++;

	end = input + strlen(input) - 1;

	while (end > input && isspace(*end))
		end -= 1;

	*(end + 1) = '\0';

	return input;
}

/* Verify string is a decimal of < given bytes and convert to long. */
int
util_str_to_dec(char *str, long *output, int max_bytes)
{
	size_t byte_len;
	long ret, mask;
	char *end_ptr;

	if (!str || !strlen(str) || !max_bytes || max_bytes > 8)
		return -EINVAL;

	errno = 0;
	ret = strtol(str, &end_ptr, 10);
	byte_len = end_ptr - str;

	/* Check strtol has not errored and that all str characters are used. */
	if (((ret == LONG_MAX || ret == LONG_MIN) && errno == ERANGE) || byte_len != strlen(str))
		return -EINVAL;

	/* Verify max bytes are not exceeded. */
	if (max_bytes < 8) {
		mask = (1L << (max_bytes * 8)) - 1;
		if (ret & ~mask)
			return -EINVAL;
	}

	*output = ret;

	return 0;
}