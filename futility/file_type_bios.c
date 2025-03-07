/* Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "file_type_bios.h"
#include "file_type.h"
#include "fmap.h"
#include "futility.h"
#include "futility_options.h"
#include "host_common.h"
#include "vb1_helper.h"

static const char * const fmap_name[] = {
	"GBB",					/* BIOS_FMAP_GBB */
	"FW_MAIN_A",				/* BIOS_FMAP_FW_MAIN_A */
	"FW_MAIN_B",				/* BIOS_FMAP_FW_MAIN_B */
	"VBLOCK_A",				/* BIOS_FMAP_VBLOCK_A */
	"VBLOCK_B",				/* BIOS_FMAP_VBLOCK_B */
};
_Static_assert(ARRAY_SIZE(fmap_name) == NUM_BIOS_COMPONENTS,
	       "Size of fmap_name[] should match NUM_BIOS_COMPONENTS");

static void fmap_limit_area(FmapAreaHeader *ah, uint32_t len)
{
	uint32_t sum = ah->area_offset + ah->area_size;
	if (sum < ah->area_size || sum > len) {
		VB2_DEBUG("%s %#x + %#x > %#x\n",
			  ah->area_name, ah->area_offset, ah->area_size, len);
		ah->area_offset = 0;
		ah->area_size = 0;
	}
}

/** Show functions **/

static int show_gbb_buf(const char *name, uint8_t *buf, uint32_t len,
			void *data)
{
	struct vb2_gbb_header *gbb = (struct vb2_gbb_header *)buf;
	struct bios_state_s *state = (struct bios_state_s *)data;
	int retval = 0;
	uint32_t maxlen = 0;

	if (!len) {
		printf("GBB header:              %s <invalid>\n", name);
		return 1;
	}

	/* It looks like a GBB or we wouldn't be called. */
	if (!futil_valid_gbb_header(gbb, len, &maxlen))
		retval = 1;

	printf("GBB header:              %s\n", name);
	printf("  Version:               %d.%d\n",
	       gbb->major_version, gbb->minor_version);
	printf("  Flags:                 0x%08x\n", gbb->flags);
	printf("  Regions:                 offset       size\n");
	printf("    hwid                 0x%08x   0x%08x\n",
	       gbb->hwid_offset, gbb->hwid_size);
	printf("    bmpvf                0x%08x   0x%08x\n",
	       gbb->bmpfv_offset, gbb->bmpfv_size);
	printf("    rootkey              0x%08x   0x%08x\n",
	       gbb->rootkey_offset, gbb->rootkey_size);
	printf("    recovery_key         0x%08x   0x%08x\n",
	       gbb->recovery_key_offset, gbb->recovery_key_size);

	printf("  Size:                  0x%08x / 0x%08x%s\n",
	       maxlen, len, maxlen > len ? "  (not enough)" : "");

	if (retval) {
		printf("GBB header is invalid, ignoring content\n");
		return retval;
	}

	printf("GBB content:\n");
	printf("  HWID:                  %s\n", buf + gbb->hwid_offset);
	print_hwid_digest(gbb, "     digest:             ", "\n");

	struct vb2_packed_key *pubkey =
		(struct vb2_packed_key *)(buf + gbb->rootkey_offset);
	if (vb2_packed_key_looks_ok(pubkey, gbb->rootkey_size) == VB2_SUCCESS) {
		if (state) {
			state->rootkey.offset =
				state->area[BIOS_FMAP_GBB].offset +
				gbb->rootkey_offset;
			state->rootkey.buf = buf + gbb->rootkey_offset;
			state->rootkey.len = gbb->rootkey_size;
			state->rootkey.is_valid = 1;
		}
		printf("  Root Key:\n");
		show_pubkey(pubkey, "    ");
	} else {
		retval = 1;
		printf("  Root Key:              <invalid>\n");
	}

	pubkey = (struct vb2_packed_key *)(buf + gbb->recovery_key_offset);
	if (vb2_packed_key_looks_ok(pubkey, gbb->recovery_key_size)
	    == VB2_SUCCESS) {
		if (state) {
			state->recovery_key.offset =
				state->area[BIOS_FMAP_GBB].offset +
				gbb->recovery_key_offset;
			state->recovery_key.buf = buf +
				gbb->recovery_key_offset;
			state->recovery_key.len = gbb->recovery_key_size;
			state->recovery_key.is_valid = 1;
		}
		printf("  Recovery Key:\n");
		show_pubkey(pubkey, "    ");
	} else {
		retval = 1;
		printf("  Recovery Key:          <invalid>\n");
	}

	if (!retval && state)
		state->area[BIOS_FMAP_GBB].is_valid = 1;

	return retval;
}

int ft_show_gbb(const char *name, void *data)
{
	int retval = 0;
	int fd = -1;
	uint8_t *buf;
	uint32_t len;

	retval = futil_open_and_map_file(name, &fd, FILE_RO, &buf, &len);
	if (retval)
		return 1;

	retval = show_gbb_buf(name, buf, len, data);

	futil_unmap_and_close_file(fd, FILE_RO, buf, len);
	return retval;
}

/*
 * This handles FW_MAIN_A and FW_MAIN_B while processing a BIOS image.
 *
 * The data is just the RW firmware blob, so there's nothing useful to show
 * about it. We'll just mark it as present so when we encounter corresponding
 * VBLOCK area, we'll have this to verify.
 */
static int fmap_show_fw_main(const char *name, uint8_t *buf, uint32_t len,
			     void *data)
{
	struct bios_state_s *state = (struct bios_state_s *)data;

	if (!len) {
		printf("Firmware body:           %s <invalid>\n", name);
		return 1;
	}

	printf("Firmware body:           %s\n", name);
	printf("  Offset:                0x%08x\n",
	       state->area[state->c].offset);
	printf("  Size:                  0x%08x\n", len);

	state->area[state->c].is_valid = 1;

	return 0;
}

/* Functions to call to show the bios components */
static int (*fmap_show_fn[])(const char *name, uint8_t *buf, uint32_t len,
			       void *data) = {
	show_gbb_buf,
	fmap_show_fw_main,
	fmap_show_fw_main,
	show_fw_preamble_buf,
	show_fw_preamble_buf,
};
_Static_assert(ARRAY_SIZE(fmap_show_fn) == NUM_BIOS_COMPONENTS,
	       "Size of fmap_show_fn[] should match NUM_BIOS_COMPONENTS");

int ft_show_bios(const char *name, void *data)
{
	FmapHeader *fmap;
	FmapAreaHeader *ah = 0;
	char ah_name[FMAP_NAMELEN + 1];
	enum bios_component c;
	int retval = 0;
	struct bios_state_s state;
	int fd = -1;
	uint8_t *buf;
	uint32_t len;

	retval = futil_open_and_map_file(name, &fd, FILE_RO, &buf, &len);
	if (retval)
		return 1;

	memset(&state, 0, sizeof(state));

	printf("BIOS:                    %s\n", name);

	/* We've already checked, so we know this will work. */
	fmap = fmap_find(buf, len);
	for (c = 0; c < NUM_BIOS_COMPONENTS; c++) {
		/* We know one of these will work, too */
		if (fmap_find_by_name(buf, len, fmap, fmap_name[c], &ah)) {
			/* But the file might be truncated */
			fmap_limit_area(ah, len);
			/* The name is not necessarily null-terminated */
			snprintf(ah_name, sizeof(ah_name), "%s", ah->area_name);

			/* Update the state we're passing around */
			state.c = c;
			state.area[c].offset = ah->area_offset;
			state.area[c].buf = buf + ah->area_offset;
			state.area[c].len = ah->area_size;

			VB2_DEBUG("showing FMAP area %d (%s),"
				  " offset=0x%08x len=0x%08x\n",
				  c, ah_name, ah->area_offset, ah->area_size);

			/* Go look at it. */
			if (fmap_show_fn[c])
				retval += fmap_show_fn[c](ah_name,
							  state.area[c].buf,
							  state.area[c].len,
							  &state);
		}
	}

	futil_unmap_and_close_file(fd, FILE_RO, buf, len);
	return retval;
}

/** Sign functions **/

/*
 * This handles FW_MAIN_A and FW_MAIN_B while signing a BIOS image. The data is
 * just the RW firmware blob so there's nothing useful to do with it, but we'll
 * mark it as valid so that we'll know that this FMAP area exists and can
 * be signed.
 */
static int fmap_sign_fw_main(const char *name, uint8_t *buf, uint32_t len,
			     void *data)
{
	struct bios_state_s *state = (struct bios_state_s *)data;
	state->area[state->c].is_valid = 1;
	return 0;
}

/*
 * This handles VBLOCK_A and VBLOCK_B while processing a BIOS image. We don't
 * do any signing here. We just check to see if the existing FMAP area contains
 * a firmware preamble so we can preserve its contents. We do the signing once
 * we've looked over all the components.
 */
static int fmap_sign_fw_preamble(const char *name, uint8_t *buf, uint32_t len,
				 void *data)
{
	static uint8_t workbuf[VB2_FIRMWARE_WORKBUF_RECOMMENDED_SIZE]
		__attribute__((aligned(VB2_WORKBUF_ALIGN)));
	static struct vb2_workbuf wb;
	vb2_workbuf_init(&wb, workbuf, sizeof(workbuf));

	struct vb2_keyblock *keyblock = (struct vb2_keyblock *)buf;
	struct bios_state_s *state = (struct bios_state_s *)data;

	/*
	 * If we have a valid keyblock and fw_preamble, then we can use them to
	 * determine the size of the firmware body. Otherwise, we'll have to
	 * just sign the whole region.
	 */
	if (VB2_SUCCESS != vb2_verify_keyblock_hash(keyblock, len, &wb)) {
		fprintf(stderr, "Warning: %s keyblock is invalid. "
			"Signing the entire FW FMAP region...\n", name);
		goto whatever;
	}

	if (vb2_packed_key_looks_ok(&keyblock->data_key,
				    keyblock->data_key.key_offset +
				    keyblock->data_key.key_size)) {
		fprintf(stderr, "Warning: %s public key is invalid. "
			"Signing the entire FW FMAP region...\n", name);
		goto whatever;
	}
	uint32_t more = keyblock->keyblock_size;
	struct vb2_fw_preamble *preamble =
		(struct vb2_fw_preamble *)(buf + more);
	uint32_t fw_size = preamble->body_signature.data_size;
	struct bios_area_s *fw_body_area = 0;

	switch (state->c) {
	case BIOS_FMAP_VBLOCK_A:
		fw_body_area = &state->area[BIOS_FMAP_FW_MAIN_A];
		/* Preserve the flags if they're not specified */
		if (!sign_option.flags_specified)
			sign_option.flags = preamble->flags;
		break;
	case BIOS_FMAP_VBLOCK_B:
		fw_body_area = &state->area[BIOS_FMAP_FW_MAIN_B];
		break;
	default:
		FATAL("Can only handle VBLOCK_A or VBLOCK_B\n");
	}

	if (fw_size > fw_body_area->len) {
		fprintf(stderr,
			"%s says the firmware is larger than we have\n",
			name);
		return 1;
	}

	/* Update the firmware size */
	fw_body_area->len = fw_size;

whatever:
	state->area[state->c].is_valid = 1;

	return 0;
}

static int write_new_preamble(struct bios_area_s *vblock,
			      struct bios_area_s *fw_body,
			      struct vb2_private_key *signkey,
			      struct vb2_keyblock *keyblock)
{
	struct vb2_signature *body_sig;
	struct vb2_fw_preamble *preamble;

	body_sig = vb2_calculate_signature(fw_body->buf, fw_body->len, signkey);
	if (!body_sig) {
		fprintf(stderr, "Error calculating body signature\n");
		return 1;
	}

	preamble = vb2_create_fw_preamble(sign_option.version,
			(struct vb2_packed_key *)sign_option.kernel_subkey,
			body_sig,
			signkey,
			sign_option.flags);
	if (!preamble) {
		fprintf(stderr, "Error creating firmware preamble.\n");
		free(body_sig);
		return 1;
	}

	/* Write the new keyblock */
	uint32_t more = keyblock->keyblock_size;
	memcpy(vblock->buf, keyblock, more);
	/* and the new preamble */
	memcpy(vblock->buf + more, preamble, preamble->preamble_size);

	free(preamble);
	free(body_sig);

	return 0;
}

static int write_loem(const char *ab, struct bios_area_s *vblock)
{
	char filename[PATH_MAX];
	int n;
	n = snprintf(filename, sizeof(filename), "%s/vblock_%s.%s",
		     sign_option.loemdir ? sign_option.loemdir : ".",
		     ab, sign_option.loemid);
	if (n >= sizeof(filename)) {
		fprintf(stderr, "LOEM args produce bogus filename\n");
		return 1;
	}

	FILE *fp = fopen(filename, "w");
	if (!fp) {
		fprintf(stderr, "Can't open %s for writing: %s\n",
			filename, strerror(errno));
		return 1;
	}

	if (1 != fwrite(vblock->buf, vblock->len, 1, fp)) {
		fprintf(stderr, "Can't write to %s: %s\n",
			filename, strerror(errno));
		fclose(fp);
		return 1;
	}
	if (fclose(fp)) {
		fprintf(stderr, "Failed closing loem output: %s\n",
			strerror(errno));
		return 1;
	}

	return 0;
}

/* This signs a full BIOS image after it's been traversed. */
static int sign_bios_at_end(struct bios_state_s *state)
{
	struct bios_area_s *vblock_a = &state->area[BIOS_FMAP_VBLOCK_A];
	struct bios_area_s *vblock_b = &state->area[BIOS_FMAP_VBLOCK_B];
	struct bios_area_s *fw_a = &state->area[BIOS_FMAP_FW_MAIN_A];
	struct bios_area_s *fw_b = &state->area[BIOS_FMAP_FW_MAIN_B];
	int retval = 0;

	if (!vblock_a->is_valid || !vblock_b->is_valid ||
	    !fw_a->is_valid || !fw_b->is_valid) {
		fprintf(stderr, "Something's wrong. Not changing anything\n");
		return 1;
	}

	retval |= write_new_preamble(vblock_a, fw_a, sign_option.signprivate,
				     sign_option.keyblock);

	retval |= write_new_preamble(vblock_b, fw_b, sign_option.signprivate,
				     sign_option.keyblock);

	if (sign_option.loemid) {
		retval |= write_loem("A", vblock_a);
		retval |= write_loem("B", vblock_b);
	}

	return retval;
}

/* Functions to call while preparing to sign the bios */
static int (*fmap_sign_fn[])(const char *name, uint8_t *buf, uint32_t len,
			     void *data) = {
	0,
	fmap_sign_fw_main,
	fmap_sign_fw_main,
	fmap_sign_fw_preamble,
	fmap_sign_fw_preamble,
};
_Static_assert(ARRAY_SIZE(fmap_sign_fn) == NUM_BIOS_COMPONENTS,
	       "Size of fmap_sign_fn[] should match NUM_BIOS_COMPONENTS");

int ft_sign_bios(const char *name, void *data)
{
	FmapHeader *fmap;
	FmapAreaHeader *ah = 0;
	char ah_name[FMAP_NAMELEN + 1];
	enum bios_component c;
	int retval = 0;
	struct bios_state_s state;
	int fd = -1;
	uint8_t *buf = NULL;
	uint32_t len = 0;

	retval = futil_open_and_map_file(name, &fd, FILE_MODE_SIGN(sign_option),
					 &buf, &len);
	if (retval)
		return 1;

	memset(&state, 0, sizeof(state));

	/* We've already checked, so we know this will work. */
	fmap = fmap_find(buf, len);
	for (c = 0; c < NUM_BIOS_COMPONENTS; c++) {
		/* We know one of these will work, too */
		if (fmap_find_by_name(buf, len, fmap, fmap_name[c], &ah)) {
			/* But the file might be truncated */
			fmap_limit_area(ah, len);
			/* The name is not necessarily null-terminated */
			snprintf(ah_name, sizeof(ah_name), "%s", ah->area_name);

			/* Update the state we're passing around */
			state.c = c;
			state.area[c].buf = buf + ah->area_offset;
			state.area[c].len = ah->area_size;

			VB2_DEBUG("examining FMAP area %d (%s),"
				  " offset=0x%08x len=0x%08x\n",
				  c, ah_name, ah->area_offset, ah->area_size);

			/* Go look at it, but abort on error */
			if (fmap_sign_fn[c])
				retval += fmap_sign_fn[c](ah_name,
							  state.area[c].buf,
							  state.area[c].len,
							  &state);
		}
	}

	retval += sign_bios_at_end(&state);

	futil_unmap_and_close_file(fd, FILE_MODE_SIGN(sign_option), buf, len);
	return retval;
}

enum futil_file_type ft_recognize_bios_image(uint8_t *buf, uint32_t len)
{
	FmapHeader *fmap;
	enum bios_component c;

	fmap = fmap_find(buf, len);
	if (!fmap)
		return FILE_TYPE_UNKNOWN;

	for (c = 0; c < NUM_BIOS_COMPONENTS; c++)
		if (!fmap_find_by_name(buf, len, fmap, fmap_name[c], 0))
			break;
	if (c == NUM_BIOS_COMPONENTS)
		return FILE_TYPE_BIOS_IMAGE;

	return FILE_TYPE_UNKNOWN;
}
