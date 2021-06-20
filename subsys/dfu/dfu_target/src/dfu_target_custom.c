/*
 * Copyright (c) 2021 > Innblue
 */

#if !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include <string.h>

#include <zephyr.h>
#include <pm_config.h>
#include <logging/log.h>
#include <nrfx.h>
#include <dfu/dfu_target_custom.h>
#include <dfu/dfu_target.h>
#include <dfu/dfu_target_stream.h>

LOG_MODULE_REGISTER(dfu_target_custom, CONFIG_DFU_TARGET_LOG_LEVEL);

#define MCUBOOT_SECONDARY_LAST_PAGE_ADDR                                       \
	(PM_MCUBOOT_SECONDARY_ADDRESS + PM_MCUBOOT_SECONDARY_SIZE - 1)

static struct dfu_target_custom_config target_cfg;
static uint8_t fota_buf[512];
static uint8_t *stream_buf = fota_buf;
static size_t stream_buf_len = sizeof(fota_buf);
static size_t filesize;

size_t dfu_target_custom_get_filesize(void)
{
	return filesize;
}

int dfu_target_custom_set_config(struct dfu_target_custom_config cfg)
{
	if (cfg.cb == NULL || cfg.file_max_size == 0 || cfg.flash_offset == 0 ||
	    strlen(cfg.flash_device_name) == 0)
		return -EINVAL;

	target_cfg.flash_device_name = cfg.flash_device_name;
	target_cfg.cb = cfg.cb;
	target_cfg.flash_offset = cfg.flash_offset;
	target_cfg.file_max_size = cfg.file_max_size;

	return 0;
}

bool dfu_target_custom_identify(const void *const buf)
{
	return target_cfg.cb(buf);
}

int dfu_target_custom_init(size_t file_size, dfu_target_callback_t cb)
{
	ARG_UNUSED(cb);
	const struct device *flash_dev;
	int err;

	if (target_cfg.cb == NULL || target_cfg.file_max_size == 0 || target_cfg.flash_offset == 0 ||
		strlen(target_cfg.flash_device_name) == 0) {
		LOG_ERR("Set configs first, dfu_target_custom_set_config()");
		return -EINVAL;
	}

	if (file_size > target_cfg.file_max_size) {
		LOG_ERR("Requested file too big to fit in flash %zu > 0x%x",
			file_size, target_cfg.file_max_size);
		return -EFBIG;
	}

	flash_dev = device_get_binding(target_cfg.flash_device_name);
	if (flash_dev == NULL) {
		LOG_ERR("Failed to get device '%s'",
			target_cfg.flash_device_name);
		return -EFAULT;
	}

	err = dfu_target_stream_init(&(struct dfu_target_stream_init){
		.id = "CUSTOM",
		.fdev = flash_dev,
		.buf = stream_buf,
		.len = stream_buf_len,
		.offset = target_cfg.flash_offset,
		.size = target_cfg.file_max_size,
		.cb = NULL });
	if (err < 0) {
		LOG_ERR("dfu_target_stream_init failed %d", err);
		return err;
	}

	filesize = file_size;

	return 0;
}

int dfu_target_custom_offset_get(size_t *out)
{
	return dfu_target_stream_offset_get(out);
}

int dfu_target_custom_write(const void *const buf, size_t len)
{
	return dfu_target_stream_write(buf, len);
}

int dfu_target_custom_done(bool successful)
{
	int err = 0;

	err = dfu_target_stream_done(successful);
	if (err != 0) {
		LOG_ERR("dfu_target_stream_done error %d", err);
		return err;
	}

	if (successful) {
		err = stream_flash_erase_page(
			dfu_target_stream_get_stream(),
			MCUBOOT_SECONDARY_LAST_PAGE_ADDR);
		if (err != 0) {
			LOG_ERR("Unable to delete last page: %d", err);
			return err;
		}
		LOG_INF("DFU custom target OK!");
	}
	return err;
}
