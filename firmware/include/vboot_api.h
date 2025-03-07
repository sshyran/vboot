/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * APIs provided by firmware to vboot_reference.
 *
 * General notes:
 *
 * All verified boot functions now start with "Vb" for namespace clarity.  This
 * fixes the problem where uboot and vboot both defined assert().
 *
 * Verified boot APIs to be implemented by the calling firmware and exported to
 * vboot_reference start with "VbEx".
 *
 * TODO: split this file into a vboot_entry_points.h file which contains the
 * entry points for the firmware to call vboot_reference, and a
 * vboot_firmware_exports.h which contains the APIs to be implemented by the
 * calling firmware and exported to vboot_reference.
 */

#ifndef VBOOT_REFERENCE_VBOOT_API_H_
#define VBOOT_REFERENCE_VBOOT_API_H_

#include <stdint.h>
#include <stdlib.h>

#include "../2lib/include/2return_codes.h"
#include "gpt.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

struct vb2_context;
typedef struct VbSharedDataHeader VbSharedDataHeader;


/*****************************************************************************/
/* Main entry points from firmware into vboot_reference */

/*
 * We use disk handles rather than indices.  Using indices causes problems if
 * a disk is removed/inserted in the middle of processing.
 */
typedef void *VbExDiskHandle_t;

typedef struct VbSelectAndLoadKernelParams {
	/* Inputs to VbSelectAndLoadKernel() */
	/* Destination buffer for kernel (normally at 0x100000 on x86) */
	void *kernel_buffer;
	/* Size of kernel buffer in bytes */
	uint32_t kernel_buffer_size;

	/*
	 * Outputs from VbSelectAndLoadKernel(); valid only if it returns
	 * success.
	 */
	/* Handle of disk containing loaded kernel */
	VbExDiskHandle_t disk_handle;
	/* Partition number on disk to boot (1...M) */
	uint32_t partition_number;
	/* Address of bootloader image in RAM */
	uint64_t bootloader_address;
	/* Size of bootloader image in bytes */
	uint32_t bootloader_size;
	/* UniquePartitionGuid for boot partition */
	uint8_t partition_guid[16];
	/* Flags set by signer */
	uint32_t flags;
} VbSelectAndLoadKernelParams;

/**
 * Select and loads the kernel.
 *
 * Returns VB2_SUCCESS if success, non-zero if error; on error, caller
 * should reboot. */
vb2_error_t VbSelectAndLoadKernel(struct vb2_context *ctx,
				  VbSelectAndLoadKernelParams *kparams);

/**
 * Attempt loading a kernel from the specified type(s) of disks.
 *
 * If successful, sets kparams.disk_handle to the disk for the kernel and
 * returns VB2_SUCCESS.
 *
 * @param ctx			Vboot context
 * @param disk_flags		Flags to pass to VbExDiskGetInfo()
 * @return VB2_SUCCESS or the most specific VB2_ERROR_LK error.
 */
vb2_error_t VbTryLoadKernel(struct vb2_context *ctx, uint32_t disk_flags);

/* miniOS flags */

/* Boot from non-active miniOS partition only */
#define VB_MINIOS_FLAG_NON_ACTIVE (1 << 0)

/**
 * Attempt loading a miniOS kernel from internal disk.
 *
 * Scans sectors at the start and end of the disk, and looks for miniOS kernels
 * starting at the beginning of the sector.  Attempts loading any miniOS
 * kernels found.
 *
 * If successful, sets lkp.disk_handle to the disk for the kernel and returns
 * VB2_SUCCESS.
 *
 * @param ctx			Vboot context
 * @param minios_flags		Flags for miniOS
 * @return VB2_SUCCESS or the most specific VB2_ERROR_LK error.
 */
vb2_error_t VbTryLoadMiniOsKernel(struct vb2_context *ctx,
				  uint32_t minios_flags);

/*****************************************************************************/
/* Disk access (previously in boot_device.h) */

/* Flags for VbDisk APIs */

/*
 * Disk selection in the lower 16 bits (where the disk lives), and disk
 * attributes in the higher 16 bits (extra information about the disk
 * needed to access it correctly).
 */
#define VB_DISK_FLAG_SELECT_MASK 0xffff
#define VB_DISK_FLAG_ATTRIBUTE_MASK (0xffff << 16)

/* Disk is removable.  Example removable disks: SD cards, USB keys.  */
#define VB_DISK_FLAG_REMOVABLE (1 << 0)
/*
 * Disk is fixed.  If this flag is present, disk is internal to the system and
 * not removable.  Example fixed disks: internal SATA SSD, eMMC.
 */
#define VB_DISK_FLAG_FIXED (1 << 1)
/*
 * Note that VB_DISK_FLAG_REMOVABLE and VB_DISK_FLAG_FIXED are
 * mutually-exclusive for a single disk.  VbExDiskGetInfo() may specify both
 * flags to request disks of both types in a single call.
 *
 * At some point we could specify additional flags, but we don't currently
 * have a way to make use of these:
 *
 * USB              Device is known to be attached to USB.  Note that the SD
 *                  card reader inside x86 systems is attached to USB so this
 *                  isn't super useful.
 * SD               Device is known to be a SD card.  Note that external card
 *                  readers might not return this information, so also of
 *                  questionable use.
 * READ_ONLY        Device is known to be read-only.  Could be used by recovery
 *                  when processing read-only recovery image.
 */

/*
 * Disks are used in two ways:
 * - As a random-access device to read and write the GPT
 * - As a streaming device to read the kernel
 * These are implemented differently on raw NAND vs eMMC/SATA/USB
 * - On eMMC/SATA/USB, both of these refer to the same underlying
 *   storage, so they have the same size and LBA size. In this case,
 *   the GPT should not point to the same address as itself.
 * - On raw NAND, the GPT is held on a portion of the SPI flash.
 *   Random access GPT operations refer to the SPI and streaming
 *   operations refer to NAND. The GPT may therefore point into
 *   the same offsets as itself.
 * These types are distinguished by the following flag and VbDiskInfo
 * has separate fields to describe the random-access ("GPT") and
 * streaming aspects of the disk. If a disk is random-access (i.e.
 * not raw NAND) then these fields are equal.
 */
#define VB_DISK_FLAG_EXTERNAL_GPT (1 << 16)

/* Information on a single disk */
typedef struct VbDiskInfo {
	/* Disk handle */
	VbExDiskHandle_t handle;
	/* Size of a random-access LBA sector in bytes */
	uint64_t bytes_per_lba;
	/* Number of random-access LBA sectors on the device.
	 * If streaming_lba_count is 0, this stands in for the size of the
	 * randomly accessed portion as well as the streaming portion.
	 * Otherwise, this is only the randomly-accessed portion. */
	uint64_t lba_count;
	/* Number of streaming sectors on the device */
	uint64_t streaming_lba_count;
	/* Flags (see VB_DISK_FLAG_* constants) */
	uint32_t flags;
	/*
	 * Optional name string, for use in debugging.  May be empty or null if
	 * not available.
	 */
	const char *name;
} VbDiskInfo;

/**
 * Store information into [info] for all disks (storage devices) attached to
 * the system which match all of the disk_flags.
 *
 * On output, count indicates how many disks are present, and [infos_ptr]
 * points to a [count]-sized array of VbDiskInfo structs with the information
 * on those disks; this pointer must be freed by calling VbExDiskFreeInfo().
 * If count=0, infos_ptr may point to NULL.  If [infos_ptr] points to NULL
 * because count=0 or error, it is not necessary to call VbExDiskFreeInfo().
 *
 * A multi-function device (such as a 4-in-1 card reader) should provide
 * multiple disk handles.
 *
 * The firmware must not alter or free the list pointed to by [infos_ptr] until
 * VbExDiskFreeInfo() is called.
 */
vb2_error_t VbExDiskGetInfo(VbDiskInfo **infos_ptr, uint32_t *count,
			    uint32_t disk_flags);

/**
 * Free a disk information list [infos] previously returned by
 * VbExDiskGetInfo().  If [preserve_handle] != NULL, the firmware must ensure
 * that handle remains valid after this call; all other handles from the info
 * list need not remain valid after this call.
 */
vb2_error_t VbExDiskFreeInfo(VbDiskInfo *infos,
			     VbExDiskHandle_t preserve_handle);

/**
 * Read lba_count LBA sectors, starting at sector lba_start, from the disk,
 * into the buffer.
 *
 * This is used for random access to the GPT. It is not for the partition
 * contents. The upper limit is lba_count.
 *
 * If the disk handle is invalid (for example, the handle refers to a disk
 * which as been removed), the function must return error but must not
 * crash.
 */
vb2_error_t VbExDiskRead(VbExDiskHandle_t handle, uint64_t lba_start,
			 uint64_t lba_count, void *buffer);

/**
 * Write lba_count LBA sectors, starting at sector lba_start, to the disk, from
 * the buffer.
 *
 * This is used for random access to the GPT. It does not (necessarily) access
 * the streaming portion of the device.
 *
 * If the disk handle is invalid (for example, the handle refers to a disk
 * which as been removed), the function must return error but must not
 * crash.
 */
vb2_error_t VbExDiskWrite(VbExDiskHandle_t handle, uint64_t lba_start,
			  uint64_t lba_count, const void *buffer);

/* Streaming read interface */
typedef void *VbExStream_t;

/**
 * Open a stream on a disk
 *
 * @param handle	Disk to open the stream against
 * @param lba_start	Starting sector offset within the disk to stream from
 * @param lba_count	Maximum extent of the stream in sectors
 * @param stream	out-paramter for the generated stream
 *
 * @return Error code, or VB2_SUCCESS.
 *
 * This is used for access to the contents of the actual partitions on the
 * device. It is not used to access the GPT. The size of the content addressed
 * is within streaming_lba_count.
 */
vb2_error_t VbExStreamOpen(VbExDiskHandle_t handle, uint64_t lba_start,
			   uint64_t lba_count, VbExStream_t *stream_ptr);

/**
 * Read from a stream on a disk
 *
 * @param stream	Stream to read from
 * @param bytes		Number of bytes to read
 * @param buffer	Destination to read into
 *
 * @return Error code, or VB2_SUCCESS. Failure to read as much data as
 * requested is an error.
 *
 * This is used for access to the contents of the actual partitions on the
 * device. It is not used to access the GPT.
 */
vb2_error_t VbExStreamRead(VbExStream_t stream, uint32_t bytes, void *buffer);

/**
 * Close a stream
 *
 * @param stream	Stream to close
 */
void VbExStreamClose(VbExStream_t stream);

#ifdef __cplusplus
}
#endif  /* __cplusplus */

#endif  /* VBOOT_REFERENCE_VBOOT_API_H_ */
