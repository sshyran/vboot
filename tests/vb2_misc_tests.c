/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for misc library
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2secdata.h"
#include "2sysincludes.h"
#include "test_common.h"

/* Common context for tests */
static uint8_t workbuf[VB2_FIRMWARE_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static uint8_t workbuf2[VB2_FIRMWARE_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_gbb_header gbb;
static struct vb2_secdata_fwmp *fwmp;
static enum vb2_boot_mode *boot_mode;

/* Mocked function data */
static enum vb2_resource_index mock_resource_index;
static void *mock_resource_ptr;
static uint32_t mock_resource_size;
static int mock_tpm_clear_called;
static int mock_tpm_clear_retval;

static void reset_common_data(void)
{
	memset(workbuf, 0xaa, sizeof(workbuf));
	memset(workbuf2, 0xbb, sizeof(workbuf2));

	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	sd = vb2_get_sd(ctx);
	sd->status |= VB2_SD_STATUS_SECDATA_FWMP_INIT;

	memset(&gbb, 0, sizeof(gbb));

	vb2_nv_init(ctx);

	vb2api_secdata_firmware_create(ctx);
	vb2_secdata_firmware_init(ctx);

	fwmp = (struct vb2_secdata_fwmp *)&ctx->secdata_fwmp;

	mock_tpm_clear_called = 0;
	mock_tpm_clear_retval = VB2_SUCCESS;

	boot_mode = (enum vb2_boot_mode *)&ctx->boot_mode;
	*boot_mode = VB2_BOOT_MODE_NORMAL;
};

/* Mocked functions */

struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

vb2_error_t vb2ex_read_resource(struct vb2_context *c,
				enum vb2_resource_index index, uint32_t offset,
				void *buf, uint32_t size)
{
	if (index != mock_resource_index)
		return VB2_ERROR_EX_READ_RESOURCE_INDEX;

	if (offset > mock_resource_size || offset + size > mock_resource_size)
		return VB2_ERROR_EX_READ_RESOURCE_SIZE;

	memcpy(buf, (uint8_t *)mock_resource_ptr + offset, size);
	return VB2_SUCCESS;
}

vb2_error_t vb2ex_tpm_clear_owner(struct vb2_context *c)
{
	mock_tpm_clear_called++;

	return mock_tpm_clear_retval;
}

/* Tests */
static void init_workbuf_tests(void)
{
	struct vb2_context *orig_ctx;

	/* check constants */
	TEST_TRUE(sizeof(struct vb2_context) < VB2_CONTEXT_MAX_SIZE,
		  "vb2_context max size constant");

	/* vb2api_init() - misaligned */
	TEST_EQ(vb2api_init(workbuf + 1, sizeof(workbuf) - 1, &ctx),
		VB2_ERROR_WORKBUF_ALIGN, "vb2api_init - misaligned");

	/* vb2api_init() - size too small */
	TEST_EQ(vb2api_init(workbuf, sizeof(struct vb2_shared_data) - 1,
			      &ctx), VB2_ERROR_WORKBUF_SMALL,
		"vb2api_init - size too small");

	/* vb2api_init() - success */
	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init - success");
	TEST_TRUE((uintptr_t)workbuf < (uintptr_t)ctx &&
		  (uintptr_t)ctx < (uintptr_t)workbuf + sizeof(workbuf),
		  "  return proper pointer");
	struct vb2_context zero_ctx = {0};
	TEST_SUCC(memcmp(ctx, &zero_ctx, sizeof(struct vb2_context)),
		  "  vb2_context set to zero");
	sd = vb2_get_sd(ctx);
	TEST_EQ(sd->magic, VB2_SHARED_DATA_MAGIC, "  set magic");
	TEST_EQ(sd->struct_version_major, VB2_SHARED_DATA_VERSION_MAJOR,
		"  set major version");
	TEST_EQ(sd->struct_version_minor, VB2_SHARED_DATA_VERSION_MINOR,
		"  set minor version");
	TEST_EQ(sd->workbuf_size, sizeof(workbuf), "  set workbuf size");
	TEST_TRUE(sd->workbuf_used - sizeof(struct vb2_shared_data)
		  < VB2_WORKBUF_ALIGN, "  set workbuf used");

	/* vb2api_relocate() - misaligned source */
	reset_common_data();
	memmove(workbuf + 1, workbuf, sizeof(workbuf) - 1);
	TEST_SUCC(vb2api_relocate(workbuf2, workbuf + 1, sizeof(workbuf) - 1,
				  &ctx), "vb2api_relocate - misaligned source");

	/* vb2api_relocate() - misaligned target */
	reset_common_data();
	TEST_EQ(vb2api_relocate(workbuf2 + 1, workbuf, sizeof(workbuf) - 1,
				&ctx),
		VB2_ERROR_WORKBUF_ALIGN, "vb2api_relocate - misaligned target");

	/* vb2api_relocate() - bad magic */
	reset_common_data();
	sd->magic = 0;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		VB2_ERROR_SHARED_DATA_MAGIC, "vb2api_relocate - bad magic");

	/* vb2api_relocate() - small major version */
	reset_common_data();
	sd->struct_version_major--;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		VB2_ERROR_SHARED_DATA_VERSION,
		"vb2api_relocate - small major version");

	/* vb2api_relocate() - big major version */
	reset_common_data();
	sd->struct_version_major++;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		VB2_ERROR_SHARED_DATA_VERSION,
		"vb2api_relocate - big major version");

	/* vb2api_relocate() - small minor version */
	if (VB2_SHARED_DATA_VERSION_MINOR > 0) {
		reset_common_data();
		sd->struct_version_minor--;
		TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf),
					&ctx),
			VB2_ERROR_SHARED_DATA_VERSION,
			  "vb2api_relocate - small minor version");
	}

	/* vb2api_relocate() - big minor version */
	reset_common_data();
	sd->struct_version_minor++;
	TEST_SUCC(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		  "vb2api_relocate - big minor version");

	/* vb2api_relocate() - small workbuf_used */
	reset_common_data();
	sd->workbuf_used = sizeof(struct vb2_shared_data) - 1;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		VB2_ERROR_WORKBUF_INVALID,
		"vb2api_relocate - small workbuf_used");

	/* vb2api_relocate() - workbuf_size < workbuf_used */
	reset_common_data();
	sd->workbuf_used = sd->workbuf_size;
	sd->workbuf_size--;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		VB2_ERROR_WORKBUF_INVALID,
		"vb2api_relocate - workbuf_size < workbuf_used");

	/* vb2api_relocate() - target workbuf too small */
	reset_common_data();
	sd->workbuf_used = sd->workbuf_size - 1;
	TEST_EQ(vb2api_relocate(workbuf2, workbuf, sd->workbuf_used - 1, &ctx),
		VB2_ERROR_WORKBUF_SMALL,
		"vb2api_relocate - target workbuf too small");

	/* vb2api_relocate() - success (same size) */
	reset_common_data();
	orig_ctx = ctx;
	TEST_SUCC(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		  "vb2api_relocate - success (same size)");
	sd = vb2_get_sd(ctx);
	TEST_EQ((uintptr_t)orig_ctx - (uintptr_t)workbuf,
		(uintptr_t)ctx - (uintptr_t)workbuf2,
		"  same context pointer");
	TEST_SUCC(memcmp(workbuf2, workbuf, sd->workbuf_used),
		  "  same workbuf");

	/* vb2api_relocate() - success (smaller size) */
	reset_common_data();
	TEST_SUCC(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf) - 1, &ctx),
		  "vb2api_relocate - success (smaller size)");
	sd = vb2_get_sd(ctx);
	TEST_EQ(sd->workbuf_size, sizeof(workbuf) - 1, "  set workbuf size");

	/* vb2api_relocate() - success (larger size) */
	reset_common_data();
	sd->workbuf_size--;
	TEST_SUCC(vb2api_relocate(workbuf2, workbuf, sizeof(workbuf), &ctx),
		  "vb2api_relocate - success (larger size)");
	sd = vb2_get_sd(ctx);
	TEST_EQ(sd->workbuf_size, sizeof(workbuf), "  set workbuf size");

	/* vb2api_relocate() - success (overlapping) */
	reset_common_data();
	orig_ctx = ctx;
	sd->workbuf_size -= VB2_WORKBUF_ALIGN;
	memcpy(workbuf2, workbuf, sd->workbuf_used);
	TEST_SUCC(vb2api_relocate(workbuf + VB2_WORKBUF_ALIGN, workbuf,
				  sizeof(workbuf) - VB2_WORKBUF_ALIGN, &ctx),
		  "vb2api_relocate - success (overlapping)");
	sd = vb2_get_sd(ctx);
	TEST_EQ((uintptr_t)ctx - (uintptr_t)orig_ctx,
		VB2_WORKBUF_ALIGN,
		"  context pointer moved");
	TEST_SUCC(memcmp(workbuf2, workbuf + VB2_WORKBUF_ALIGN,
			 sd->workbuf_used), "  same workbuf");

	/* vb2api_reinit() - workbuf_size < workbuf_used */
	reset_common_data();
	sd->workbuf_size = sd->workbuf_used - 1;
	TEST_EQ(vb2api_reinit(workbuf, &ctx), VB2_ERROR_WORKBUF_INVALID,
		"vb2api_reinit - workbuf_size < workbuf_used");

	/* vb2api_reinit() - success */
	reset_common_data();
	orig_ctx = ctx;
	TEST_SUCC(vb2api_reinit(workbuf, &ctx),
		  "vb2api_reinit - success");
	TEST_EQ((uintptr_t)ctx, (uintptr_t)orig_ctx,
		"  context pointer unchanged");
}

static void misc_tests(void)
{
	struct vb2_workbuf wb;

	reset_common_data();
	sd->workbuf_used = VB2_WORKBUF_ALIGN;

	vb2_workbuf_from_ctx(ctx, &wb);

	TEST_PTR_EQ(wb.buf, workbuf + VB2_WORKBUF_ALIGN,
		    "vb_workbuf_from_ctx() buf");
	TEST_EQ(wb.size, sd->workbuf_size - VB2_WORKBUF_ALIGN,
		"vb_workbuf_from_ctx() size");

	reset_common_data();
	sd->status |= VB2_SD_STATUS_RECOVERY_DECIDED;
	TEST_ABORT(VB2_REC_OR_DIE(ctx, "die\n"), "REC_OR_DIE in normal mode");

	reset_common_data();
	sd->status |= VB2_SD_STATUS_RECOVERY_DECIDED;
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	VB2_REC_OR_DIE(ctx, "VB2_REC_OR_DIE() test in recovery mode\n");
	/* Would exit here if it didn't work as intended. */

	reset_common_data();
	VB2_REC_OR_DIE(ctx, "VB2_REC_OR_DIE() test in fw_phase1\n");
}

static void gbb_tests(void)
{
	struct vb2_gbb_header gbbsrc = {
		.signature = {'$', 'G', 'B', 'B'},
		.major_version = VB2_GBB_MAJOR_VER,
		.minor_version = VB2_GBB_MINOR_VER,
		.header_size = sizeof(struct vb2_gbb_header),
		.flags = 0x1234,
		.rootkey_offset = 240,
		.rootkey_size = 1040,
	};

	struct vb2_gbb_header gbbdest;

	TEST_EQ(sizeof(struct vb2_gbb_header),
		EXPECTED_VB2_GBB_HEADER_SIZE,
		"sizeof(struct vb2_gbb_header)");

	reset_common_data();

	/* Good contents */
	mock_resource_index = VB2_RES_GBB;
	mock_resource_ptr = &gbbsrc;
	mock_resource_size = sizeof(gbbsrc);
	TEST_SUCC(vb2_read_gbb_header(ctx, &gbbdest), "read gbb header good");
	TEST_SUCC(memcmp(&gbbsrc, &gbbdest, sizeof(gbbsrc)),
		  "read gbb contents");

	mock_resource_index = VB2_RES_GBB + 1;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_EX_READ_RESOURCE_INDEX, "read gbb header missing");
	mock_resource_index = VB2_RES_GBB;

	gbbsrc.signature[0]++;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_GBB_MAGIC, "read gbb header bad magic");
	gbbsrc.signature[0]--;

	gbbsrc.major_version = VB2_GBB_MAJOR_VER + 1;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_GBB_VERSION, "read gbb header major version");
	gbbsrc.major_version = VB2_GBB_MAJOR_VER;

	gbbsrc.minor_version = VB2_GBB_MINOR_VER + 1;
	TEST_SUCC(vb2_read_gbb_header(ctx, &gbbdest),
		  "read gbb header minor++");
	gbbsrc.minor_version = 1;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_GBB_TOO_OLD, "read gbb header 1.1 fails");
	gbbsrc.minor_version = 0;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_GBB_TOO_OLD, "read gbb header 1.0 fails");
	gbbsrc.minor_version = VB2_GBB_MINOR_VER;

	gbbsrc.header_size--;
	TEST_EQ(vb2_read_gbb_header(ctx, &gbbdest),
		VB2_ERROR_GBB_HEADER_SIZE, "read gbb header size");
	TEST_EQ(vb2_fw_init_gbb(ctx),
		VB2_ERROR_GBB_HEADER_SIZE, "init gbb failure");
	gbbsrc.header_size++;

	/* Init GBB */
	int used_before = sd->workbuf_used;
	TEST_SUCC(vb2_fw_init_gbb(ctx), "init gbb");
	/* Manually calculate the location of GBB since we have mocked out the
	   original definition of vb2_get_gbb. */
	struct vb2_gbb_header *current_gbb = vb2_member_of(sd, sd->gbb_offset);
	TEST_SUCC(memcmp(&gbbsrc, current_gbb, sizeof(gbbsrc)),
		  "  copy gbb contents");
	TEST_TRUE(sd->workbuf_used - sizeof(gbbsrc) - used_before
		  < VB2_WORKBUF_ALIGN, "  unexpected workbuf size");

	/* Workbuf failure */
	reset_common_data();
	sd->workbuf_used = sd->workbuf_size - 4;
	TEST_EQ(vb2_fw_init_gbb(ctx),
		VB2_ERROR_GBB_WORKBUF, "init gbb no workbuf");

	/* Check for setting NO_SECDATA_FWMP context flag */
	reset_common_data();
	TEST_SUCC(vb2_fw_init_gbb(ctx), "init gbb");
	TEST_EQ(ctx->flags & VB2_CONTEXT_NO_SECDATA_FWMP, 0,
		"without DISABLE_FWMP: NO_SECDATA_FWMP shouldn't be set");
	reset_common_data();
	gbbsrc.flags |= VB2_GBB_FLAG_DISABLE_FWMP;
	TEST_SUCC(vb2_fw_init_gbb(ctx), "init gbb");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_NO_SECDATA_FWMP, 0,
		 "with DISABLE_FWMP: NO_SECDATA_FWMP should be set");
}

static void fail_tests(void)
{
	/* Early fail (before even NV init) */
	reset_common_data();
	sd->status &= ~VB2_SD_STATUS_NV_INIT;
	vb2api_fail(ctx, 1, 2);
	TEST_NEQ(sd->status & VB2_SD_STATUS_NV_INIT, 0, "vb2api_fail inits NV");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		1, "vb2api_fail request");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE),
		2, "vb2api_fail subcode");

	/* Repeated fail doesn't overwrite the error code */
	vb2api_fail(ctx, 3, 4);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		1, "vb2api_fail repeat");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE),
		2, "vb2api_fail repeat2");

	/* Fail with other slot good doesn't trigger recovery */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_TRY_COUNT, 3);
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_UNKNOWN);
	sd->status |= VB2_SD_STATUS_CHOSE_SLOT;
	sd->fw_slot = 0;
	sd->last_fw_slot = 1;
	sd->last_fw_result = VB2_FW_RESULT_UNKNOWN;
	vb2api_fail(ctx, 5, 6);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST), 0, "vb2_failover");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_FAILURE, "vb2api_fail this fw");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_COUNT), 0,
		"vb2api_fail use up tries");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_NEXT), 1,
		"vb2api_fail try other slot");

	/* Fail with other slot already failing triggers recovery */
	reset_common_data();
	sd->status |= VB2_SD_STATUS_CHOSE_SLOT;
	sd->fw_slot = 1;
	sd->last_fw_slot = 0;
	sd->last_fw_result = VB2_FW_RESULT_FAILURE;
	vb2api_fail(ctx, 7, 8);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST), 7,
		"vb2api_fail both slots bad");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_FAILURE, "vb2api_fail this fw");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_NEXT), 0,
		"vb2api_fail try other slot");
}

static void recovery_tests(void)
{
	/* No recovery */
	reset_common_data();
	TEST_EQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		0, "recovery not yet decided before testing check_recovery()");
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, 0, "No recovery reason");
	TEST_EQ(ctx->flags & VB2_CONTEXT_RECOVERY_MODE,
		0, "Not recovery mode");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");

	/* From request */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, 3);
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, 3, "Recovery reason from request");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST), 3, "NV not cleared");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_RECOVERY_MODE,
		 0, "Recovery mode");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");

	/* From request, but already failed */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, 4);
	sd->recovery_reason = 5;
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, 5, "Recovery reason already failed");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		4, "NV not cleared");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");

	/* Override */
	reset_common_data();
	sd->recovery_reason = 6;
	ctx->flags |= VB2_CONTEXT_FORCE_RECOVERY_MODE;
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, VB2_RECOVERY_RO_MANUAL,
		"Recovery reason forced");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");

	/* Override subcode TRAIN_AND_REBOOT */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_RECOVERY_SUBCODE, VB2_RECOVERY_TRAIN_AND_REBOOT);
	ctx->flags |= VB2_CONTEXT_FORCE_RECOVERY_MODE;
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, VB2_RECOVERY_RO_MANUAL,
		"Recovery reason forced");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");

	/* Promote subcode from BROKEN screen*/
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_RECOVERY_SUBCODE, VB2_RECOVERY_US_TEST);
	ctx->flags |= VB2_CONTEXT_FORCE_RECOVERY_MODE;
	vb2_check_recovery(ctx);
	TEST_EQ(sd->recovery_reason, VB2_RECOVERY_US_TEST,
		"Recovery reason forced from BROKEN");
	TEST_NEQ(sd->status & VB2_SD_STATUS_RECOVERY_DECIDED,
		 0, "Recovery decided");
}

static void dev_switch_tests(void)
{
	uint32_t v;

	/* Normal mode */
	reset_common_data();
	TEST_SUCC(vb2_check_dev_switch(ctx), "dev mode off");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");
	TEST_EQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx not in dev");
	TEST_EQ(mock_tpm_clear_called, 0, "  no tpm clear");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_REQ_WIPEOUT), 0, "  no nv wipeout");

	/* Dev mode */
	reset_common_data();
	vb2_secdata_firmware_set(
		ctx, VB2_SECDATA_FIRMWARE_FLAGS,
		(VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE |
		 VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER));
	TEST_SUCC(vb2_check_dev_switch(ctx), "dev mode on");
	TEST_NEQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd in dev");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx in dev");
	TEST_EQ(mock_tpm_clear_called, 0, "  no tpm clear");

	/* Any normal mode boot clears dev boot flags */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_EXTERNAL, 1);
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_ALTFW, 1);
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 1);
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT, 1);
	TEST_SUCC(vb2_check_dev_switch(ctx), "dev mode off");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DEV_BOOT_EXTERNAL),
		0, "  cleared dev boot external");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DEV_BOOT_ALTFW),
		0, "  cleared dev boot altfw");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY),
		0, "  cleared dev boot signed only");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DEV_DEFAULT_BOOT),
		0, "  cleared dev default boot");

	/* Normal-dev transition clears TPM */
	reset_common_data();
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS,
				 VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE);
	TEST_SUCC(vb2_check_dev_switch(ctx), "to dev mode");
	TEST_EQ(mock_tpm_clear_called, 1, "  tpm clear");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	TEST_EQ(v, (VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE |
		    VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER),
		"  last boot developer now");

	/* Dev-normal transition clears TPM too */
	reset_common_data();
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS,
				 VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER);
	TEST_SUCC(vb2_check_dev_switch(ctx), "from dev mode");
	TEST_EQ(mock_tpm_clear_called, 1, "  tpm clear");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	TEST_EQ(v, 0, "  last boot not developer now");

	/* Disable dev mode */
	reset_common_data();
	vb2_secdata_firmware_set(
		ctx, VB2_SECDATA_FIRMWARE_FLAGS,
		(VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE |
		 VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER));
	vb2_nv_set(ctx, VB2_NV_DISABLE_DEV_REQUEST, 1);
	TEST_SUCC(vb2_check_dev_switch(ctx), "disable dev request");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISABLE_DEV_REQUEST),
		0, "  request cleared");

	/* Force enabled by GBB */
	reset_common_data();
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_SWITCH_ON;
	TEST_SUCC(vb2_check_dev_switch(ctx), "dev on via gbb");
	TEST_NEQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd in dev");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	TEST_EQ(v, VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER,
		"  doesn't set dev on in secdata_firmware "
		"but does set last boot dev");
	TEST_EQ(mock_tpm_clear_called, 1, "  tpm clear");

	/* Request disable by ctx flag */
	reset_common_data();
	vb2_secdata_firmware_set(
		ctx, VB2_SECDATA_FIRMWARE_FLAGS,
		(VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE |
		 VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER));
	ctx->flags |= VB2_CONTEXT_DISABLE_DEVELOPER_MODE;
	TEST_SUCC(vb2_check_dev_switch(ctx), "disable dev on ctx request");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");

	/* Simulate clear owner failure */
	reset_common_data();
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS,
				 VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER);
	mock_tpm_clear_retval = VB2_ERROR_EX_TPM_CLEAR_OWNER;
	TEST_EQ(vb2_check_dev_switch(ctx),
		VB2_ERROR_EX_TPM_CLEAR_OWNER, "tpm clear fail");
	TEST_EQ(mock_tpm_clear_called, 1, "  tpm clear");
	v = vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS);
	TEST_EQ(v, VB2_SECDATA_FIRMWARE_FLAG_LAST_BOOT_DEVELOPER,
		"  last boot still developer");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		VB2_RECOVERY_TPM_CLEAR_OWNER, "  requests recovery");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE),
		(uint8_t)VB2_ERROR_EX_TPM_CLEAR_OWNER, "  recovery subcode");

	/*
	 * secdata_firmware failure in normal mode fails and shows dev=0 even
	 * if dev mode was on in the (inaccessible) secdata_firmware. Since this
	 * happens in fw_phase1, we do not abort -- we know that when secdata
	 * is uninitialized here, we must be headed for recovery mode.
	 */
	reset_common_data();
	vb2_secdata_firmware_set(ctx, VB2_SECDATA_FIRMWARE_FLAGS,
				 VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE);
	sd->status &= ~VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	TEST_SUCC(vb2_check_dev_switch(ctx), "secdata_firmware fail normal");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");
	TEST_EQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx not in dev");

	/* secdata_firmware failure in recovery mode continues */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	sd->status &= ~VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	TEST_SUCC(vb2_check_dev_switch(ctx), "secdata_firmware fail recovery");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");
	TEST_EQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx not in dev");

	/* And doesn't check or clear dev disable request */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	sd->status &= ~VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	vb2_nv_set(ctx, VB2_NV_DISABLE_DEV_REQUEST, 1);
	TEST_SUCC(vb2_check_dev_switch(ctx),
		  "secdata_firmware fail recovery disable");
	TEST_EQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd not in dev");
	TEST_EQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx not in dev");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISABLE_DEV_REQUEST),
		1, "  request not cleared");

	/* Can still override with GBB flag */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	sd->status &= ~VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_SWITCH_ON;
	TEST_SUCC(vb2_check_dev_switch(ctx),
		  "secdata_firmware fail recovery gbb");
	TEST_NEQ(sd->flags & VB2_SD_FLAG_DEV_MODE_ENABLED, 0, "  sd in dev");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_DEVELOPER_MODE, 0, "  ctx in dev");
	TEST_EQ(mock_tpm_clear_called, 1, "  tpm clear");

	/* Force wipeout by ctx flag */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_FORCE_WIPEOUT_MODE;
	TEST_SUCC(vb2_check_dev_switch(ctx), "wipeout on ctx flag");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_REQ_WIPEOUT), 1, "  nv wipeout");
}

static void enable_dev_tests(void)
{
	reset_common_data();
	TEST_FAIL(vb2api_enable_developer_mode(ctx),
		 "vb2api_enable_developer_mode - failed");
	TEST_EQ(vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS) &
		VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE, 0,
		"  dev mode flag not set");

	reset_common_data();
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	TEST_SUCC(vb2api_enable_developer_mode(ctx),
		  "vb2api_enable_developer_mode - success");
	TEST_NEQ(vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS) &
	         VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE, 0,
		 "  dev mode flag set");

	/* secdata_firmware not initialized, aborts */
	reset_common_data();
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	sd->status &= ~VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	sd->status |= VB2_SD_STATUS_RECOVERY_DECIDED;
	TEST_ABORT(vb2api_enable_developer_mode(ctx),
		   "secdata_firmware no init, enable dev mode aborted");
	sd->status |= VB2_SD_STATUS_SECDATA_FIRMWARE_INIT;
	TEST_EQ(vb2_secdata_firmware_get(ctx, VB2_SECDATA_FIRMWARE_FLAGS) &
	        VB2_SECDATA_FIRMWARE_FLAG_DEV_MODE, 0,
		"  dev mode flag not set");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DEV_BOOT_EXTERNAL), 0,
		"  NV_DEV_BOOT_EXTERNAL not set");
}

static void tpm_clear_tests(void)
{
	/* No clear request */
	reset_common_data();
	TEST_SUCC(vb2_check_tpm_clear(ctx), "no clear request");
	TEST_EQ(mock_tpm_clear_called, 0, "tpm not cleared");

	/* Successful request */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST, 1);
	TEST_SUCC(vb2_check_tpm_clear(ctx), "clear request");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST),
		0, "request cleared");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_CLEAR_TPM_OWNER_DONE),
		1, "done set");
	TEST_EQ(mock_tpm_clear_called, 1, "tpm cleared");

	/* Failed request */
	reset_common_data();
	mock_tpm_clear_retval = VB2_ERROR_EX_TPM_CLEAR_OWNER;
	vb2_nv_set(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST, 1);
	TEST_EQ(vb2_check_tpm_clear(ctx),
		VB2_ERROR_EX_TPM_CLEAR_OWNER, "clear failure");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_CLEAR_TPM_OWNER_REQUEST),
		0, "request cleared");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_CLEAR_TPM_OWNER_DONE),
		0, "done not set");
}

static void select_slot_tests(void)
{
	/* Slot A */
	reset_common_data();
	TEST_SUCC(vb2_select_fw_slot(ctx), "select slot A");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_UNKNOWN, "result unknown");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 0, "tried A");
	TEST_EQ(sd->fw_slot, 0, "selected A");
	TEST_EQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "didn't choose B");

	/* Slot B */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_TRY_NEXT, 1);
	TEST_SUCC(vb2_select_fw_slot(ctx), "select slot B");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_UNKNOWN, "result unknown");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 1, "tried B");
	TEST_EQ(sd->fw_slot, 1, "selected B");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "ctx says choose B");

	/* Slot A ran out of tries */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_TRYING);
	TEST_SUCC(vb2_select_fw_slot(ctx), "select slot A out of tries");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_NEXT), 1, "try B next");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 1, "tried B");
	TEST_EQ(sd->fw_slot, 1, "selected B");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "ctx says choose B");

	/* Slot A ran out of tries, even with nofail active */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_NOFAIL_BOOT;
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_TRYING);
	TEST_SUCC(vb2_select_fw_slot(ctx), "select slot A out of tries");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_NEXT), 1, "try B next");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 1, "tried B");
	TEST_EQ(sd->fw_slot, 1, "selected B");
	TEST_NEQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "ctx says choose B");

	/* Slot A used up a try */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_TRY_COUNT, 3);
	TEST_SUCC(vb2_select_fw_slot(ctx), "try slot A");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_TRYING, "result trying");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 0, "tried A");
	TEST_EQ(sd->fw_slot, 0, "selected A");
	TEST_EQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "didn't choose B");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_COUNT), 2, "tries decremented");

	/* Slot A failed, but nofail active */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_NOFAIL_BOOT;
	vb2_nv_set(ctx, VB2_NV_TRY_COUNT, 3);
	TEST_SUCC(vb2_select_fw_slot(ctx), "try slot A");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_RESULT),
		VB2_FW_RESULT_TRYING, "result trying");
	TEST_NEQ(sd->status & VB2_SD_STATUS_CHOSE_SLOT, 0, "chose slot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_TRIED), 0, "tried A");
	TEST_EQ(sd->fw_slot, 0, "selected A");
	TEST_EQ(ctx->flags & VB2_CONTEXT_FW_SLOT_B, 0, "didn't choose B");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_TRY_COUNT), 3, "tries not decremented");

	/* Tried/result get copied to the previous fields */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_FW_TRIED, 0);
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_SUCCESS);
	vb2_select_fw_slot(ctx);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_PREV_TRIED), 0, "prev A");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_PREV_RESULT),	VB2_FW_RESULT_SUCCESS,
		"prev success");

	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_FW_TRIED, 1);
	vb2_nv_set(ctx, VB2_NV_FW_RESULT, VB2_FW_RESULT_FAILURE);
	vb2_select_fw_slot(ctx);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_PREV_TRIED), 1, "prev B");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_FW_PREV_RESULT),	VB2_FW_RESULT_FAILURE,
		"prev failure");
}

static void need_reboot_for_display_tests(void)
{
	/* Display not available, reboot required */
	reset_common_data();
	TEST_EQ(vb2api_need_reboot_for_display(ctx), 1,
		"need_reboot_for_display: need reboot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST), 1,
		"  set display request");

	/* Display available, don't need reboot */
	reset_common_data();
	sd->flags |= VB2_SD_FLAG_DISPLAY_AVAILABLE;
	TEST_EQ(vb2api_need_reboot_for_display(ctx), 0,
		"need_reboot_for_display: don't need reboot");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_DISPLAY_REQUEST), 0,
		"  not set display request");
}

static void clear_recovery_tests(void)
{

	/* Manual recovery */
	reset_common_data();
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	sd->recovery_reason = 4;
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, 5);
	vb2_nv_set(ctx, VB2_NV_RECOVERY_SUBCODE, 13);
	vb2_clear_recovery(ctx);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		0, "  request cleared");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE),
		0, "  subcode cleared");

	/* Broken screen */
	reset_common_data();
	*boot_mode = VB2_BOOT_MODE_BROKEN_SCREEN;
	sd->recovery_reason = 4;
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	vb2_nv_set(ctx, VB2_NV_RECOVERY_REQUEST, 5);
	vb2_nv_set(ctx, VB2_NV_RECOVERY_SUBCODE, 13);
	vb2_clear_recovery(ctx);
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_REQUEST),
		0, "  request cleared");
	TEST_EQ(vb2_nv_get(ctx, VB2_NV_RECOVERY_SUBCODE),
		4, "  subcode shifted");
}

static void get_recovery_reason_tests(void)
{
	reset_common_data();
	sd->recovery_reason = 4;
	TEST_EQ(vb2api_get_recovery_reason(ctx), 4, "correct recovery reason");
}

static void phone_recovery_enabled_tests(void)
{
	/* Phone recovery enabled */
	reset_common_data();
	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	TEST_EQ(vb2api_phone_recovery_enabled(ctx), 1,
		"phone recovery enabled");
	TEST_EQ(vb2api_phone_recovery_ui_enabled(ctx), 1,
		"  ui also enabled");

	/* Phone recovery disabled */
	reset_common_data();
	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	vb2_secdata_kernel_set(ctx, VB2_SECDATA_KERNEL_FLAGS,
			       VB2_SECDATA_KERNEL_FLAG_PHONE_RECOVERY_DISABLED);
	TEST_EQ(vb2api_phone_recovery_enabled(ctx), 0,
		"phone recovery disabled");
	TEST_EQ(vb2api_phone_recovery_ui_enabled(ctx), 0,
		"  ui also disabled");

	/* Only UI disabled */
	reset_common_data();
	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	vb2_secdata_kernel_set(
		ctx, VB2_SECDATA_KERNEL_FLAGS,
		VB2_SECDATA_KERNEL_FLAG_PHONE_RECOVERY_UI_DISABLED);
	TEST_EQ(vb2api_phone_recovery_enabled(ctx), 1,
		"phone recovery enabled again");
	TEST_EQ(vb2api_phone_recovery_ui_enabled(ctx), 0,
		"  ui disabled");
}

static void diagnostic_ui_enabled_tests(void)
{
	reset_common_data();
	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	TEST_EQ(vb2api_diagnostic_ui_enabled(ctx), 1,
		"diagnostic UI enabled");

	reset_common_data();
	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	vb2_secdata_kernel_set(
		ctx, VB2_SECDATA_KERNEL_FLAGS,
		VB2_SECDATA_KERNEL_FLAG_DIAGNOSTIC_UI_DISABLED);
	TEST_EQ(vb2api_diagnostic_ui_enabled(ctx), 0,
		"diagnostic UI disabled");
}

static void dev_default_boot_tests(void)
{
	/* No default boot */
	reset_common_data();
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"no default boot, boot disk");

	/* Set boot altfw by GBB */
	reset_common_data();
	gbb.flags |= VB2_GBB_FLAG_DEFAULT_DEV_BOOT_ALTFW;
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_EXTERNAL);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_ALTFW,
		"GBB set default boot altfw");

	/* Boot from internal disk */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"set default boot internal disk");

	/* Boot from external disk */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED;
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_EXTERNAL);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_EXTERNAL,
		"set default boot external disk");

	/* Boot from external disk not allowed */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_EXTERNAL);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"default boot external not allowed");
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED;
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_EXTERNAL);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"default boot external not allowed");

	/* Boot altfw */
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED;
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_ALTFW);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_ALTFW,
		"set default boot altfw");

	/* Boot altfw not allowed */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_ALTFW);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"default boot altfw not allowed");
	reset_common_data();
	ctx->flags |= VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED;
	vb2_nv_set(ctx, VB2_NV_DEV_DEFAULT_BOOT,
		   VB2_DEV_DEFAULT_BOOT_TARGET_ALTFW);
	TEST_EQ(vb2api_get_dev_default_boot_target(ctx),
		VB2_DEV_DEFAULT_BOOT_TARGET_INTERNAL,
		"default boot altfw not allowed");
}

static void fill_dev_boot_flags_tests(void)
{
	/* Dev boot - allowed by default */
	reset_common_data();
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALLOWED,
		  "dev boot - allowed by default");

	/* Dev boot - disabled by FWMP */
	reset_common_data();
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_DISABLE_BOOT;
	vb2_fill_dev_boot_flags(ctx);
	TEST_FALSE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALLOWED,
		   "dev boot - FWMP disabled");

	/* Dev boot - force enabled by GBB */
	reset_common_data();
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_DISABLE_BOOT;
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_SWITCH_ON;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALLOWED,
		  "dev boot - GBB force dev on");

	/* External boot - not allowed by default */
	reset_common_data();
	vb2_fill_dev_boot_flags(ctx);
	TEST_FALSE(ctx->flags & VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED,
		   "dev boot external - not allowed by default");

	/* External boot - enabled by nvdata */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_EXTERNAL, 1);
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED,
		  "dev boot external - nvdata enabled");

	/* External boot - enabled by FWMP */
	reset_common_data();
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_EXTERNAL;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED,
		  "dev boot external - secdata enabled");

	/* External boot - force enabled by GBB */
	reset_common_data();
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_BOOT_USB;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED,
		  "dev boot external - GBB force enabled");

	/* External boot - set all flags */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_EXTERNAL, 1);
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_EXTERNAL;
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_BOOT_USB;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_EXTERNAL_ALLOWED,
		  "dev boot external - all flags set");

	/* Alternate boot - not allowed by default */
	reset_common_data();
	vb2_fill_dev_boot_flags(ctx);
	TEST_FALSE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED,
		   "dev boot altfw - not allowed by default");

	/* Alternate boot - enabled by nvdata */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_ALTFW, 1);
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED,
		  "dev boot altfw - nvdata enabled");

	/* Alternate boot - enabled by FWMP */
	reset_common_data();
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_ALTFW;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED,
		  "dev boot altfw - secdata enabled");

	/* Alternate boot - force enabled by GBB */
	reset_common_data();
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_BOOT_ALTFW;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED,
		  "dev boot altfw - GBB force enabled");

	/* Alternate boot - set all flags */
	reset_common_data();
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_ALTFW, 1);
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_ALTFW;
	gbb.flags |= VB2_GBB_FLAG_FORCE_DEV_BOOT_ALTFW;
	vb2_fill_dev_boot_flags(ctx);
	TEST_TRUE(ctx->flags & VB2_CONTEXT_DEV_BOOT_ALTFW_ALLOWED,
		  "dev boot altfw - all flags set");
}

static void use_dev_screen_short_delay_tests(void)
{
	/* Normal delay */
	reset_common_data();
	TEST_EQ(vb2api_use_short_dev_screen_delay(ctx), 0,
		"short delay: no");

	/* Short delay */
	gbb.flags |= VB2_GBB_FLAG_DEV_SCREEN_SHORT_DELAY;
	TEST_EQ(vb2api_use_short_dev_screen_delay(ctx), 1,
		"short delay: yes");
}

int main(int argc, char* argv[])
{
	init_workbuf_tests();
	misc_tests();
	gbb_tests();
	fail_tests();
	recovery_tests();
	dev_switch_tests();
	enable_dev_tests();
	tpm_clear_tests();
	select_slot_tests();
	need_reboot_for_display_tests();
	clear_recovery_tests();
	get_recovery_reason_tests();
	phone_recovery_enabled_tests();
	diagnostic_ui_enabled_tests();
	dev_default_boot_tests();
	fill_dev_boot_flags_tests();
	use_dev_screen_short_delay_tests();

	return gTestSuccess ? 0 : 255;
}
