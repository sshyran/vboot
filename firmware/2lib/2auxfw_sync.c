/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Auxiliary firmware (auxfw) sync routines for vboot.
 */

#include "2api.h"
#include "2common.h"
#include "2misc.h"

/**
 * Determine if we are allowed to update auxfw.
 *
 * @param ctx		Vboot2 context
 * @return boolean (true iff we can update auxfw)
 */
static int auxfw_sync_allowed(struct vb2_context *ctx)
{
	struct vb2_gbb_header *gbb = vb2_get_gbb(ctx);

	/* Reasons not to do sync at all */
	if (gbb->flags & VB2_GBB_FLAG_DISABLE_AUXFW_SOFTWARE_SYNC)
		return 0;
	if (ctx->flags & VB2_CONTEXT_RECOVERY_MODE)
		return 0;
	return 1;
}

/**
 * Decides if auxfw sync is allowed to be performed.
 *
 * If sync is allowed, invokes the external callback,
 * vb2ex_auxfw_check() to allow the client to decide on the auxfw
 * update severity.
 *
 * @param ctx		Vboot2 context
 * @return VB2_SUCCESS, or non-zero error code.
 */
static vb2_error_t auxfw_sync_check_update(struct vb2_context *ctx,
					   enum vb2_auxfw_update_severity *severity)
{
	if (!auxfw_sync_allowed(ctx)) {
		*severity = VB2_AUXFW_NO_UPDATE;
		return VB2_SUCCESS;
	}

	return vb2ex_auxfw_check(severity);
}

vb2_error_t vb2api_auxfw_sync(struct vb2_context *ctx)
{
	enum vb2_auxfw_update_severity fw_update = VB2_AUXFW_NO_UPDATE;

	/* Check for update severity */
	VB2_TRY(auxfw_sync_check_update(ctx, &fw_update));

	if (fw_update > VB2_AUXFW_NO_UPDATE) {
		VB2_DEBUG("Updating auxfw\n");
		VB2_TRY(vb2ex_auxfw_update(), ctx, VB2_RECOVERY_AUXFW_UPDATE);
		/*
		 * auxfw update is applied successfully. Request EC reboot to
		 * RO, so that the chips that had FW update get reset to a
		 * clean state.
		 */
		return VB2_REQUEST_REBOOT_EC_TO_RO;
	}

	return vb2ex_auxfw_finalize(ctx);
}
