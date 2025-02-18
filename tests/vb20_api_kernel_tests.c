/* Copyright 2015 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Tests for kernel verification library, api layer
 */

#include <stdio.h>

#include "2api.h"
#include "2common.h"
#include "2misc.h"
#include "2nvstorage.h"
#include "2rsa.h"
#include "2secdata.h"
#include "2sysincludes.h"
#include "test_common.h"
#include "vboot_struct.h"

/* Common context for tests */
static uint8_t workbuf[VB2_KERNEL_WORKBUF_RECOMMENDED_SIZE]
	__attribute__((aligned(VB2_WORKBUF_ALIGN)));
static struct vb2_context *ctx;
static struct vb2_shared_data *sd;
static struct vb2_fw_preamble *fwpre;
static struct vb2_kernel_preamble *kpre;
static struct vb2_packed_key *kdkey;
static const char fw_kernel_key_data[36] = "Test kernel key data";
static char kernel_data[0x4008] = "Sure it's a kernel...";

/* Mocked function data */

static struct {
	struct vb2_gbb_header h;
	struct vb2_packed_key recovery_key;
	char recovery_key_data[32];
} mock_gbb;

static int mock_read_res_fail_on_call;
static int mock_unpack_key_retval;
static int mock_load_kernel_keyblock_retval;
static int mock_load_kernel_preamble_retval;
static int mock_secdata_fwmp_check_retval;

/* Type of test to reset for */
enum reset_type {
	FOR_PHASE1,
	FOR_PHASE2,
	FOR_PHASE3,
};

static void reset_common_data(enum reset_type t)
{
	struct vb2_packed_key *k;

	memset(workbuf, 0xaa, sizeof(workbuf));

	TEST_SUCC(vb2api_init(workbuf, sizeof(workbuf), &ctx),
		  "vb2api_init failed");

	sd = vb2_get_sd(ctx);
	sd->status |= VB2_SD_STATUS_RECOVERY_DECIDED;
	vb2_nv_init(ctx);

	vb2api_secdata_kernel_create(ctx);
	vb2_secdata_kernel_init(ctx);
	vb2_secdata_kernel_set(ctx, VB2_SECDATA_KERNEL_VERSIONS, 0x20002);

	mock_read_res_fail_on_call = 0;
	mock_unpack_key_retval = VB2_SUCCESS;
	mock_load_kernel_keyblock_retval = VB2_SUCCESS;
	mock_load_kernel_preamble_retval = VB2_SUCCESS;
	mock_secdata_fwmp_check_retval = VB2_SUCCESS;

	/* Recovery key in mock GBB */
	memset(&mock_gbb, 0, sizeof(mock_gbb));
	mock_gbb.recovery_key.algorithm = 11;
	mock_gbb.recovery_key.key_offset =
		vb2_offset_of(&mock_gbb.recovery_key,
			      &mock_gbb.recovery_key_data);
	mock_gbb.recovery_key.key_size = sizeof(mock_gbb.recovery_key_data);
	strcpy(mock_gbb.recovery_key_data, "The recovery key");
	mock_gbb.h.recovery_key_offset =
		vb2_offset_of(&mock_gbb, &mock_gbb.recovery_key);
	mock_gbb.h.recovery_key_size =
		mock_gbb.recovery_key.key_offset +
		mock_gbb.recovery_key.key_size;


	if (t == FOR_PHASE1) {
		uint8_t *kdata;

		/* Create mock firmware preamble in the context */
		sd->preamble_offset = sd->workbuf_used;
		fwpre = (struct vb2_fw_preamble *)
			vb2_member_of(sd, sd->preamble_offset);
		k = &fwpre->kernel_subkey;
		kdata = (uint8_t *)fwpre + sizeof(*fwpre);
		memcpy(kdata, fw_kernel_key_data, sizeof(fw_kernel_key_data));
		k->algorithm = 7;
		k->key_offset = vb2_offset_of(k, kdata);
		k->key_size = sizeof(fw_kernel_key_data);
		sd->preamble_size = sizeof(*fwpre) + k->key_size;
		vb2_set_workbuf_used(ctx,
				     sd->preamble_offset + sd->preamble_size);

		/* Needed to check that secdata_kernel initialization is
		   performed by phase1 function. */
		sd->status &= ~VB2_SD_STATUS_SECDATA_KERNEL_INIT;

	} else if (t == FOR_PHASE2) {
		struct vb2_signature *sig;
		struct vb2_digest_context dc;
		uint8_t *sdata;

		/* Create mock kernel data key */
		sd->data_key_offset = sd->workbuf_used;
		kdkey = (struct vb2_packed_key *)
			vb2_member_of(sd, sd->data_key_offset);
		kdkey->algorithm = VB2_ALG_RSA2048_SHA256;
		sd->data_key_size = sizeof(*kdkey);
		vb2_set_workbuf_used(ctx,
				     sd->data_key_offset + sd->data_key_size);

		/* Create mock kernel preamble in the context */
		sd->preamble_offset = sd->workbuf_used;
		kpre = (struct vb2_kernel_preamble *)
			vb2_member_of(sd, sd->preamble_offset);
		sdata = (uint8_t *)kpre + sizeof(*kpre);

		sig = &kpre->body_signature;
		sig->data_size = sizeof(kernel_data);
		sig->sig_offset = vb2_offset_of(sig, sdata);
		sig->sig_size = VB2_SHA512_DIGEST_SIZE;

		vb2_digest_init(&dc, VB2_HASH_SHA256);
		vb2_digest_extend(&dc, (const uint8_t *)kernel_data,
				  sizeof(kernel_data));
		vb2_digest_finalize(&dc, sdata, sig->sig_size);

		sd->preamble_size = sizeof(*kpre) + sig->sig_size;
		sd->vblock_preamble_offset =
			0x10000 - sd->preamble_size;
		vb2_set_workbuf_used(ctx,
				     sd->preamble_offset + sd->preamble_size);

	} else {
		/* Set flags and versions for roll-forward */
		sd->kernel_version = 0x20004;
		sd->kernel_version_secdata = 0x20002;
		sd->flags |= VB2_SD_FLAG_KERNEL_SIGNED;
		ctx->flags |= VB2_CONTEXT_ALLOW_KERNEL_ROLL_FORWARD;
	}
};

/* Mocked functions */

vb2_error_t vb2api_secdata_fwmp_check(struct vb2_context *c, uint8_t *size)
{
	return mock_secdata_fwmp_check_retval;
}

struct vb2_gbb_header *vb2_get_gbb(struct vb2_context *c)
{
	return &mock_gbb.h;
}

vb2_error_t vb2ex_read_resource(struct vb2_context *c,
				enum vb2_resource_index index, uint32_t offset,
				void *buf, uint32_t size)
{
	uint8_t *rptr;
	uint32_t rsize;

	if (--mock_read_res_fail_on_call == 0)
		return VB2_ERROR_MOCK;

	switch(index) {
	case VB2_RES_GBB:
		rptr = (uint8_t *)&mock_gbb;
		rsize = sizeof(mock_gbb);
		break;
	default:
		return VB2_ERROR_EX_READ_RESOURCE_INDEX;
	}

	if (offset > rsize || offset + size > rsize)
		return VB2_ERROR_EX_READ_RESOURCE_SIZE;

	memcpy(buf, rptr + offset, size);
	return VB2_SUCCESS;
}

vb2_error_t vb2_load_kernel_keyblock(struct vb2_context *c)
{
	return mock_load_kernel_keyblock_retval;
}

vb2_error_t vb2_load_kernel_preamble(struct vb2_context *c)
{
	return mock_load_kernel_preamble_retval;
}

vb2_error_t vb2_unpack_key_buffer(struct vb2_public_key *key,
				  const uint8_t *buf, uint32_t size)
{
	const struct vb2_packed_key *k = (const struct vb2_packed_key *)buf;

	key->arrsize = 0;
	key->hash_alg = vb2_crypto_to_hash(k->algorithm);
	return mock_unpack_key_retval;
}

vb2_error_t vb2_verify_digest(const struct vb2_public_key *key,
			      struct vb2_signature *sig, const uint8_t *digest,
			      const struct vb2_workbuf *wb)
{
	if (memcmp(digest, (uint8_t *)sig + sig->sig_offset, sig->sig_size))
		return VB2_ERROR_VDATA_VERIFY_DIGEST;

	return VB2_SUCCESS;
}

/* Tests */

static void load_kernel_vblock_tests(void)
{
	reset_common_data(FOR_PHASE1);
	TEST_SUCC(vb2api_load_kernel_vblock(ctx), "load vblock good");

	reset_common_data(FOR_PHASE1);
	mock_load_kernel_keyblock_retval = VB2_ERROR_MOCK;
	TEST_EQ(vb2api_load_kernel_vblock(ctx), VB2_ERROR_MOCK,
		"load vblock bad keyblock");

	reset_common_data(FOR_PHASE1);
	mock_load_kernel_preamble_retval = VB2_ERROR_MOCK;
	TEST_EQ(vb2api_load_kernel_vblock(ctx), VB2_ERROR_MOCK,
		"load vblock bad preamble");
}

static void get_kernel_size_tests(void)
{
	uint32_t offs, size;

	reset_common_data(FOR_PHASE2);
	offs = size = 0;
	TEST_SUCC(vb2api_get_kernel_size(ctx, &offs, &size), "get size good");
	TEST_EQ(offs, 0x10000, "  offset");
	TEST_EQ(size, sizeof(kernel_data), "  size");

	/* Don't need to pass pointers */
	reset_common_data(FOR_PHASE2);
	TEST_SUCC(vb2api_get_kernel_size(ctx, NULL, NULL), "get size null");

	reset_common_data(FOR_PHASE2);
	sd->preamble_size = 0;
	TEST_EQ(vb2api_get_kernel_size(ctx, &offs, &size),
		VB2_ERROR_API_GET_KERNEL_SIZE_PREAMBLE,
		"get size no preamble");
}

static void verify_kernel_data_tests(void)
{
	reset_common_data(FOR_PHASE2);
	TEST_SUCC(vb2api_verify_kernel_data(ctx, kernel_data,
					    sizeof(kernel_data)),
		  "verify data good");

	reset_common_data(FOR_PHASE2);
	sd->preamble_size = 0;
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_API_VERIFY_KDATA_PREAMBLE, "verify no preamble");

	reset_common_data(FOR_PHASE2);
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data) + 1),
		VB2_ERROR_API_VERIFY_KDATA_SIZE, "verify size");

	reset_common_data(FOR_PHASE2);
	sd->workbuf_used = sd->workbuf_size + VB2_WORKBUF_ALIGN -
			   vb2_wb_round_up(sizeof(struct vb2_digest_context));
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_API_VERIFY_KDATA_WORKBUF, "verify workbuf");

	reset_common_data(FOR_PHASE2);
	sd->data_key_size = 0;
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_API_VERIFY_KDATA_KEY, "verify no key");

	reset_common_data(FOR_PHASE2);
	mock_unpack_key_retval = VB2_ERROR_MOCK;
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_MOCK, "verify unpack key");

	reset_common_data(FOR_PHASE2);
	kdkey->algorithm = VB2_ALG_COUNT;
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_SHA_INIT_ALGORITHM, "verify hash init");

	reset_common_data(FOR_PHASE2);
	sd->workbuf_used = sd->workbuf_size -
			   vb2_wb_round_up(sizeof(struct vb2_digest_context));
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_API_CHECK_HASH_WORKBUF_DIGEST, "verify hash workbuf");

	reset_common_data(FOR_PHASE2);
	kernel_data[3] ^= 0xd0;
	TEST_EQ(vb2api_verify_kernel_data(ctx, kernel_data,
					  sizeof(kernel_data)),
		VB2_ERROR_VDATA_VERIFY_DIGEST, "verify hash digest");
	kernel_data[3] ^= 0xd0;
}

static void phase3_tests(void)
{
	uint32_t v;

	reset_common_data(FOR_PHASE3);
	TEST_SUCC(vb2api_kernel_phase3(ctx), "phase3 good");
	v = vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	TEST_EQ(v, 0x20004, "  version");

	reset_common_data(FOR_PHASE3);
	sd->kernel_version = 0x20001;
	TEST_SUCC(vb2api_kernel_phase3(ctx), "phase3 no rollback");
	v = vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	TEST_EQ(v, 0x20002, "  version");

	reset_common_data(FOR_PHASE3);
	sd->flags &= ~VB2_SD_FLAG_KERNEL_SIGNED;
	TEST_SUCC(vb2api_kernel_phase3(ctx), "phase3 unsigned kernel");
	v = vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	TEST_EQ(v, 0x20002, "  version");

	reset_common_data(FOR_PHASE3);
	ctx->flags |= VB2_CONTEXT_RECOVERY_MODE;
	TEST_SUCC(vb2api_kernel_phase3(ctx), "phase3 recovery");
	v = vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	TEST_EQ(v, 0x20002, "  version");

	reset_common_data(FOR_PHASE3);
	ctx->flags &= ~VB2_CONTEXT_ALLOW_KERNEL_ROLL_FORWARD;
	TEST_SUCC(vb2api_kernel_phase3(ctx), "phase3 no rollforward");
	v = vb2_secdata_kernel_get(ctx, VB2_SECDATA_KERNEL_VERSIONS);
	TEST_EQ(v, 0x20002, "  version");

	reset_common_data(FOR_PHASE3);
	sd->status &= ~VB2_SD_STATUS_SECDATA_KERNEL_INIT;
	TEST_ABORT(vb2api_kernel_phase3(ctx), "phase3 set fail");
}

int main(int argc, char* argv[])
{
	load_kernel_vblock_tests();
	get_kernel_size_tests();
	verify_kernel_data_tests();
	phase3_tests();

	return gTestSuccess ? 0 : 255;
}
