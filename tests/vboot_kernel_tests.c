/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for vboot_kernel.c
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2secdata.h"
#include "2secdata_struct.h"
#include "cgptlib.h"
#include "cgptlib_internal.h"
#include "gpt.h"
#include "load_kernel_fw.h"
#include "test_common.h"
#include "vboot_api.h"

/* Mock kernel partition */
struct mock_part {
	uint32_t start;
	uint32_t size;
};

/* Partition list; ends with a 0-size partition. */
#define MOCK_PART_COUNT 8
static struct mock_part mock_parts[MOCK_PART_COUNT];
static int mock_part_next;

/* Mock data */
static uint8_t kernel_buffer[80000];
static int disk_read_to_fail;
static int gpt_init_fail;
static int keyblock_verify_fail;  /* 0=ok, 1=sig, 2=hash */
static int preamble_verify_fail;
static int verify_data_fail;
static int unpack_key_fail;
static int gpt_flag_external;

static struct vb2_gbb_header gbb;
static VbSelectAndLoadKernelParams lkp;
static VbDiskInfo disk_info;
static struct vb2_keyblock kbh;
static struct vb2_kernel_preamble kph;
static struct vb2_secdata_fwmp *fwmp;
static uint8_t mock_digest[VB2_SHA256_DIGEST_SIZE] = {12, 34, 56, 78};
static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_packed_key mock_key;
static enum vb2_boot_mode *boot_mode;

/**
 * Reset mock data (for use before each test)
 */
static void ResetMocks(void)
{
	disk_read_to_fail = -1;

	gpt_init_fail = 0;
	keyblock_verify_fail = 0;
	preamble_verify_fail = 0;
	verify_data_fail = 0;
	unpack_key_fail = 0;

	gpt_flag_external = 0;

	memset(&gbb, 0, sizeof(gbb));
	gbb.major_version = VB2_GBB_MAJOR_VER;
	gbb.minor_version = VB2_GBB_MINOR_VER;
	gbb.flags = 0;

	memset(&lkp, 0, sizeof(lkp));
	lkp.kernel_buffer = kernel_buffer;
	lkp.kernel_buffer_size = sizeof(kernel_buffer);
	lkp.disk_handle = (VbExDiskHandle_t)1;

	memset(&disk_info, 0, sizeof(disk_info));
	disk_info.bytes_per_lba = 512;
	disk_info.streaming_lba_count = 1024;
	disk_info.lba_count = 1024;
	disk_info.handle = lkp.disk_handle;

	memset(&kbh, 0, sizeof(kbh));
	kbh.data_key.key_version = 2;
	kbh.keyblock_flags = -1;
	kbh.keyblock_size = sizeof(kbh);

	memset(&kph, 0, sizeof(kph));
	kph.kernel_version = 1;
	kph.preamble_size = 4096 - kbh.keyblock_size;
	kph.body_signature.data_size = 70144;
	kph.bootloader_address = 0xbeadd008;
	kph.bootloader_size = 0x1234;

	memset(mock_parts, 0, sizeof(mock_parts));
	mock_parts[0].start = 100;
	mock_parts[0].size = 150;  /* 75 KB */
	mock_part_next = 0;

	memset(&mock_key, 0, sizeof(mock_key));

	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");
	vb2_nv_init(ctx);

	sd = vb2_get_sd(ctx);
	sd->kernel_version_secdata = 0x20001;

	/* CRC will be invalid after here, but nobody's checking */
	sd->status |= VB2_SD_STATUS_SECDATA_FWMP_INIT;
	fwmp = (struct vb2_secdata_fwmp *)ctx->secdata_fwmp;
	memcpy(&fwmp->dev_key_hash, mock_digest, sizeof(fwmp->dev_key_hash));

	boot_mode = (enum vb2_boot_mode *)&ctx->boot_mode;
	*boot_mode = VB2_BOOT_MODE_NORMAL;

	// TODO: more workbuf fields - flags, secdata_firmware

	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	vb2_secdata_kernel_set(ctx, VB2_SECDATA_KERNEL_FLAGS,
			VB2_SECDATA_KERNEL_FLAG_HWCRYPTO_ALLOWED);
}

/* Mocks */
struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &gbb;
}

vb2_error_t vb2ex_read_resource(struct vb2_context *c,
				enum vb2_resource_index index, uint32_t offset,
				void *buf, uint32_t size)
{
	memset(buf, 0, size);
	return VB2_SUCCESS;
}

vb2_error_t vb2_gbb_read_root_key(struct vb2_context *c,
				  struct vb2_packed_key **keyp, uint32_t *size,
				  struct vb2_workbuf *wb)
{
	*keyp = &mock_key;
	return VB2_SUCCESS;
}

vb2_error_t vb2_gbb_read_recovery_key(struct vb2_context *c,
				      struct vb2_packed_key **keyp,
				      uint32_t *size, struct vb2_workbuf *wb)
{
	*keyp = &mock_key;
	return VB2_SUCCESS;
}

vb2_error_t VbExDiskRead(VbExDiskHandle_t h, uint64_t lba_start,
			 uint64_t lba_count, void *buffer)
{
	if ((int)lba_start == disk_read_to_fail)
		return VB2_ERROR_MOCK;

	return VB2_SUCCESS;
}

int AllocAndReadGptData(VbExDiskHandle_t disk_handle, GptData *gptdata)
{
	return GPT_SUCCESS;
}

int GptInit(GptData *gpt)
{
	return gpt_init_fail;
}

int GptNextKernelEntry(GptData *gpt, uint64_t *start_sector, uint64_t *size)
{
	struct mock_part *p = mock_parts + mock_part_next;

	if (!p->size)
		return GPT_ERROR_NO_VALID_KERNEL;

	if (gpt->flags & GPT_FLAG_EXTERNAL)
		gpt_flag_external++;

	gpt->current_kernel = mock_part_next;
	*start_sector = p->start;
	*size = p->size;
	mock_part_next++;
	return GPT_SUCCESS;
}

int GptUpdateKernelEntry(GptData *gpt, uint32_t update_type)
{
	return GPT_SUCCESS;
}

int WriteAndFreeGptData(VbExDiskHandle_t disk_handle, GptData *gptdata)
{
	return GPT_SUCCESS;
}

void GetCurrentKernelUniqueGuid(GptData *gpt, void *dest)
{
	static char fake_guid[] = "FakeGuid";

	memcpy(dest, fake_guid, sizeof(fake_guid));
}

vb2_error_t vb2_unpack_key_buffer(struct vb2_public_key *key,
				  const uint8_t *buf, uint32_t size)
{
	if (--unpack_key_fail == 0)
		return VB2_ERROR_MOCK;

	return VB2_SUCCESS;
}

vb2_error_t vb2_verify_keyblock(struct vb2_keyblock *block, uint32_t size,
				const struct vb2_public_key *key,
				const struct vb2_workbuf *wb)
{
	if (keyblock_verify_fail >= 1)
		return VB2_ERROR_MOCK;

	/* Use this as an opportunity to override the keyblock */
	memcpy((void *)block, &kbh, sizeof(kbh));
	return VB2_SUCCESS;
}

vb2_error_t vb2_verify_keyblock_hash(const struct vb2_keyblock *block,
				     uint32_t size,
				     const struct vb2_workbuf *wb)
{
	if (keyblock_verify_fail >= 2)
		return VB2_ERROR_MOCK;

	/* Use this as an opportunity to override the keyblock */
	memcpy((void *)block, &kbh, sizeof(kbh));
	return VB2_SUCCESS;
}

vb2_error_t vb2_verify_kernel_preamble(struct vb2_kernel_preamble *preamble,
			       uint32_t size, const struct vb2_public_key *key,
			       const struct vb2_workbuf *wb)
{
	if (preamble_verify_fail)
		return VB2_ERROR_MOCK;

	/* Use this as an opportunity to override the preamble */
	memcpy((void *)preamble, &kph, sizeof(kph));
	return VB2_SUCCESS;
}

vb2_error_t vb2_verify_data(const uint8_t *data, uint32_t size,
			    struct vb2_signature *sig,
			    const struct vb2_public_key *key,
			    const struct vb2_workbuf *wb)
{
	if (verify_data_fail)
		return VB2_ERROR_MOCK;

	return VB2_SUCCESS;
}

vb2_error_t vb2_digest_buffer(const uint8_t *buf, uint32_t size,
			      enum vb2_hash_algorithm hash_alg, uint8_t *digest,
			      uint32_t digest_size)
{
	memcpy(digest, mock_digest, sizeof(mock_digest));
	return VB2_SUCCESS;
}

/* Make sure nothing tested here ever calls this directly. */
void vb2api_fail(struct vb2_context *c, uint8_t reason, uint8_t subcode)
{
	TEST_TRUE(0, "  called vb2api_fail()");
}

static void TestLoadKernel(int expect_retval, const char *test_name)
{
	TEST_EQ(LoadKernel(ctx, &lkp, &disk_info), expect_retval, test_name);
}

/**
 * Trivial invalid calls to LoadKernel()
 */
static void InvalidParamsTest(void)
{
	ResetMocks();
	gpt_init_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_NO_KERNEL_FOUND, "Bad GPT");

	/* This causes the stream open call to fail */
	ResetMocks();
	lkp.disk_handle = NULL;
	disk_info.handle = NULL;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND, "Bad disk handle");
}

static void LoadKernelTest(void)
{
	ResetMocks();
	TestLoadKernel(0, "First kernel good");
	TEST_EQ(lkp.partition_number, 1, "  part num");
	TEST_EQ(lkp.bootloader_address, 0xbeadd008, "  bootloader addr");
	TEST_EQ(lkp.bootloader_size, 0x1234, "  bootloader size");
	TEST_STR_EQ((char *)lkp.partition_guid, "FakeGuid", "  guid");
	TEST_EQ(gpt_flag_external, 0, "GPT was internal");
	TEST_NEQ(sd->flags & VB2_SD_FLAG_KERNEL_SIGNED, 0, "  use signature");

	ResetMocks();
	mock_parts[1].start = 300;
	mock_parts[1].size = 150;
	TestLoadKernel(0, "Two good kernels");
	TEST_EQ(lkp.partition_number, 1, "  part num");
	TEST_EQ(mock_part_next, 1, "  didn't read second one");

	/* Fail if no kernels found */
	ResetMocks();
	mock_parts[0].size = 0;
	TestLoadKernel(VB2_ERROR_LK_NO_KERNEL_FOUND, "No kernels");

	/* Skip kernels which are too small */
	ResetMocks();
	mock_parts[0].size = 10;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND, "Too small");

	ResetMocks();
	disk_read_to_fail = 100;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail reading kernel start");

	ResetMocks();
	keyblock_verify_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail key block sig");

	/* In dev mode, fail if hash is bad too */
	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	keyblock_verify_fail = 2;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail key block dev hash");

	/* But just bad sig is ok */
	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	keyblock_verify_fail = 1;
	TestLoadKernel(0, "Succeed keyblock dev sig");
	TEST_EQ(sd->flags & VB2_SD_FLAG_KERNEL_SIGNED, 0, "  use hash");

	/* In dev mode and requiring signed kernel, fail if sig is bad */
	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 1);
	keyblock_verify_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail key block dev sig");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_OFFICIAL_ONLY;
	keyblock_verify_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail key block dev sig fwmp");

	/* Check keyblock flags */
	ResetMocks();
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_0
		| VB2_KEYBLOCK_FLAG_DEVELOPER_1
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock dev flag mismatch");

	ResetMocks();
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock rec flag mismatch");

	ResetMocks();
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_0
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock minios flag mismatch");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_1
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock recdev flag mismatch");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(0, "Keyblock rec flag okay");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE | VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock rec!dev flag mismatch");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE | VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_1
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(0, "Keyblock recdev flag okay");

	/* Check keyblock flags (dev mode + signed kernel required) */
	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 1);
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock dev flag mismatch (signed kernel required)");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_OFFICIAL_ONLY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_1
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock dev flag mismatch (signed kernel required)");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_OFFICIAL_ONLY;
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_0
		| VB2_KEYBLOCK_FLAG_DEVELOPER_0
		| VB2_KEYBLOCK_FLAG_MINIOS_1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock dev flag mismatch (signed kernel required)");

	ResetMocks();
	ctx->flags |= VB2_CONTEXT_DEVELOPER_MODE;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 1);
	kbh.keyblock_flags = VB2_KEYBLOCK_FLAG_RECOVERY_0
		| VB2_KEYBLOCK_FLAG_DEVELOPER_1
		| VB2_KEYBLOCK_FLAG_MINIOS_0;
	TestLoadKernel(0, "Keyblock dev flag okay (signed kernel required)");

	/* Check kernel key version */
	ResetMocks();
	kbh.data_key.key_version = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock kernel key rollback");

	ResetMocks();
	kbh.data_key.key_version = 0x10000;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock kernel key version too big");

	ResetMocks();
	kbh.data_key.key_version = 3;
	TestLoadKernel(0, "Keyblock version roll forward");
	TEST_EQ(sd->kernel_version, 0x30001, "  SD version");

	ResetMocks();
	kbh.data_key.key_version = 3;
	mock_parts[1].start = 300;
	mock_parts[1].size = 150;
	TestLoadKernel(0, "Two kernels roll forward");
	TEST_EQ(mock_part_next, 2, "  read both");
	TEST_EQ(sd->kernel_version, 0x30001, "  SD version");

	ResetMocks();
	kbh.data_key.key_version = 1;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	TestLoadKernel(0, "Key version ignored in dev mode");

	ResetMocks();
	kbh.data_key.key_version = 1;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	TestLoadKernel(0, "Key version ignored in rec mode");

	ResetMocks();
	unpack_key_fail = 2;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND, "Bad data key");

	ResetMocks();
	preamble_verify_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND, "Bad preamble");

	ResetMocks();
	kph.kernel_version = 0;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Kernel version rollback");

	ResetMocks();
	kph.kernel_version = 0;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	TestLoadKernel(0, "Kernel version ignored in dev mode");

	ResetMocks();
	kph.kernel_version = 0;
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	TestLoadKernel(0, "Kernel version ignored in rec mode");

	/* Check kernel version (dev mode + signed kernel required) */
	ResetMocks();
	kbh.data_key.key_version = 0;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	vb2_nv_set(ctx, VB2_NV_DEV_BOOT_SIGNED_ONLY, 1);
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock key version checked in dev mode "
		       "(signed kernel required)");

	ResetMocks();
	kbh.data_key.key_version = 0;
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_ENABLE_OFFICIAL_ONLY;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Keyblock key version checked in dev mode "
		       "(signed kernel required)");

	/* Check developer key hash - bad */
	ResetMocks();
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_USE_KEY_HASH;
	fwmp->dev_key_hash[0]++;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail keyblock dev fwmp hash");

	/* Check developer key hash - bad (recovery mode) */
	ResetMocks();
	*boot_mode = VB2_BOOT_MODE_MANUAL_RECOVERY;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_USE_KEY_HASH;
	fwmp->dev_key_hash[0]++;
	TestLoadKernel(0, "Bad keyblock dev fwmp hash ignored in rec mode");

	/* Check developer key hash - good */
	ResetMocks();
	*boot_mode = VB2_BOOT_MODE_DEVELOPER;
	fwmp->flags |= VB2_SECDATA_FWMP_DEV_USE_KEY_HASH;
	TestLoadKernel(0, "Good keyblock dev fwmp hash");

	ResetMocks();
	kph.preamble_size |= 0x07;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Kernel body offset");

	ResetMocks();
	kph.preamble_size += 65536;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Kernel body offset huge");

	/* Check getting kernel load address from header */
	ResetMocks();
	kph.body_load_address = (size_t)kernel_buffer;
	lkp.kernel_buffer = NULL;
	TestLoadKernel(0, "Get load address from preamble");
	TEST_PTR_EQ(lkp.kernel_buffer, kernel_buffer, "  address");
	/* Size is rounded up to nearest sector */
	TEST_EQ(lkp.kernel_buffer_size, 70144, "  size");

	ResetMocks();
	lkp.kernel_buffer_size = 8192;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Kernel too big for buffer");

	ResetMocks();
	mock_parts[0].size = 130;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Kernel too big for partition");

	ResetMocks();
	kph.body_signature.data_size = 8192;
	TestLoadKernel(0, "Kernel tiny");

	ResetMocks();
	disk_read_to_fail = 228;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND,
		       "Fail reading kernel data");

	ResetMocks();
	verify_data_fail = 1;
	TestLoadKernel(VB2_ERROR_LK_INVALID_KERNEL_FOUND, "Bad data");

	/* Check that EXTERNAL_GPT flag makes it down */
	ResetMocks();
	disk_info.flags |= VB_DISK_FLAG_EXTERNAL_GPT;
	TestLoadKernel(0, "Succeed external GPT");
	TEST_EQ(gpt_flag_external, 1, "GPT was external");

	/* Check recovery from unreadble primary GPT */
	ResetMocks();
	disk_read_to_fail = 1;
	TestLoadKernel(0, "Can't read disk");
}

int main(void)
{
	InvalidParamsTest();
	LoadKernelTest();

	return gTestSuccess ? 0 : 255;
}
