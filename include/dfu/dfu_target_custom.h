/*
 * Copyright (c) 2021 > Innblue
 */

#ifndef DFU_TARGET_CUSTOM_H__
#define DFU_TARGET_CUSTOM_H__

#include <stddef.h>
#include <dfu/dfu_target.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef bool (*dfu_target_custom_secmat_verification_t)(const void *const buf);

struct dfu_target_custom_config {
	char *flash_device_name;
	uint32_t flash_offset;
	uint32_t file_max_size;
	dfu_target_custom_secmat_verification_t cb;
};

size_t dfu_target_custom_get_filesize(void);

int dfu_target_custom_set_config(struct dfu_target_custom_config cfg);

bool dfu_target_custom_identify(const void *const buf);

int dfu_target_custom_init(size_t file_size, dfu_target_callback_t cb);

int dfu_target_custom_offset_get(size_t *offset);

int dfu_target_custom_write(const void *const buf, size_t len);

int dfu_target_custom_done(bool successful);

#ifdef __cplusplus
}
#endif

#endif /* DFU_TARGET_CUSTOM_H__ */

/**@} */
