/** @file

  Copyright (c) 2013-2014, ARM Ltd. All rights reserved.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD
License
  which accompanies this distribution.  The full text of the license may be
found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2015 - 2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 *  with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#define PAGE_SHIFT 12
#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define HIBERNATE_SIG           "S1SUSPEND"
#define __NEW_UTS_LEN 64
#define ALIGN32_BELOW(addr) ALIGN_POINTER (addr - 32, 32)
#define LOCAL_ROUND_TO_PAGE(x, y) (((x) + (y - 1)) & (~(y - 1)))
#define ROUND_TO_PAGE(x, y) ((ADD_OF ((x), (y))) & (~(y)))
#define ALIGN_PAGES(x, y) (((x) + (y - 1)) / (y))
#define DECOMPRESS_SIZE_FACTOR 8
#define ALIGNMENT_MASK_4KB 4096
#define MAX_NUMBER_OF_LOADED_IMAGES 32
#define PVMFW_CONFIG_MAX_BLOBS 2
/* Return True if integer overflow will occur */
#define CHECK_ADD64(a, b) ((MAX_UINT64 - b < a) ? TRUE : FALSE)
#include "FastbootResponse.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/Debug.h>
#include <Uefi.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/PcdLib.h>
#include <Library/PrintLib.h>
#include <Library/ThreadStack.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include <Guid/EventGroup.h>

#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/EFIUbiFlasher.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextOut.h>

#include "AutoGen.h"
#include "FastbootCmds.h"
#include "FastbootMain.h"
#include "LinuxLoaderLib.h"
#include "MetaFormat.h"
#include "SparseFormat.h"
STATIC struct GetVarPartitionInfo PublishedPartInfo[MAX_NUM_PARTITIONS];

#define BOOT_NAME_SIZE 16
#define BOOT_ARGS_SIZE 512
#define BOOT_EXTRA_ARGS_SIZE 1024
#define BOOT_MAGIC "ANDROID!"
#define BOOT_MAGIC_SIZE 8
struct boot_img_hdr_v0 {
  CHAR8 magic[BOOT_MAGIC_SIZE];

  UINT32 kernel_size; /* size in bytes */
  UINT32 kernel_addr; /* physical load addr */

  UINT32 ramdisk_size; /* size in bytes */
  UINT32 ramdisk_addr; /* physical load addr */

  UINT32 second_size; /* size in bytes */
  UINT32 second_addr; /* physical load addr */

  UINT32 tags_addr;  /* physical addr for kernel tags */
  UINT32 page_size;  /* flash page size we assume */
  UINT32 header_version; /* version for the boot image header */
  UINT32 os_version; /* version << 11 | patch_level */

  UINT8 name[BOOT_NAME_SIZE]; /* asciiz product name */

  UINT8 cmdline[BOOT_ARGS_SIZE];

  UINT32 id[8]; /* timestamp / checksum / sha1 / etc */

  /* Supplemental command line data; kept here to maintain
   * binary compatibility with older versions of mkbootimg
   */
  UINT8 extra_cmdline[BOOT_EXTRA_ARGS_SIZE];
} __attribute__((packed));

/*
 * It is expected that callers would explicitly specify which version of the
 * boot image header they need to use.
 */
typedef struct boot_img_hdr_v0 boot_img_hdr;

STATIC FASTBOOT_VAR *Varlist;
STATIC BOOLEAN Finished = FALSE;
STATIC CHAR8 FullProduct[MAX_RSP_SIZE];
STATIC CHAR8 StrVariant[MAX_RSP_SIZE];
STATIC CHAR8 StrSocVersion[MAX_RSP_SIZE];
STATIC CHAR8 LogicalBlkSizeStr[MAX_RSP_SIZE];
STATIC CHAR8 EraseBlkSizeStr[MAX_RSP_SIZE];
STATIC CHAR8 MaxBufferSizeStr[MAX_RSP_SIZE];


#define MAX_DISPLAY_PANEL_OVERRIDE 256
#define MAX_GPU_CONFIG_OVERRIDE 256

extern struct PartitionEntry PtnEntries[MAX_NUM_PARTITIONS];

STATIC ANDROID_FASTBOOT_STATE mState = ExpectCmdState;
/* When in ExpectDataState, the number of bytes of data to expect: */
STATIC UINT64 mNumDataBytes;
STATIC UINT64 mFlashNumDataBytes;
/* .. and the number of bytes so far received this data phase */
STATIC UINT64 mBytesReceivedSoFar;
/*  and the buffer to save data into */
STATIC UINT8 *mDataBuffer = NULL;
/*  and the offset for usb to save data into */
STATIC UINT8 *mFlashDataBuffer = NULL;
STATIC UINT8 *mUsbDataBuffer = NULL;

STATIC EFI_KERNEL_PROTOCOL  *KernIntf = NULL;
STATIC BOOLEAN IsMultiThreadSupported = FALSE;
STATIC BOOLEAN IsFlashComplete = TRUE;
STATIC LockHandle *LockDownload;
STATIC LockHandle *LockFlash;

STATIC EFI_STATUS FlashResult = EFI_SUCCESS;
#ifdef ENABLE_UPDATE_PARTITIONS_CMDS
STATIC EFI_EVENT UsbTimerEvent;
#endif

STATIC UINT64 MaxUSBBufferSize = 0;

STATIC INT32 Lun = NO_LUN;
STATIC BOOLEAN LunSet;

STATIC FASTBOOT_CMD *cmdlist;

STATIC EFI_STATUS
FastbootCommandSetup (VOID *Base, UINT64 Size);
STATIC VOID
AcceptCmd (IN UINT64 Size, IN CHAR8 *Data);
STATIC VOID
AcceptCmdHandler (IN EFI_EVENT Event, IN VOID *Context);

#define NAND_PAGES_PER_BLOCK 64

#define UBI_HEADER_MAGIC "UBI#"
#define UBI_NUM_IMAGES 1
typedef struct UbiHeader {
  CHAR8 HdrMagic[4];
} UbiHeader_t;

typedef struct {
  UINT64 Size;
  VOID *Data;
} CmdInfo;

typedef struct {
  CHAR16 PartitionName[MAX_GPT_NAME_SIZE];
  UINT32 PartitionSize;
  UINT8 *FlashDataBuffer;
  UINT64 FlashNumDataBytes;
} FlashInfo;

STATIC BOOLEAN FlashSplitNeeded;
STATIC BOOLEAN UsbTimerStarted;

BOOLEAN IsUsbTimerStarted (VOID) {
  return UsbTimerStarted;
}

BOOLEAN IsFlashSplitNeeded (VOID)
{
  if (IsUseMThreadParallel ()) {
    return FlashSplitNeeded;
  } else {
    return UsbTimerStarted;
  }
}

BOOLEAN FlashComplete (VOID)
{
  return IsFlashComplete;
}

#ifdef DISABLE_PARALLEL_DOWNLOAD_FLASH
BOOLEAN IsDisableParallelDownloadFlash (VOID)
{
  return TRUE;
}
#else
BOOLEAN IsDisableParallelDownloadFlash (VOID)
{
  return FALSE;
}
#endif

/* Clean up memory for the getvar and cmdlist variables
 * during exit.
 */
STATIC EFI_STATUS FastbootUnInit (VOID)
{
  FASTBOOT_VAR *Var;
  FASTBOOT_CMD *cmd;

  while (Varlist) {
    Var = Varlist;
    Varlist = Varlist->next;
    FreePool (Var);
  }

  while (cmdlist) {
    cmd = cmdlist;
    cmdlist = cmdlist->next;
    FreePool (cmd);
  }
  return EFI_SUCCESS;
}

/* Publish a variable readable by the built-in getvar command
 * These Variables must not be temporary, shallow copies are used.
 */
STATIC VOID
FastbootPublishVar (IN CONST CHAR8 *Name, IN CONST CHAR8 *Value)
{
  FASTBOOT_VAR *Var;
  Var = AllocateZeroPool (sizeof (*Var));
  if (Var) {
    Var->next = Varlist;
    Varlist = Var;
    Var->name = Name;
    Var->value = Value;
  } else {
    DEBUG ((EFI_D_VERBOSE,
            "Failed to publish a variable readable(%a): malloc error!\n",
            Name));
  }
}

/* Returns the Remaining amount of bytes expected
 * This lets us bypass ZLT issues
 */
UINTN GetXfrSize (VOID)
{
  UINTN BytesLeft = mNumDataBytes - mBytesReceivedSoFar;
  if ((mState == ExpectDataState) && (BytesLeft < USB_BUFFER_SIZE))
    return BytesLeft;

  return USB_BUFFER_SIZE;
}

/* Acknowlege to host, INFO, OKAY and FAILURE */
STATIC VOID
FastbootAck (IN CONST CHAR8 *code, CONST CHAR8 *Reason)
{
  /* gTxBuffer is a USB_BUFF_SIZE allocation, including room for the local
   * terminator after a full 64-byte protocol response. */
  UINTN Length = SfbFastbootEncodeResponse (
      code, Reason, GetFastbootDeviceData ()->gTxBuffer);
  GetFastbootDeviceData ()->UsbDeviceProtocol->Send (
      ENDPOINT_OUT, Length, GetFastbootDeviceData ()->gTxBuffer);
  DEBUG ((EFI_D_VERBOSE, "Sending %d:%a\n",
          AsciiStrLen (GetFastbootDeviceData ()->gTxBuffer),
          GetFastbootDeviceData ()->gTxBuffer));
}

VOID
FastbootFail (IN CONST CHAR8 *Reason)
{
  FastbootAck ("FAIL", Reason);
}

VOID
FastbootInfo (IN CONST CHAR8 *Info)
{
  FastbootAck ("INFO", Info);
}

VOID
FastbootOkay (IN CONST CHAR8 *info)
{
  FastbootAck ("OKAY", info);
}

VOID PartitionDump (VOID)
{
  EFI_STATUS Status;
  EFI_PARTITION_ENTRY *PartEntry;
  UINT16 i;
  UINT32 j;
  /* By default the LunStart and LunEnd would point to '0' and max value */
  UINT32 LunStart = 0;
  UINT32 LunEnd = GetMaxLuns ();

  /* If Lun is set in the Handle flash command then find the block io for that
   * lun */
  if (LunSet) {
    LunStart = Lun;
    LunEnd = Lun + 1;
  }
  for (i = LunStart; i < LunEnd; i++) {
    for (j = 0; j < Ptable[i].MaxHandles; j++) {
      Status =
          gBS->HandleProtocol (Ptable[i].HandleInfoList[j].Handle,
                               &gEfiPartitionRecordGuid, (VOID **)&PartEntry);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_VERBOSE, "Error getting the partition record for Lun %d "
                               "and Handle: %d : %r\n",
                i, j, Status));
        continue;
      }
      DEBUG ((EFI_D_INFO, "Name:[%s] StartLba: %u EndLba:%u\n",
              PartEntry->PartitionName, PartEntry->StartingLBA,
              PartEntry->EndingLBA));
    }
  }
}

EFI_STATUS
PartitionGetInfo (IN CHAR16 *PartitionName,
                  OUT EFI_BLOCK_IO_PROTOCOL **BlockIo,
                  OUT EFI_HANDLE **Handle)
{
  EFI_STATUS Status;
  EFI_PARTITION_ENTRY *PartEntry;
  UINT16 i;
  UINT32 j;
  /* By default the LunStart and LunEnd would point to '0' and max value */
  UINT32 LunStart = 0;
  UINT32 LunEnd = GetMaxLuns ();

  /* If Lun is set in the Handle flash command then find the block io for that
   * lun */
  if (LunSet) {
    LunStart = Lun;
    LunEnd = Lun + 1;
  }
  for (i = LunStart; i < LunEnd; i++) {
    for (j = 0; j < Ptable[i].MaxHandles; j++) {
      Status =
          gBS->HandleProtocol (Ptable[i].HandleInfoList[j].Handle,
                               &gEfiPartitionRecordGuid, (VOID **)&PartEntry);
      if (EFI_ERROR (Status)) {
        continue;
      }
      if (!(StrCmp (PartitionName, PartEntry->PartitionName))) {
        *BlockIo = Ptable[i].HandleInfoList[j].BlkIo;
        *Handle = Ptable[i].HandleInfoList[j].Handle;
        return Status;
      }
    }
  }

  DEBUG ((EFI_D_ERROR, "Partition not found : %s\n", PartitionName));
  return EFI_NOT_FOUND;
}
VOID RebootDevice (UINT8 RebootReason);

#ifdef ENABLE_UPDATE_PARTITIONS_CMDS
/* Helper function to write data to disk */
STATIC EFI_STATUS
WriteToDisk (IN EFI_BLOCK_IO_PROTOCOL *BlockIo,
             IN EFI_HANDLE *Handle,
             IN VOID *Image,
             IN UINT64 Size,
             IN UINT64 offset)
{
  return WriteBlockToPartitionNoFlush (BlockIo, Handle, offset, Size, Image);
}

STATIC EFI_STATUS
HandleChunkTypeRaw (sparse_header_t *sparse_header,
        chunk_header_t *chunk_header,
        VOID **Image,
        SparseImgParam *SparseImgData)
{
  EFI_STATUS Status;

  if (sparse_header == NULL ||
      chunk_header == NULL ||
      *Image == NULL ||
      SparseImgData == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid input Parameters\n"));
    return EFI_INVALID_PARAMETER;
  }

  if ((UINT64)chunk_header->total_sz !=
      ((UINT64)sparse_header->chunk_hdr_sz +
       SparseImgData->ChunkDataSz)) {
    DEBUG ((EFI_D_ERROR, "Bogus chunk size for chunk type Raw\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (CHECK_ADD64 ((UINT64)*Image, SparseImgData->ChunkDataSz)) {
    DEBUG ((EFI_D_ERROR,
            "Integer overflow while adding Image and chunk data sz\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (SparseImgData->ImageEnd < (UINT64)*Image +
      SparseImgData->ChunkDataSz) {
    DEBUG ((EFI_D_ERROR,
            "buffer overreads occured due to invalid sparse header\n"));
    return EFI_INVALID_PARAMETER;
  }

  /* Data is validated, now write to the disk */
  SparseImgData->WrittenBlockCount =
    SparseImgData->TotalBlocks * SparseImgData->BlockCountFactor;
  Status = WriteToDisk (SparseImgData->BlockIo, SparseImgData->Handle,
                        *Image,
                        SparseImgData->ChunkDataSz,
                        SparseImgData->WrittenBlockCount);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Flash Write Failure\n"));
    return Status;
  }

  if (SparseImgData->TotalBlocks >
       (MAX_UINT32 - chunk_header->chunk_sz)) {
    DEBUG ((EFI_D_ERROR, "Bogus size for RAW chunk Type\n"));
    return EFI_INVALID_PARAMETER;
  }

  SparseImgData->TotalBlocks += chunk_header->chunk_sz;
  *Image += SparseImgData->ChunkDataSz;

  return EFI_SUCCESS;
}

STATIC EFI_STATUS
HandleChunkTypeFill (sparse_header_t *sparse_header,
        chunk_header_t *chunk_header,
        VOID **Image,
        SparseImgParam *SparseImgData)
{
  UINT32 *FillBuf = NULL;
  UINT32 FillVal;
  EFI_STATUS Status = EFI_SUCCESS;
  UINT32 Temp;

  if (sparse_header == NULL ||
      chunk_header == NULL ||
      *Image == NULL ||
      SparseImgData == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid input Parameters\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (chunk_header->total_sz !=
     (sparse_header->chunk_hdr_sz + sizeof (UINT32))) {
    DEBUG ((EFI_D_ERROR, "Bogus chunk size for chunk type FILL\n"));
    return EFI_INVALID_PARAMETER;
  }

  FillBuf = AllocateZeroPool (sparse_header->blk_sz);
  if (!FillBuf) {
    DEBUG ((EFI_D_ERROR, "Malloc failed for: CHUNK_TYPE_FILL\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  if (CHECK_ADD64 ((UINT64)*Image, sizeof (UINT32))) {
    DEBUG ((EFI_D_ERROR,
              "Integer overflow while adding Image and uint32\n"));
    Status = EFI_INVALID_PARAMETER;
    goto out;
  }

  if (SparseImgData->ImageEnd < (UINT64)*Image + sizeof (UINT32)) {
    DEBUG ((EFI_D_ERROR,
            "Buffer overread occured due to invalid sparse header\n"));
   Status = EFI_INVALID_PARAMETER;
   goto out;
  }

  FillVal = *(UINT32 *)*Image;
  *Image = (CHAR8 *)*Image + sizeof (UINT32);

  for (Temp = 0;
       Temp < (sparse_header->blk_sz / sizeof (FillVal));
       Temp++) {
    FillBuf[Temp] = FillVal;
  }

  for (Temp = 0; Temp < chunk_header->chunk_sz; Temp++) {
    /* Make sure the data does not exceed the partition size */
    if ((UINT64)SparseImgData->TotalBlocks *
         (UINT64)sparse_header->blk_sz +
         sparse_header->blk_sz >
         SparseImgData->PartitionSize) {
      DEBUG ((EFI_D_ERROR, "Chunk data size for fill type "
                            "exceeds partition size\n"));
      Status = EFI_VOLUME_FULL;
      goto out;
    }

    SparseImgData->WrittenBlockCount =
      SparseImgData->TotalBlocks *
        SparseImgData->BlockCountFactor;
    Status = WriteToDisk (SparseImgData->BlockIo,
                          SparseImgData->Handle,
                          (VOID *)FillBuf,
                          sparse_header->blk_sz,
                          SparseImgData->WrittenBlockCount);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Flash write failure for FILL Chunk\n"));

    goto out;
    }

    SparseImgData->TotalBlocks++;
  }

  out:
    if (FillBuf) {
    FreePool (FillBuf);
    FillBuf = NULL;
    }
    return Status;
}

STATIC EFI_STATUS
ValidateChunkDataAndFlash (sparse_header_t *sparse_header,
             chunk_header_t *chunk_header,
             VOID **Image,
             SparseImgParam *SparseImgData)
{
  EFI_STATUS Status;

  if (sparse_header == NULL ||
      chunk_header == NULL ||
      *Image == NULL ||
      SparseImgData == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid input Parameters\n"));
    return EFI_INVALID_PARAMETER;
  }

  switch (chunk_header->chunk_type) {
    case CHUNK_TYPE_RAW:
    Status = HandleChunkTypeRaw (sparse_header,
                                 chunk_header,
                                 Image,
                                 SparseImgData);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    break;

    case CHUNK_TYPE_FILL:
      Status = HandleChunkTypeFill (sparse_header,
                                    chunk_header,
                                    Image,
                                    SparseImgData);

      if (EFI_ERROR (Status)) {
        return Status;
      }

    break;

    case CHUNK_TYPE_DONT_CARE:
      if (SparseImgData->TotalBlocks >
           (MAX_UINT32 - chunk_header->chunk_sz)) {
        DEBUG ((EFI_D_ERROR, "bogus size for chunk DONT CARE type\n"));
        return EFI_INVALID_PARAMETER;
      }
      SparseImgData->TotalBlocks += chunk_header->chunk_sz;
    break;

    case CHUNK_TYPE_CRC:
      if (chunk_header->total_sz != sparse_header->chunk_hdr_sz) {
        DEBUG ((EFI_D_ERROR, "Bogus chunk size for chunk type CRC\n"));
        return EFI_INVALID_PARAMETER;
      }

      if (SparseImgData->TotalBlocks >
           (MAX_UINT32 - chunk_header->chunk_sz)) {
        DEBUG ((EFI_D_ERROR, "Bogus size for chunk type CRC\n"));
        return EFI_INVALID_PARAMETER;
      }

      SparseImgData->TotalBlocks += chunk_header->chunk_sz;


      if (CHECK_ADD64 ((UINT64)*Image, SparseImgData->ChunkDataSz)) {
        DEBUG ((EFI_D_ERROR,
                "Integer overflow while adding Image and chunk data sz\n"));
        return EFI_INVALID_PARAMETER;
      }

      *Image += (UINT32)SparseImgData->ChunkDataSz;
      if (SparseImgData->ImageEnd < (UINT64)*Image) {
        DEBUG ((EFI_D_ERROR, "buffer overreads occured due to "
                              "invalid sparse header\n"));
        return EFI_INVALID_PARAMETER;
      }
    break;

    default:
      DEBUG ((EFI_D_ERROR, "Unknown chunk type: %x\n",
             chunk_header->chunk_type));
      return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

/* Handle Sparse Image Flashing */
STATIC
EFI_STATUS
HandleSparseImgFlash (IN CHAR16 *PartitionName,
                      IN UINT32 PartitionMaxSize,
                      IN VOID *Image,
                      IN UINT64 sz)
{
  sparse_header_t *sparse_header;
  chunk_header_t *chunk_header;
  EFI_STATUS Status;

  SparseImgParam SparseImgData = {0};

  if (CHECK_ADD64 ((UINT64)Image, sz)) {
    DEBUG ((EFI_D_ERROR, "Integer overflow while adding Image and sz\n"));
    return EFI_INVALID_PARAMETER;
  }

  SparseImgData.ImageEnd = (UINT64)Image + sz;
  /* Caller to ensure that the partition is present in the Partition Table*/
  Status = PartitionGetInfo (PartitionName,
                             &(SparseImgData.BlockIo),
                             &(SparseImgData.Handle));

  if (Status != EFI_SUCCESS)
    return Status;
  if (!SparseImgData.BlockIo) {
    DEBUG ((EFI_D_ERROR, "BlockIo for %a is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }
  if (!SparseImgData.Handle) {
    DEBUG ((EFI_D_ERROR, "EFI handle for %a is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }
  // Check image will fit on device
  SparseImgData.PartitionSize = GetPartitionSize (SparseImgData.BlockIo);
  if (!SparseImgData.PartitionSize) {
    return EFI_BAD_BUFFER_SIZE;
  }

  if (sz < sizeof (sparse_header_t)) {
    DEBUG ((EFI_D_ERROR, "Input image is invalid\n"));
    return EFI_INVALID_PARAMETER;
  }

  sparse_header = (sparse_header_t *)Image;
  if (((UINT64)sparse_header->total_blks * (UINT64)sparse_header->blk_sz) >
      SparseImgData.PartitionSize) {
    DEBUG ((EFI_D_ERROR, "Image is too large for the partition\n"));
    return EFI_VOLUME_FULL;
  }

  Image += sizeof (sparse_header_t);

  if (sparse_header->file_hdr_sz != sizeof (sparse_header_t)) {
    DEBUG ((EFI_D_ERROR, "Sparse header size mismatch\n"));
    return EFI_BAD_BUFFER_SIZE;
  }

  if (!sparse_header->blk_sz) {
    DEBUG ((EFI_D_ERROR, "Invalid block size in the sparse header\n"));
    return EFI_INVALID_PARAMETER;
  }

  if ((sparse_header->blk_sz) % (SparseImgData.BlockIo->Media->BlockSize)) {
    DEBUG ((EFI_D_ERROR, "Unsupported sparse block size %x\n",
            sparse_header->blk_sz));
    return EFI_INVALID_PARAMETER;
  }

  SparseImgData.BlockCountFactor = (sparse_header->blk_sz) /
                                   (SparseImgData.BlockIo->Media->BlockSize);

  DEBUG ((EFI_D_VERBOSE, "=== Sparse Image Header ===\n"));
  DEBUG ((EFI_D_VERBOSE, "magic: 0x%x\n", sparse_header->magic));
  DEBUG (
      (EFI_D_VERBOSE, "major_version: 0x%x\n", sparse_header->major_version));
  DEBUG (
      (EFI_D_VERBOSE, "minor_version: 0x%x\n", sparse_header->minor_version));
  DEBUG ((EFI_D_VERBOSE, "file_hdr_sz: %d\n", sparse_header->file_hdr_sz));
  DEBUG ((EFI_D_VERBOSE, "chunk_hdr_sz: %d\n", sparse_header->chunk_hdr_sz));
  DEBUG ((EFI_D_VERBOSE, "blk_sz: %d\n", sparse_header->blk_sz));
  DEBUG ((EFI_D_VERBOSE, "total_blks: %d\n", sparse_header->total_blks));
  DEBUG ((EFI_D_VERBOSE, "total_chunks: %d\n", sparse_header->total_chunks));

  /* Start processing the chunks */
  for (SparseImgData.Chunk = 0;
       SparseImgData.Chunk < sparse_header->total_chunks;
       SparseImgData.Chunk++) {

    if (((UINT64)SparseImgData.TotalBlocks * (UINT64)sparse_header->blk_sz) >=
        SparseImgData.PartitionSize) {
      DEBUG ((EFI_D_ERROR, "Size of image is too large for the partition\n"));
      return EFI_VOLUME_FULL;
    }

    /* Read and skip over chunk header */
    chunk_header = (chunk_header_t *)Image;

    if (CHECK_ADD64 ((UINT64)Image, sizeof (chunk_header_t))) {
      DEBUG ((EFI_D_ERROR,
              "Integer overflow while adding Image and chunk header\n"));
      return EFI_INVALID_PARAMETER;
    }
    Image += sizeof (chunk_header_t);

    if (SparseImgData.ImageEnd < (UINT64)Image) {
      DEBUG ((EFI_D_ERROR,
              "buffer overreads occured due to invalid sparse header\n"));
      return EFI_BAD_BUFFER_SIZE;
    }

    DEBUG ((EFI_D_VERBOSE, "=== Chunk Header ===\n"));
    DEBUG ((EFI_D_VERBOSE, "chunk_type: 0x%x\n", chunk_header->chunk_type));
    DEBUG ((EFI_D_VERBOSE, "chunk_data_sz: 0x%x\n", chunk_header->chunk_sz));
    DEBUG ((EFI_D_VERBOSE, "total_size: 0x%x\n", chunk_header->total_sz));

    if (sparse_header->chunk_hdr_sz != sizeof (chunk_header_t)) {
      DEBUG ((EFI_D_ERROR, "chunk header size mismatch\n"));
      return EFI_INVALID_PARAMETER;
    }

    SparseImgData.ChunkDataSz = (UINT64)sparse_header->blk_sz *
                                 chunk_header->chunk_sz;
    /* Make sure that chunk size calculate from sparse image does not exceed the
     * partition size
     */
    if ((UINT64)SparseImgData.TotalBlocks *
        (UINT64)sparse_header->blk_sz +
        SparseImgData.ChunkDataSz >
        SparseImgData.PartitionSize) {
      DEBUG ((EFI_D_ERROR, "Chunk data size exceeds partition size\n"));
      return EFI_VOLUME_FULL;
    }

    Status = ValidateChunkDataAndFlash (sparse_header,
                                        chunk_header,
                                        &Image,
                                        &SparseImgData);

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  DEBUG ((EFI_D_INFO, "Wrote %d blocks, expected to write %d blocks\n",
            SparseImgData.TotalBlocks, sparse_header->total_blks));

  if (SparseImgData.TotalBlocks != sparse_header->total_blks) {
    DEBUG ((EFI_D_ERROR, "Sparse Image Write Failure\n"));
    Status = EFI_VOLUME_CORRUPTED;
  } else if (((SparseImgData.BlockIo)->FlushBlocks (SparseImgData.BlockIo))
               != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Sparse Image Flush Failure\n"));
    Status = EFI_DEVICE_ERROR;
  }
  return Status;
}

#ifdef NAND_UBI_VOLUME_FLASHING_ENABLED
/* UBI Volume flashing */
STATIC
EFI_STATUS
HandleUbiVolFlash (
  IN CHAR16  *VolumeName,
  IN UINT32 VolumeMaxSize,
  IN VOID   *Image,
  IN UINT64   Size)
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINT32 UbiPageSize;
  UINT32 UbiBlockSize;
  EFI_UBI_FLASHER_PROTOCOL *Ubi;
  UBI_FLASHER_HANDLE UbiFlasherHandle;
  CHAR8 VolumeNameAscii[MAX_GPT_NAME_SIZE] = {'\0'};

  Status = gBS->LocateProtocol (&gEfiUbiFlasherProtocolGuid,
                                NULL,
                                (VOID **) &Ubi);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "UBI Volume flashing not supported\n"));
    return Status;
  }

  UnicodeStrToAsciiStr (VolumeName, VolumeNameAscii);
  Status = Ubi->UbiFlasherOpen (VolumeNameAscii,
                                &UbiFlasherHandle,
                                &UbiPageSize,
                                &UbiBlockSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to open UBI volume\n"));
    return Status;
  }

  /* Note: sparse image is not supported for ubi volume flashing */
  Status = Ubi->UbiFlasherWrite (UbiFlasherHandle, 1, Image, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to write UBI volume\n"));
  }

  Status = Ubi->UbiFlasherClose (UbiFlasherHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to close UBI volume\n"));
    return Status;
  }

  return Status;
}
#endif

/* Raw Image flashing */
STATIC
EFI_STATUS
HandleRawImgFlash (IN CHAR16 *PartitionName,
                   IN UINT32 PartitionMaxSize,
                   IN VOID *Image,
                   IN UINT64 Size)
{
  EFI_STATUS Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  UINT64 PartitionSize;
  EFI_HANDLE *Handle = NULL;

  Status = PartitionGetInfo (PartitionName, &BlockIo, &Handle);
  if (Status != EFI_SUCCESS) {
#ifdef NAND_UBI_VOLUME_FLASHING_ENABLED
    DEBUG ((EFI_D_ERROR, "[%s] Partition Not Found - trying volume\n",
            PartitionName));
    Status = HandleUbiVolFlash (PartitionName,
            ARRAY_SIZE (PartitionName), Image, Size);
#endif
    return Status;
  }
  if (!BlockIo) {
    DEBUG ((EFI_D_ERROR, "BlockIo for %a is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }
  if (!Handle) {
    DEBUG ((EFI_D_ERROR, "EFI handle for %a is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }

  /* Check image will fit on device */
  PartitionSize = GetPartitionSize (BlockIo);
  if (PartitionSize < Size ||
      !PartitionSize) {
    DEBUG ((EFI_D_ERROR, "Partition not big enough.\n"));
    DEBUG ((EFI_D_ERROR, "Partition Size:\t%d\nImage Size:\t%d\n",
            PartitionSize, Size));

    return EFI_VOLUME_FULL;
  }

  Status = WriteBlockToPartition (BlockIo, Handle, 0, Size, Image);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Writing Block to partition Failure\n"));
  }

  return Status;
}

/* UBI Image flashing */
STATIC
EFI_STATUS
HandleUbiImgFlash (
  IN CHAR16  *PartitionName,
  IN UINT32 PartitionMaxSize,
  IN VOID   *Image,
  IN UINT64   Size)
{
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  UINT32 UbiPageSize;
  UINT32 UbiBlockSize;
  EFI_UBI_FLASHER_PROTOCOL *Ubi;
  UBI_FLASHER_HANDLE UbiFlasherHandle;
  EFI_HANDLE *Handle = NULL;
  CHAR8 PartitionNameAscii[MAX_GPT_NAME_SIZE] = {'\0'};
  UINT64 PartitionSize = 0;

  Status = PartitionGetInfo (PartitionName, &BlockIo, &Handle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Unable to get Parition Info\n"));
    return Status;
  }

  /* Check if Image fits into partition */
  PartitionSize = GetPartitionSize (BlockIo);
  if (Size > PartitionSize ||
    !PartitionSize) {
    DEBUG ((EFI_D_ERROR, "Input Size is invalid\n"));
    return EFI_INVALID_PARAMETER;
  }

  Status = gBS->LocateProtocol (&gEfiUbiFlasherProtocolGuid,
                                NULL,
                                (VOID **) &Ubi);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "UBI Image flashing not supported.\n"));
    return Status;
  }

  UnicodeStrToAsciiStr (PartitionName, PartitionNameAscii);
  Status = Ubi->UbiFlasherOpen (PartitionNameAscii,
                                &UbiFlasherHandle,
                                &UbiPageSize,
                                &UbiBlockSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Unable to open UBI Protocol.\n"));
    return Status;
  }

  /* UBI_NUM_IMAGES can replace with number of sparse images being flashed. */
  Status = Ubi->UbiFlasherWrite (UbiFlasherHandle, UBI_NUM_IMAGES, Image, Size);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Unable to open UBI Protocol.\n"));
    return Status;
  }

  Status = Ubi->UbiFlasherClose (UbiFlasherHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Unable to close UBI Protocol.\n"));
    return Status;
  }

  return Status;
}

/* Meta Image flashing */
STATIC
EFI_STATUS
HandleMetaImgFlash (IN CHAR16 *PartitionName,
                    IN UINT32 PartitionMaxSize,
                    IN VOID *Image,
                    IN UINT64 Size)
{
  UINT32 i;
  UINT32 images;
  EFI_STATUS Status = EFI_DEVICE_ERROR;
  img_header_entry_t *img_header_entry;
  meta_header_t *meta_header;
  CHAR16 PartitionNameFromMeta[MAX_GPT_NAME_SIZE];
  UINT64 ImageEnd = 0;
  BOOLEAN PnameTerminated = FALSE;
  UINT32 j;

  if (Size < sizeof (meta_header_t)) {
    DEBUG ((EFI_D_ERROR,
            "Error: The size is smaller than the image header size\n"));
    return EFI_INVALID_PARAMETER;
  }

  meta_header = (meta_header_t *)Image;
  img_header_entry = (img_header_entry_t *)(Image + sizeof (meta_header_t));
  images = meta_header->img_hdr_sz / sizeof (img_header_entry_t);
  if (images > MAX_IMAGES_IN_METAIMG) {
    DEBUG (
        (EFI_D_ERROR,
         "Error: Number of images(%u)in meta_image are greater than expected\n",
         images));
    return EFI_INVALID_PARAMETER;
  }

  if (Size <= (sizeof (meta_header_t) + meta_header->img_hdr_sz)) {
    DEBUG (
        (EFI_D_ERROR,
         "Error: The size is smaller than image header size + entry size\n"));
    return EFI_INVALID_PARAMETER;
  }

  if (CHECK_ADD64 ((UINT64)Image, Size)) {
    DEBUG ((EFI_D_ERROR, "Integer overflow detected in %d, %a\n", __LINE__,
            __FUNCTION__));
    return EFI_BAD_BUFFER_SIZE;
  }
  ImageEnd = (UINT64)Image + Size;

  for (i = 0; i < images; i++) {
    PnameTerminated = FALSE;

    if (!img_header_entry[i].ptn_name[0] ||
        img_header_entry[i].start_offset == 0 || img_header_entry[i].size == 0)
      break;

    if (CHECK_ADD64 ((UINT64)Image, img_header_entry[i].start_offset)) {
      DEBUG ((EFI_D_ERROR, "Integer overflow detected in %d, %a\n", __LINE__,
              __FUNCTION__));
      return EFI_BAD_BUFFER_SIZE;
    }
    if (CHECK_ADD64 ((UINT64) (Image + img_header_entry[i].start_offset),
                     img_header_entry[i].size)) {
      DEBUG ((EFI_D_ERROR, "Integer overflow detected in %d, %a\n", __LINE__,
              __FUNCTION__));
      return EFI_BAD_BUFFER_SIZE;
    }
    if (ImageEnd < ((UINT64)Image + img_header_entry[i].start_offset +
                    img_header_entry[i].size)) {
      DEBUG ((EFI_D_ERROR, "Image size mismatch\n"));
      return EFI_INVALID_PARAMETER;
    }

    for (j = 0; j < MAX_GPT_NAME_SIZE; j++) {
      if (!(img_header_entry[i].ptn_name[j])) {
        PnameTerminated = TRUE;
        break;
      }
    }
    if (!PnameTerminated) {
      DEBUG ((EFI_D_ERROR, "ptn_name string not terminated properly\n"));
      return EFI_INVALID_PARAMETER;
    }
    AsciiStrToUnicodeStr (img_header_entry[i].ptn_name, PartitionNameFromMeta);

    Status = HandleRawImgFlash (
        PartitionNameFromMeta, ARRAY_SIZE (PartitionNameFromMeta),
        (void *)Image + img_header_entry[i].start_offset,
        img_header_entry[i].size);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Meta Image Write Failure\n"));
      return Status;
    }
  }
  return Status;
}

/* Erase partition */
STATIC EFI_STATUS
FastbootErasePartition (IN CHAR16 *PartitionName)
{
  EFI_STATUS Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE *Handle = NULL;

  Status = PartitionGetInfo (PartitionName, &BlockIo, &Handle);
  if (Status != EFI_SUCCESS)
    return Status;
  if (!BlockIo) {
    DEBUG ((EFI_D_ERROR, "BlockIo for %s is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }
  if (!Handle) {
    DEBUG ((EFI_D_ERROR, "EFI handle for %s is corrupted\n", PartitionName));
    return EFI_VOLUME_CORRUPTED;
  }

  Status = ErasePartition (BlockIo, Handle);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Partition Erase failed: %r\n", Status));
    return Status;
  }

  return Status;
}

INT32 __attribute__ ( (no_sanitize ("safe-stack")))
SparseImgFlashThread (VOID* Arg)
{
  Thread* CurrentThread = KernIntf->Thread->GetCurrentThread ();
  FlashInfo* ThreadFlashInfo = (FlashInfo*) Arg;

  if (!ThreadFlashInfo || !ThreadFlashInfo->FlashDataBuffer) {
    return 0;
  }

  KernIntf->Lock->AcquireLock (LockFlash);
  IsFlashComplete = FALSE;
  FlashSplitNeeded = TRUE;

  HandleSparseImgFlash (ThreadFlashInfo->PartitionName,
          ThreadFlashInfo->PartitionSize,
          ThreadFlashInfo->FlashDataBuffer,
          ThreadFlashInfo->FlashNumDataBytes);

  FlashSplitNeeded = FALSE;
  IsFlashComplete = TRUE;
  KernIntf->Lock->ReleaseLock (LockFlash);

  ThreadStackNodeRemove (CurrentThread);

  FreePool (ThreadFlashInfo);
  ThreadFlashInfo = NULL;

  KernIntf->Thread->ThreadExit (0);

  return 0;
}

EFI_STATUS CreateSparseImgFlashThread (IN FlashInfo* ThreadFlashInfo)
{
  EFI_STATUS Status = EFI_SUCCESS;
  Thread* SparseImgFlashTD = NULL;

  SparseImgFlashTD = KernIntf->Thread->ThreadCreate ("SparseImgFlashThread",
      SparseImgFlashThread, (VOID*)ThreadFlashInfo, UEFI_THREAD_PRIORITY,
      DEFAULT_STACK_SIZE);

  if (SparseImgFlashTD == NULL) {
    return EFI_NOT_READY;
  }

  AllocateUnSafeStackPtr (SparseImgFlashTD);

  Status = KernIntf->Thread->ThreadResume (SparseImgFlashTD);
  return Status;
}

#endif

/* Handle Download Command */
STATIC VOID
CmdDownload (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  CHAR8 Response[13] = "DATA";
  UINT32 InitStrLen = AsciiStrLen ("DATA");

  CHAR16 OutputString[FASTBOOT_STRING_MAX_LENGTH];
  CHAR8 *NumBytesString = (CHAR8 *)arg;

  /* Argument is 8-character ASCII string hex representation of number of
   * bytes that will be sent in the data phase.Response is "DATA" + that same
   * 8-character string.
   */

  if (AsciiStrLen (NumBytesString) > ASCII_HEX_STRING_MAX_LENGTH ) {
    DEBUG ((EFI_D_ERROR, "ERROR: Invalid argument size\n"));
    FastbootFail (" Invalid argument size");
    return;
  }

  // Parse out number of data bytes to expect
  mNumDataBytes = AsciiStrHexToUint64 (NumBytesString);
  if (mNumDataBytes == 0) {
    DEBUG (
        (EFI_D_ERROR, "ERROR: Fail to get the number of bytes to download.\n"));
    FastbootFail ("Failed to get the number of bytes to download");
    return;
  }

  if (mNumDataBytes > MaxUSBBufferSize) {
    DEBUG ((EFI_D_ERROR,
            "ERROR: Data size (%d) is more than max download size (%d)\n",
            mNumDataBytes, MaxUSBBufferSize));
    FastbootFail ("Requested download size is more than max allowed\n");
    return;
  }

  UnicodeSPrint (OutputString, sizeof (OutputString),
                 (CONST CHAR16 *)L"Downloading %d bytes\r\n", mNumDataBytes);

  /* NumBytesString is a 8 bit string, InitStrLen is 4, and the AsciiStrnCpyS()
   * require "DestMax > SourceLen", so init length of Response as 13.
   */
  AsciiStrnCpyS (Response + InitStrLen, sizeof (Response) - InitStrLen,
                 NumBytesString, AsciiStrLen (NumBytesString));

  gBS->CopyMem (GetFastbootDeviceData ()->gTxBuffer, Response,
                sizeof (Response));

  if (IsUseMThreadParallel ()) {
    KernIntf->Lock->AcquireLock (LockDownload);
  }

  mState = ExpectDataState;
  mBytesReceivedSoFar = 0;
  GetFastbootDeviceData ()->UsbDeviceProtocol->Send (
      ENDPOINT_OUT, sizeof (Response), GetFastbootDeviceData ()->gTxBuffer);
  DEBUG ((EFI_D_VERBOSE, "CmdDownload: Send 12 %a\n",
          GetFastbootDeviceData ()->gTxBuffer));
}
STATIC VOID WaitForTransferComplete (VOID)
{
  USB_DEVICE_EVENT Msg;
  USB_DEVICE_EVENT_DATA Payload;
  UINTN PayloadSize;

  /* Wait for the transfer to complete */
  while (1) {
    GetFastbootDeviceData ()->UsbDeviceProtocol->HandleEvent (&Msg,
            &PayloadSize, &Payload);
    if (UsbDeviceEventTransferNotification == Msg) {
      if (1 == USB_INDEX_TO_EP (Payload.TransferOutcome.EndpointIndex)) {
        if (USB_ENDPOINT_DIRECTION_IN ==
            USB_INDEX_TO_EPDIR (Payload.TransferOutcome.EndpointIndex))
          break;
      }
    }
  }
}
#ifdef ENABLE_UPDATE_PARTITIONS_CMDS
STATIC EFI_STATUS
FetchParseHex (IN CONST CHAR8 *Token, OUT UINT64 *Value)
{
  CONST CHAR8 *Cursor = Token;

  if (Cursor[0] == '0' && (Cursor[1] == 'x' || Cursor[1] == 'X'))
    Cursor += 2;
  if (*Cursor == '\0')
    return EFI_INVALID_PARAMETER;

  for (; *Cursor; Cursor++) {
    if (!((*Cursor >= '0' && *Cursor <= '9') ||
          (*Cursor >= 'a' && *Cursor <= 'f') ||
          (*Cursor >= 'A' && *Cursor <= 'F')))
      return EFI_INVALID_PARAMETER;
  }

  *Value = AsciiStrHexToUint64 (Token);
  return EFI_SUCCESS;
}

/* fetch:<partition>[:<offset>[:<size>]]
 *
 * Reads [offset, offset + size) from the named partition and streams it back
 * to the host. offset and size are hex; when omitted offset defaults to 0 and
 * size to the rest of the partition. This is read-only - nothing on the device
 * is modified. The host upload phase (DATA + payload + OKAY) mirrors the
 * synchronous send used by CmdGetVarAll.
 */
STATIC VOID
CmdFetch (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  EFI_STATUS Status;
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE *Handle = NULL;
  CHAR8 FetchArg[MAX_FASTBOOT_COMMAND_SIZE];
  CHAR16 PartitionName[MAX_GPT_NAME_SIZE];
  CHAR8 *OffsetStr;
  CHAR8 *SizeStr;
  UINT64 ByteOffset = 0;
  UINT64 RequestSize = 0;
  UINT64 PartitionSize;
  UINT64 BlockSize;
  UINT64 MaxChunk;
  UINT64 CurrentOffset;
  UINT64 RemainingBytes;
  UINT64 BlockOffset;
  UINT64 SendBytes;
  UINT64 ReadBytes;
  VOID *TxBuffer;
  BOOLEAN SizeGiven = FALSE;

  /* A flash queued by parallel download may still be running; finish it so the
   * data we read back is consistent, and clear any LUN filter left by flash. */
  WaitForFlashFinished ();
  LunSet = FALSE;

  if (arg == NULL || AsciiStrLen (arg) == 0 ||
      AsciiStrLen (arg) >= sizeof (FetchArg)) {
    FastbootFail ("Invalid fetch argument");
    return;
  }
  AsciiStrnCpyS (FetchArg, sizeof (FetchArg), arg, AsciiStrLen (arg));

  /* Split "partition[:offset[:size]]". Qualcomm partition names never contain
   * ':', so a left-to-right split is unambiguous. */
  OffsetStr = AsciiStrStr (FetchArg, ":");
  if (OffsetStr != NULL) {
    *OffsetStr++ = '\0';
    SizeStr = AsciiStrStr (OffsetStr, ":");
    if (SizeStr != NULL)
      *SizeStr++ = '\0';

    if (EFI_ERROR (FetchParseHex (OffsetStr, &ByteOffset))) {
      FastbootFail ("Invalid fetch offset");
      return;
    }
    if (SizeStr != NULL) {
      if (EFI_ERROR (FetchParseHex (SizeStr, &RequestSize))) {
        FastbootFail ("Invalid fetch size");
        return;
      }
      SizeGiven = TRUE;
    }
  }

  if (AsciiStrLen (FetchArg) == 0 ||
      AsciiStrLen (FetchArg) >= MAX_GPT_NAME_SIZE) {
    FastbootFail ("Invalid partition name");
    return;
  }
  AsciiStrToUnicodeStr (FetchArg, PartitionName);

  Status = PartitionGetInfo (PartitionName, &BlockIo, &Handle);
  if (Status != EFI_SUCCESS) {
    FastbootFail ("Partition not found");
    return;
  }
  if (BlockIo == NULL || BlockIo->Media == NULL) {
    FastbootFail ("Partition is corrupted");
    return;
  }

  PartitionSize = GetPartitionSize (BlockIo);
  BlockSize = BlockIo->Media->BlockSize;
  if (PartitionSize == 0 || BlockSize == 0 || BlockSize > USB_BUFFER_SIZE) {
    FastbootFail ("Invalid partition geometry");
    return;
  }

  if (ByteOffset > PartitionSize) {
    FastbootFail ("Fetch offset exceeds partition");
    return;
  }
  if (!SizeGiven) {
    RequestSize = PartitionSize - ByteOffset;
  } else if (CHECK_ADD64 (ByteOffset, RequestSize) ||
             ByteOffset + RequestSize > PartitionSize) {
    FastbootFail ("Fetch range exceeds partition");
    return;
  }
  /* The DATA phase carries an 8-digit (32-bit) length, so a single fetch can
   * return at most MAX_UINT32 bytes; larger reads must be split by offset. */
  if (RequestSize > MaxUSBBufferSize) {
    DEBUG ((EFI_D_ERROR,
            "ERROR: Data size (%d) is more than max fetch size (%d)\n",
            RequestSize, MaxUSBBufferSize));
    FastbootFail ("Requested fetch size is more than max allowed\n");
    return;
  }

  /* Largest block-aligned read that fits the transmit buffer. */
  MaxChunk = USB_BUFFER_SIZE - (USB_BUFFER_SIZE % BlockSize);

  TxBuffer = GetFastbootDeviceData ()->gTxBuffer;
  AsciiSPrint (TxBuffer, MAX_RSP_SIZE, "DATA%08x", (UINT32)RequestSize);
  Status = GetFastbootDeviceData ()->UsbDeviceProtocol->Send (
      ENDPOINT_OUT, AsciiStrLen (TxBuffer), TxBuffer);
  if (Status != EFI_SUCCESS) {
    FastbootFail ("Failed to send fetch response");
    return;
  }
  WaitForTransferComplete ();

  CurrentOffset = ByteOffset;
  RemainingBytes = RequestSize;
  while (RemainingBytes) {
    /* Read the block-aligned window covering the next slice into TxBuffer,
     * then send only the requested bytes starting at the in-block offset. */
    BlockOffset = CurrentOffset % BlockSize;
    SendBytes = RemainingBytes;
    if (SendBytes > MaxChunk - BlockOffset)
      SendBytes = MaxChunk - BlockOffset;

    ReadBytes = BlockOffset + SendBytes;
    if (ReadBytes % BlockSize)
      ReadBytes += BlockSize - (ReadBytes % BlockSize);

    Status = BlockIo->ReadBlocks (BlockIo, BlockIo->Media->MediaId,
                                  CurrentOffset / BlockSize,
                                  (UINTN)ReadBytes, TxBuffer);
    if (Status != EFI_SUCCESS) {
      FastbootFail ("Failed to read partition");
      return;
    }

    Status = GetFastbootDeviceData ()->UsbDeviceProtocol->Send (
        ENDPOINT_OUT, (UINTN)SendBytes, (UINT8 *)TxBuffer + BlockOffset);
    if (Status != EFI_SUCCESS) {
      FastbootFail ("Failed to send fetch data");
      return;
    }
    WaitForTransferComplete ();

    CurrentOffset += SendBytes;
    RemainingBytes -= SendBytes;
  }

  FastbootOkay ("");
}
/*  Function needed for event notification callback */
STATIC VOID
BlockIoCallback (IN EFI_EVENT Event, IN VOID *Context)
{
}

STATIC VOID
UsbTimerHandler (IN EFI_EVENT Event, IN VOID *Context)
{
  HandleUsbEvents ();
  if (FastbootFatal ())
    DEBUG ((EFI_D_ERROR, "Continue detected, Exiting App...\n"));
}

STATIC EFI_STATUS
HandleUsbEventsInTimer ()
{
  EFI_STATUS Status = EFI_SUCCESS;

  if (UsbTimerEvent)
    return Status;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             UsbTimerHandler, NULL, &UsbTimerEvent);

  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (UsbTimerEvent, TimerPeriodic, 100000);
  }

  return Status;
}

STATIC VOID StopUsbTimer (VOID)
{
  if (UsbTimerEvent) {
    gBS->SetTimer (UsbTimerEvent, TimerCancel, 0);
    gBS->CloseEvent (UsbTimerEvent);
    UsbTimerEvent = NULL;
  }

  UsbTimerStarted = FALSE;
}
#else
STATIC VOID StopUsbTimer (VOID)
{
  return;
}
#endif

#ifdef ENABLE_UPDATE_PARTITIONS_CMDS

STATIC VOID ExchangeFlashAndUsbDataBuf (VOID)
{
  VOID *mTmpbuff;

  if (IsUseMThreadParallel ()) {
    KernIntf->Lock->AcquireLock (LockDownload);
    KernIntf->Lock->AcquireLock (LockFlash);
  }

  mTmpbuff = mUsbDataBuffer;
  mUsbDataBuffer = mFlashDataBuffer;
  mFlashDataBuffer = mTmpbuff;
  mFlashNumDataBytes = mNumDataBytes;

  if (IsUseMThreadParallel ()) {
    KernIntf->Lock->ReleaseLock (LockFlash);
    KernIntf->Lock->ReleaseLock (LockDownload);
  }
}

STATIC EFI_STATUS
ReenumeratePartTable (VOID)
{
  EFI_STATUS Status;
  LunSet = FALSE;
  EFI_EVENT gBlockIoRefreshEvt;

  Status =
    gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, BlockIoCallback,
                        NULL, &gBlockIoRefreshGuid, &gBlockIoRefreshEvt);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error Creating event for Block Io refresh:%x\n",
            Status));
    return Status;
  }

  Status = gBS->SignalEvent (gBlockIoRefreshEvt);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error Signalling event for Block Io refresh:%x\n",
            Status));
    return Status;
  }
  Status = EnumeratePartitions ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Enumeration of partitions failed\n"));
    return Status;
  }
  UpdatePartitionEntries ();

  DEBUG ((EFI_D_INFO, "*************** New partition Table Dump Start "
                      "*******************\n"));
  PartitionDump ();
  DEBUG ((EFI_D_INFO, "*************** New partition Table Dump End   "
                      "*******************\n"));
  return Status;
}


/* Handle Flash Command */
STATIC VOID
CmdFlash (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  EFI_STATUS Status = EFI_SUCCESS;
  sparse_header_t *sparse_header;
  meta_header_t *meta_header;
  UbiHeader_t *UbiHeader;
  CHAR16 PartitionName[MAX_GPT_NAME_SIZE];
  CHAR16 *Token = NULL;
  LunSet = FALSE;
  UINT32 UfsBootLun = 0;
  CHAR8 BootDeviceType[BOOT_DEV_NAME_SIZE_MAX];
  /* For partition info */
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE *Handle = NULL;
  CHAR8 FlashResultStr[MAX_RSP_SIZE] = "";
  UINT64 PartitionSize = 0;
  UINT32 Ret;

  ExchangeFlashAndUsbDataBuf ();
  if (mFlashDataBuffer == NULL) {
    // Doesn't look like we were sent any data
    FastbootFail ("No data to flash");
    return;
  }

  if (AsciiStrLen (arg) >= MAX_GPT_NAME_SIZE) {
    FastbootFail ("Invalid partition name");
    return;
  }
  AsciiStrToUnicodeStr (arg, PartitionName);

 
  /* Handle virtual partition avb_custom_key */

  /* Find the lun number from input string */
  Token = StrStr (PartitionName, L":");

  if (Token) {
    /* Skip past ":" to the lun number */
    Token++;
    Lun = StrDecimalToUintn (Token);

    if (Lun >= MAX_LUNS) {
      FastbootFail ("Invalid Lun number passed\n");
      goto out;
    }

    LunSet = TRUE;
  }

  GetRootDeviceType (BootDeviceType, BOOT_DEV_NAME_SIZE_MAX);

  if ((!StrnCmp (PartitionName, L"partition", StrLen (L"partition"))) ||
       ((!StrnCmp (PartitionName, L"mibib", StrLen (L"mibib"))) &&
       (!AsciiStrnCmp (BootDeviceType, "NAND", AsciiStrLen ("NAND"))))) {
    if (!AsciiStrnCmp (BootDeviceType, "UFS", AsciiStrLen ("UFS"))) {
      UfsGetSetBootLun (&UfsBootLun, TRUE); /* True = Get */
      if (UfsBootLun != 0x1) {
        UfsBootLun = 0x1;
        UfsGetSetBootLun (&UfsBootLun, FALSE); /* False = Set */
      }
    } else if (!AsciiStrnCmp (BootDeviceType, "EMMC", AsciiStrLen ("EMMC")) ||
               !AsciiStrnCmp (BootDeviceType, "NVME", AsciiStrLen ("NVME"))) {
      Lun = NO_LUN;
      LunSet = FALSE;
    }
    DEBUG ((EFI_D_INFO, "Attemping to update partition table\n"));
    DEBUG ((EFI_D_INFO, "*************** Current partition Table Dump Start "
                        "*******************\n"));
    PartitionDump ();
    DEBUG ((EFI_D_INFO, "*************** Current partition Table Dump End   "
                        "*******************\n"));
    if (!AsciiStrnCmp (BootDeviceType, "NAND", AsciiStrLen ("NAND"))) {
      Ret = PartitionVerifyMibibImage (mFlashDataBuffer);
      if (Ret) {
        FastbootFail ("Error Updating partition Table\n");
        goto out;
      }
      Status = HandleRawImgFlash (PartitionName,
                        ARRAY_SIZE (PartitionName),
                        mFlashDataBuffer, mFlashNumDataBytes);
    }
    else {
      Status = UpdatePartitionTable (mFlashDataBuffer, mFlashNumDataBytes,
                        Lun, Ptable);
    }
    /* Signal the Block IO to update and reenumerate the parition table */
    if (Status == EFI_SUCCESS)  {
      Status = ReenumeratePartTable ();
      if (Status == EFI_SUCCESS) {
        FastbootOkay ("");
        goto out;
      }
    }
    FastbootFail ("Error Updating partition Table\n");
    goto out;
  }

  sparse_header = (sparse_header_t *)mFlashDataBuffer;
  meta_header = (meta_header_t *)mFlashDataBuffer;
  UbiHeader = (UbiHeader_t *)mFlashDataBuffer;

  /* Send okay for next data sending */
  if (sparse_header->magic == SPARSE_HEADER_MAGIC) {
    Status = PartitionGetInfo (PartitionName, &BlockIo, &Handle);
    if (EFI_ERROR (Status)) {
      FastbootFail ("Partition not found");
      goto out;
    }

    PartitionSize = GetPartitionSize (BlockIo);
    if (!PartitionSize) {
      FastbootFail ("Partition error size");
      goto out;
    }

    if ((PartitionSize > MaxUSBBufferSize) &&
         !IsDisableParallelDownloadFlash ()) {
      if (IsUseMThreadParallel ()) {
        FlashInfo* ThreadFlashInfo = AllocateZeroPool (sizeof (FlashInfo));
        if (!ThreadFlashInfo) {
          DEBUG ((EFI_D_ERROR,
                  "ERROR: Failed to allocate memory for ThreadFlashInfo\n"));
          return ;
        }

        ThreadFlashInfo->FlashDataBuffer = mFlashDataBuffer,
        ThreadFlashInfo->FlashNumDataBytes = mFlashNumDataBytes;

        StrnCpyS (ThreadFlashInfo->PartitionName, MAX_GPT_NAME_SIZE,
                PartitionName, ARRAY_SIZE (PartitionName));
        ThreadFlashInfo->PartitionSize = ARRAY_SIZE (PartitionName);

        Status = CreateSparseImgFlashThread (ThreadFlashInfo);
      } else {
        IsFlashComplete = FALSE;

        Status = HandleUsbEventsInTimer ();
        if (EFI_ERROR (Status)) {
          DEBUG ((EFI_D_ERROR, "Failed to handle usb event: %r\n", Status));
          IsFlashComplete = TRUE;
          StopUsbTimer ();
        } else {
          UsbTimerStarted = TRUE;
        }
      }

      if (!EFI_ERROR (Status)) {
        FastbootOkay ("");
      }
    }

    if (EFI_ERROR (Status) ||
      !IsUseMThreadParallel () ||
      (PartitionSize <= MaxUSBBufferSize)) {
      FlashResult = HandleSparseImgFlash (PartitionName,
                                        ARRAY_SIZE (PartitionName),
                                        mFlashDataBuffer, mFlashNumDataBytes);
      IsFlashComplete = TRUE;
      StopUsbTimer ();
    }
  } else if (!AsciiStrnCmp (UbiHeader->HdrMagic, UBI_HEADER_MAGIC, 4)) {
    FlashResult = HandleUbiImgFlash (PartitionName,
                                     ARRAY_SIZE (PartitionName),
                                     mFlashDataBuffer,
                                     mFlashNumDataBytes);
  } else if (meta_header->magic == META_HEADER_MAGIC) {

    FlashResult = HandleMetaImgFlash (PartitionName,
                                      ARRAY_SIZE (PartitionName),
                                      mFlashDataBuffer, mFlashNumDataBytes);
  } else {

    FlashResult = HandleRawImgFlash (PartitionName,
                                     ARRAY_SIZE (PartitionName),
                                     mFlashDataBuffer, mFlashNumDataBytes);
  }

  /*
   * For Non-sparse image: Check flash result and update the result
   * Also, Handle if there is Failure in handling USB events especially for
   * sparse images.
   */
  if ((sparse_header->magic != SPARSE_HEADER_MAGIC) ||
        (PartitionSize < MaxUSBBufferSize) ||
        ((PartitionSize > MaxUSBBufferSize) &&
        (IsDisableParallelDownloadFlash () ||
        (Status != EFI_SUCCESS)))) {
    if (EFI_ERROR (FlashResult)) {
      if (FlashResult == EFI_NOT_FOUND) {
        AsciiSPrint (FlashResultStr, MAX_RSP_SIZE, "(%s) No such partition",
                     PartitionName);
      } else {
        AsciiSPrint (FlashResultStr, MAX_RSP_SIZE, "%a : %r",
                     "Error flashing partition", FlashResult);
      }

      DEBUG ((EFI_D_ERROR, "%a\n", FlashResultStr));
      FastbootFail (FlashResultStr);

      /* Reset the Flash Result for next flash command */
      FlashResult = EFI_SUCCESS;
      goto out;
    } else {
      DEBUG ((EFI_D_INFO, "flash image status:  %r\n", FlashResult));
      FastbootOkay ("");
    }
  }
  out:
  LunSet = FALSE;
}

STATIC VOID
CmdErase (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  EFI_STATUS Status;
  CHAR16 OutputString[FASTBOOT_STRING_MAX_LENGTH];
  CHAR16 PartitionName[MAX_GPT_NAME_SIZE];

  WaitForFlashFinished ();

  if (AsciiStrLen (arg) >= MAX_GPT_NAME_SIZE) {
    FastbootFail ("Invalid partition name");
    return;
  }
  AsciiStrToUnicodeStr (arg, PartitionName);

  // Build output string
  UnicodeSPrint (OutputString, sizeof (OutputString),
                 L"Erasing partition %s\r\n", PartitionName);
  Status = FastbootErasePartition (PartitionName);
  if (EFI_ERROR (Status)) {
    FastbootFail ("Check device console.");
    DEBUG ((EFI_D_ERROR, "Couldn't erase image:  %r\n", Status));
  } else {
    FastbootOkay ("");
  }
}
#endif

STATIC VOID
FlashCompleteHandler (IN EFI_EVENT Event, IN VOID *Context)
{
  EFI_STATUS Status = EFI_SUCCESS;

  /* Wait for flash completely before sending okay */
  if (!IsFlashComplete) {
    Status = gBS->SetTimer (Event, TimerRelative, 100000);
    if (EFI_ERROR (Status)) {
      FastbootFail ("Failed to set timer for waiting flash completely");
      goto Out;
    }
    return;
  }

  FastbootOkay ("");
Out:
  gBS->CloseEvent (Event);
  Event = NULL;
}

/* Parallel usb sending data and device writing data
 * It's need to delay to send okay until flashing finished for
 * next command.
 */
STATIC EFI_STATUS FastbootOkayDelay (VOID)
{
  EFI_STATUS Status = EFI_SUCCESS;
  EFI_EVENT FlashCompleteEvent = NULL;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             FlashCompleteHandler, NULL, &FlashCompleteEvent);
  if (EFI_ERROR (Status)) {
    FastbootFail ("Failed to creat event for waiting flash completely");
    return Status;
  }

  Status = gBS->SetTimer (FlashCompleteEvent, TimerRelative, 100000);
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (FlashCompleteEvent);
    FlashCompleteEvent = NULL;
    FastbootFail ("Failed to set timer for waiting flash completely");
  }

  return Status;
}

STATIC VOID
AcceptData (IN UINT64 Size, IN VOID *Data)
{
  UINT64 RemainingBytes = mNumDataBytes - mBytesReceivedSoFar;
  UINT32 PageSize = 0;
  UINT32 RoundSize = 0;

  /* Protocol doesn't say anything about sending extra data so just ignore it.*/
  if (Size > RemainingBytes) {
    Size = RemainingBytes;
  }

  mBytesReceivedSoFar += Size;

  /* Either queue the max transfer size 1 MB or only queue the remaining
   * amount of data left to avoid zlt issues
   */
  if (mBytesReceivedSoFar == mNumDataBytes) {
    /* Download Finished */
    DEBUG ((EFI_D_INFO, "Download Finished\n"));
    /* Zero initialized the surplus data buffer. It's risky to access the data
     * buffer which it's not zero initialized, its content might leak
     */
    GetPageSize (&PageSize);
    RoundSize = ROUND_TO_PAGE (mNumDataBytes, PageSize - 1);
    if (RoundSize < MaxUSBBufferSize) {
      gBS->SetMem ((VOID *)(Data + mNumDataBytes), RoundSize - mNumDataBytes,
                   0);
    }
    mState = ExpectCmdState;

    if (IsUseMThreadParallel ())  {
      KernIntf->Lock->ReleaseLock (LockDownload);
      FastbootOkay ("");
    } else {
      /* Stop usb timer after data transfer completed */
      StopUsbTimer ();
      /* Postpone Fastboot Okay until flash completed */
      FastbootOkayDelay ();
    }
  } else {
    GetFastbootDeviceData ()->UsbDeviceProtocol->Send (
        ENDPOINT_IN, GetXfrSize (), (Data + mBytesReceivedSoFar));
    DEBUG ((EFI_D_VERBOSE, "AcceptData: Send %d\n", GetXfrSize ()));
  }
}

/* Called based on the event received from USB device protocol:
 */
VOID
DataReady (IN UINT64 Size, IN VOID *Data)
{
  DEBUG ((EFI_D_VERBOSE, "DataReady %d\n", Size));
  if (mState == ExpectCmdState)
    AcceptCmd (Size, (CHAR8 *)Data);
  else if (mState == ExpectDataState)
    AcceptData (Size, Data);
  else {
    DEBUG ((EFI_D_ERROR, "DataReady Unknown status received\r\n"));
    return;
  }
}

STATIC VOID
FatalErrorNotify (IN EFI_EVENT Event, IN VOID *Context)
{
  DEBUG ((EFI_D_ERROR, "Fatal error sending command response. Exiting.\r\n"));
  Finished = TRUE;
}

/* Fatal error during fastboot */
BOOLEAN FastbootFatal (VOID)
{
  return Finished;
}
STATIC EFI_STATUS
ReadFromPartitionOffset (EFI_GUID *Ptype, VOID **Msg,
                         UINT32 Size, UINT32 Offset)
{
  EFI_STATUS Status;
  EFI_BLOCK_IO_PROTOCOL *BlkIo = NULL;
  PartiSelectFilter HandleFilter;
  HandleInfo HandleInfoList[1];
  UINT32 MaxHandles;
  UINT32 BlkIOAttrib = 0;
  UINT64 MsgSize;
  UINT64 PartitionSize;

  BlkIOAttrib = BLK_IO_SEL_PARTITIONED_GPT;
  BlkIOAttrib |= BLK_IO_SEL_MEDIA_TYPE_NON_REMOVABLE;
  BlkIOAttrib |= BLK_IO_SEL_MATCH_PARTITION_TYPE_GUID;

  HandleFilter.RootDeviceType = NULL;
  HandleFilter.PartitionType = Ptype;
  HandleFilter.VolumeName = NULL;

  MaxHandles = ARRAY_SIZE (HandleInfoList);

  Status =
      GetBlkIOHandles (BlkIOAttrib, &HandleFilter, HandleInfoList, &MaxHandles);

  if (Status == EFI_SUCCESS) {
    if (MaxHandles == 0)
      return EFI_NO_MEDIA;

    if (MaxHandles != 1) {
      // Unable to deterministically load from single partition
      DEBUG ((EFI_D_INFO, "%s: multiple partitions found.\r\n", __func__));
      return EFI_LOAD_ERROR;
    }
  } else {
    DEBUG ((EFI_D_ERROR,
           "%s: GetBlkIOHandles failed: %r\n", __func__, Status));
    return Status;
  }

  BlkIo = HandleInfoList[0].BlkIo;
  MsgSize = ROUND_TO_PAGE (Size, BlkIo->Media->BlockSize - 1);
  PartitionSize = GetPartitionSize (BlkIo);
  if (MsgSize > PartitionSize ||
    !PartitionSize) {
    return EFI_OUT_OF_RESOURCES;
  }

  *Msg = AllocateZeroPool (MsgSize);
  if (!(*Msg)) {
    DEBUG (
        (EFI_D_ERROR, "Error allocating memory for reading from Partition\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  if (Offset % BlkIo->Media->BlockSize) {
    DEBUG (
        (EFI_D_ERROR, "Error offset:%u passed is not multiple of blocksz:%u\n",
        Offset, BlkIo->Media->BlockSize));
    return EFI_INVALID_PARAMETER;
  }
  Status = BlkIo->ReadBlocks (BlkIo, BlkIo->Media->MediaId,
                               (Offset / BlkIo->Media->BlockSize),
                               MsgSize, *Msg);
  if (Status != EFI_SUCCESS) {
    FreePool (*Msg);
    *Msg = NULL;
    return Status;
  }

  return Status;
}
EFI_STATUS
ReadFromPartition (EFI_GUID *Ptype, VOID **Msg, UINT32 Size)
{
  return (ReadFromPartitionOffset (Ptype, Msg, Size, 0));
}

/* This function must be called to deallocate the USB buffers, as well
 * as the main Fastboot Buffer. Also Frees Variable data Structure
 */
EFI_STATUS
FastbootCmdsUnInit (VOID)
{
  EFI_STATUS Status;

  if (mDataBuffer) {
    Status = GetFastbootDeviceData ()->UsbDeviceProtocol->FreeTransferBuffer (
        (VOID *)mDataBuffer);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Failed to free up fastboot buffer\n"));
      return Status;
    }
  }
  FastbootUnInit ();
  GetFastbootDeviceData ()->UsbDeviceProtocol->Stop ();
  return EFI_SUCCESS;
}

/* This function must be called to check maximum allocatable chunk for
 * Fastboot Buffer.
 */
STATIC VOID
GetMaxAllocatableMemory (
  OUT UINT64 *FreeSize
  )
{
  EFI_MEMORY_DESCRIPTOR       *MemMap;
  EFI_MEMORY_DESCRIPTOR       *MemMapPtr;
  UINTN                       MemMapSize;
  UINTN                       MapKey, DescriptorSize;
  UINTN                       Index;
  UINTN                       MaxFree = 0;
  UINT32                      DescriptorVersion;
  EFI_STATUS                  Status;

  MemMapSize = 0;
  MemMap     = NULL;
  *FreeSize = 0;

  // Get size of current memory map.
  Status = gBS->GetMemoryMap (&MemMapSize, MemMap, &MapKey,
                              &DescriptorSize, &DescriptorVersion);
  /*
    If the MemoryMap buffer is too small, the EFI_BUFFER_TOO_SMALL error
    code is returned and the MemoryMapSize value contains the size of
    the buffer needed to contain the current memory map.
    The actual size of the buffer allocated for the consequent call
    to GetMemoryMap() should be bigger then the value returned in
    MemMapSize, since allocation of the new buffer may
    potentially increase memory map size.
  */
  if (Status != EFI_BUFFER_TOO_SMALL) {
    DEBUG ((EFI_D_ERROR, "ERROR: Undefined response get memory map\n"));
    return;
  }

  /*
    Allocate some additional memory as returned by MemMapSize,
    and query current memory map.
  */
  if (CHECK_ADD64 (MemMapSize, EFI_PAGE_SIZE)) {
    DEBUG ((EFI_D_ERROR, "ERROR: integer Overflow while adding additional"
                         "memory to MemMapSize"));
    return;
  }
  MemMapSize = MemMapSize + EFI_PAGE_SIZE;
  MemMap = AllocateZeroPool (MemMapSize);
  if (!MemMap) {
    DEBUG ((EFI_D_ERROR,
                    "ERROR: Failed to allocate memory for memory map\n"));
    return;
  }

  // Store pointer to be freed later.
  MemMapPtr = MemMap;
  // Get System MemoryMap
  Status = gBS->GetMemoryMap (&MemMapSize, MemMap, &MapKey,
                              &DescriptorSize, &DescriptorVersion);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "ERROR: Failed to query memory map\n"));
    FreePool (MemMapPtr);
    return;
  }

  // Find largest free chunk of unallocated memory available.
  for (Index = 0; Index < MemMapSize / DescriptorSize; Index ++) {
    if (MemMap->Type == EfiConventionalMemory &&
          MaxFree < MemMap->NumberOfPages) {
          MaxFree = MemMap->NumberOfPages;
    }
    MemMap = (EFI_MEMORY_DESCRIPTOR *)((UINTN)MemMap + DescriptorSize);
  }

  *FreeSize = EFI_PAGES_TO_SIZE (MaxFree);
  DEBUG ((EFI_D_VERBOSE, "Free Memory available: %ld\n", *FreeSize));
  FreePool (MemMapPtr);
  return;
}

//Shoud block command until flash finished
VOID WaitForFlashFinished (VOID)
{
  if (!IsFlashComplete &&
    IsUseMThreadParallel ()) {
    KernIntf->Lock->AcquireLock (LockFlash);
    KernIntf->Lock->ReleaseLock (LockFlash);
  }
}

VOID ThreadSleep (TimeDuration Delay)
{
  KernIntf->Thread->ThreadSleep (Delay);
}

BOOLEAN IsUseMThreadParallel (VOID)
{
  if (FixedPcdGetBool (EnableMultiThreadFlash)) {
    return IsMultiThreadSupported;
  }

  return FALSE;
}

VOID InitMultiThreadEnv ()
{
  EFI_STATUS Status = EFI_SUCCESS;

  if (IsDisableParallelDownloadFlash ()) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiKernelProtocolGuid, NULL,
      (VOID **)&KernIntf);

  if ((Status != EFI_SUCCESS) ||
    (KernIntf == NULL) ||
    KernIntf->Version < EFI_KERNEL_PROTOCOL_VER_LOCK_API) {
    DEBUG ((EFI_D_VERBOSE, "Multi thread is not supported.\n"));
    return;
  }

  KernIntf->Lock->InitLock ("DOWNLOAD", &LockDownload);
  if (&LockDownload == NULL) {
     DEBUG ((EFI_D_ERROR, "InitLock LockDownload error \n"));
     return;
  }

  KernIntf->Lock->InitLock ("FLASH", &LockFlash);
  if (&LockFlash == NULL) {
    DEBUG ((EFI_D_ERROR, "InitLock LockFlash error \n"));
    KernIntf->Lock->DestroyLock (LockDownload);
    return;
  }

  //init MultiThreadEnv succeeded, use multi thread to flash
  IsMultiThreadSupported = TRUE;

  DEBUG ((EFI_D_VERBOSE,
          "InitMultiThreadEnv successfully, will use thread to flash \n"));
}

STATIC VOID GetBufferSize (UINT64 *MaxBufferSize, UINT64 *MinBufferSize)
{
  EFI_STATUS Status;
  UINT64 DdrSize = 0;

  Status = GetDdrSize (&DdrSize);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Error getting DDR Type %r\n", Status));
    return;
  }

  if (DdrSize <= DDR_512MB) {
    /* 35MB */
    *MaxBufferSize = 36700160;
    /* 16MB */
    *MinBufferSize = 16777216;
  }
}

EFI_STATUS
FastbootCmdsInit (VOID)
{
  EFI_STATUS Status;
  EFI_EVENT mFatalSendErrorEvent;
  CHAR8 *FastBootBuffer;
  UINT64 MaxBufferSize = MAX_BUFFER_SIZE;
  UINT64 MinBufferSize = MIN_BUFFER_SIZE;

  mDataBuffer = NULL;
  mUsbDataBuffer = NULL;
  mFlashDataBuffer = NULL;

  DEBUG ((EFI_D_INFO, "Fastboot: Initializing...\n"));

  /* Disable watchdog */
  Status = gBS->SetWatchdogTimer (0, 0x10000, 0, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Fastboot: Couldn't disable watchdog timer: %r\n",
            Status));
  }

  /* Create event to pass to FASTBOOT_PROTOCOL.Send, signalling a fatal error */
  Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, FatalErrorNotify,
                             NULL, &mFatalSendErrorEvent);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Couldn't create Fastboot protocol send event: %r\n",
            Status));
    return Status;
  }

  /* Get the Max/Min download size for low memory */
  GetBufferSize (&MaxBufferSize, &MinBufferSize);

  /* Allocate buffer used to store images passed by the download command */
  GetMaxAllocatableMemory (&MaxUSBBufferSize);
  if (!MaxUSBBufferSize) {
    DEBUG ((EFI_D_ERROR, "Failed to get free memory for fastboot buffer\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  do {
    // Try allocating 3/4th of free memory available.
    MaxUSBBufferSize = EFI_FREE_MEM_DIVISOR (MaxUSBBufferSize);
    MaxUSBBufferSize = LOCAL_ROUND_TO_PAGE (MaxUSBBufferSize, EFI_PAGE_SIZE);
    if (MaxUSBBufferSize < MinBufferSize) {
      DEBUG ((EFI_D_ERROR,
        "ERROR: Allocation fail for minimim buffer for fastboot\n"));
      return EFI_OUT_OF_RESOURCES;
    }

    /* If available buffer on target is more than max buffer size,
       we limit this to max buffer buffer size we support */
    if (MaxUSBBufferSize > MaxBufferSize) {
      MaxUSBBufferSize = MaxBufferSize;
    }

    Status =
        GetFastbootDeviceData ()->UsbDeviceProtocol->AllocateTransferBuffer (
                                      MaxUSBBufferSize,
                                      (VOID **)&FastBootBuffer);
  }while (EFI_ERROR (Status));

  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Not enough memory to Allocate Fastboot Buffer\n"));
    return Status;
  }

  /* Clear allocated buffer */
  gBS->SetMem ((VOID *)FastBootBuffer, MaxUSBBufferSize , 0x0);
  DEBUG ((EFI_D_VERBOSE,
                  "Fastboot Buffer Size allocated: %ld\n", MaxBufferSize));

  MaxBufferSize = (CheckRootDeviceType () == NAND) ?
                              MaxUSBBufferSize : MaxUSBBufferSize / 2;

  FastbootCommandSetup ((VOID *)FastBootBuffer, MaxUSBBufferSize);

  InitMultiThreadEnv ();

  return EFI_SUCCESS;
}
typedef enum {
  /* 0 - 31 Cold reset: Common defined features
   * 32 - 63 Cold Reset: OEM specific reasons
   * 64 - 254 - Reserved
   * 255 - Emergency download
   */
  NORMAL_MODE = 0x0,
  RECOVERY_MODE = 0x1,
  FASTBOOT_MODE = 0x2,
  ALARM_BOOT = 0x3,
  DM_VERITY_LOGGING = 0x4,
  DM_VERITY_ENFORCING = 0x5,
  DM_VERITY_KEYSCLEAR = 0x6,
  SILENT_MODE = 0xA,
  NON_SILENT_MODE = 0xB,
  FORCED_SILENT = 0xC,
  FORCED_NON_SILENT = 0xD,
  OEM_RESET_MIN = 0x20,
  OEM_RESET_MAX = 0x3f,
  EMERGENCY_DLOAD = 0xFF,
} RebootReasonType;
/* See header for documentation */
VOID
FastbootRegister (IN CONST CHAR8 *prefix,
                  IN VOID (*handle) (CONST CHAR8 *arg, VOID *data, UINT32 sz))
{
  FASTBOOT_CMD *cmd;

  cmd = AllocateZeroPool (sizeof (*cmd));
  if (cmd) {
    cmd->prefix = prefix;
    cmd->prefix_len = AsciiStrLen (prefix);
    cmd->handle = handle;
    cmd->next = cmdlist;
    cmdlist = cmd;
  } else {
    DEBUG ((EFI_D_VERBOSE,
            "Failed to allocate memory to cmd\n"));
  }
}

STATIC VOID
CmdReboot (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  DEBUG ((EFI_D_INFO, "rebooting the device\n"));
  FastbootOkay ("");

  RebootDevice (NORMAL_MODE);

  // Shouldn't get here
  FastbootFail ("Failed to reboot");
}

/*
 * Leave fastboot and hand control back to the boot menu, in answer to either
 * "exit" or "oem exit" from the host. The OKAY must leave the wire before the
 * gadget is torn down, so the transfer is drained before the request is made;
 * the menu redraws once FastbootInitialize returns.
 */
STATIC VOID
CmdExitToMenu (IN CONST CHAR8 *arg, IN VOID *data, IN UINT32 sz)
{
  (VOID)arg;
  (VOID)data;
  (VOID)sz;

  DEBUG ((EFI_D_INFO, "exit to boot menu requested\n"));
  FastbootOkay ("");
  WaitForTransferComplete ();
  FastbootRequestExitToMenu ();
}

STATIC VOID UpdateGetVarVariable (VOID)
{
}

STATIC VOID
FastbootGetVarInfo (CONST CHAR8 *Payload)
{
  FastbootInfo (Payload);
  /* The next fragment must not overwrite an in-flight transfer buffer. */
  WaitForTransferComplete ();
}

STATIC VOID CmdGetVarAll (VOID)
{
  FASTBOOT_VAR *Var;

  for (Var = Varlist; Var; Var = Var->next)
    SfbFastbootGetVarInfo (Var->name, Var->value, FastbootGetVarInfo);

  FastbootOkay ("");
}

STATIC VOID
CmdGetVar (CONST CHAR8 *Arg, VOID *Data, UINT32 Size)
{
  FASTBOOT_VAR *Var;
  CHAR16 PartNameUniStr[MAX_GPT_NAME_SIZE];
  CHAR8 *Token = AsciiStrStr (Arg, "partition-");

  UpdateGetVarVariable ();

  if (!(AsciiStrCmp ("all", Arg))) {
    CmdGetVarAll ();
    return;
  }

  if (Token) {
    Token = AsciiStrStr (Arg, ":");
    if (Token) {
      Token = Token + AsciiStrLen (":");
      if (AsciiStrLen (Token) >= ARRAY_SIZE (PartNameUniStr)) {
        FastbootFail ("Invalid partition name");
        return;
      }

      AsciiStrToUnicodeStr (Token, PartNameUniStr);
    }
  }

  for (Var = Varlist; Var; Var = Var->next) {
    if (!AsciiStrCmp (Var->name, Arg)) {
      FastbootOkay (Var->value);
      return;
    }
  }

  FastbootFail ("GetVar Variable Not found");
}

#ifdef ENABLE_BOOT_CMD
#define EFI_PE_SIGNATURE  0x5A4D
STATIC BOOLEAN
IsEfiInBootImg (boot_img_hdr *Hdr, UINT32 Size, VOID **EfiData, UINT32 *EfiSize)
{
  UINT32 PageSize;
  UINT8  *Kernel;

  if (Size < sizeof (boot_img_hdr)) {
    return FALSE;
  }

  if (AsciiStrnCmp ((CHAR8 *)Hdr->magic, BOOT_MAGIC, BOOT_MAGIC_SIZE) != 0) {
    return FALSE;
  }

  if (Hdr->header_version > 2) {
    /* v3/v4 header 的 kernel 紧跟在 header 后面，page_size 固定 4096 */
    PageSize = 4096;
  } else {
    PageSize = Hdr->page_size;
  }

  if (PageSize == 0 || Hdr->kernel_size == 0) {
    return FALSE;
  }

  if (Size < PageSize + Hdr->kernel_size) {
    return FALSE;
  }

  Kernel = (UINT8 *)Hdr + PageSize;

  /* PE/COFF: 'M'=0x4D, 'Z'=0x5A */
  if (Kernel[0] == 0x4D && Kernel[1] == 0x5A) {
    *EfiData = Kernel;
    *EfiSize = Hdr->kernel_size;
    return TRUE;
  }

  return FALSE;
}
STATIC EFI_STATUS
BootEfiImage (VOID *Data, UINT32 Size)
{
  EFI_STATUS  Status;
  EFI_HANDLE  ImageHandle = NULL;

  Status = gBS->LoadImage (
                  FALSE,
                  gImageHandle,
                  NULL,
                  Data,
                  Size,
                  &ImageHandle
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LoadImage failed: %r\n", Status));
    return Status;
  }

  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "StartImage failed: %r\n", Status));
  }

  return Status;
}


STATIC VOID
CmdBoot (CONST CHAR8 *Arg, VOID *Data, UINT32 Size)
{
  boot_img_hdr *hdr = Data;
    VOID            *EfiData = NULL;
  UINT32           EfiSize = 0;
  /* ============================================
   * 新增：检测并处理 EFI 可执行文件
   * ============================================ */
  if (IsEfiInBootImg (hdr, Size, &EfiData, &EfiSize)) {
    DEBUG ((EFI_D_INFO, "CmdBoot: EFI image in boot.img, size=%u\n", EfiSize));

    FastbootOkay ("Booting EFI image...");
    FastbootUsbDeviceStop ();

     BootEfiImage (EfiData, EfiSize);
    return;
  }
  FastbootFail ("No EFI image found in boot.img");
}
#endif

STATIC EFI_STATUS
AcceptCmdTimerInit (IN UINT64 Size, IN CHAR8 *Data)
{
  EFI_STATUS Status = EFI_SUCCESS;
  CmdInfo *AcceptCmdInfo = NULL;
  EFI_EVENT CmdEvent = NULL;

  AcceptCmdInfo = AllocateZeroPool (sizeof (CmdInfo));
  if (!AcceptCmdInfo)
    return EFI_OUT_OF_RESOURCES;

  AcceptCmdInfo->Size = Size;
  AcceptCmdInfo->Data = Data;

  Status = gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK,
                             AcceptCmdHandler, AcceptCmdInfo, &CmdEvent);

  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (CmdEvent, TimerRelative, 100000);
  }

  if (EFI_ERROR (Status)) {
    FreePool (AcceptCmdInfo);
    AcceptCmdInfo = NULL;
  }

  return Status;
}

STATIC VOID
AcceptCmdHandler (IN EFI_EVENT Event, IN VOID *Context)
{
  CmdInfo *AcceptCmdInfo = Context;

  if (Event) {
    gBS->SetTimer (Event, TimerCancel, 0);
    gBS->CloseEvent (Event);
  }

  AcceptCmd (AcceptCmdInfo->Size, AcceptCmdInfo->Data);
  FreePool (AcceptCmdInfo);
  AcceptCmdInfo = NULL;
}

STATIC VOID
AcceptCmd (IN UINT64 Size, IN CHAR8 *Data)
{
  EFI_STATUS Status = EFI_SUCCESS;
  FASTBOOT_CMD *cmd;
  CHAR8 FlashResultStr[MAX_RSP_SIZE] = "\0";

  if (!Data) {
    FastbootFail ("Invalid input command");
    return;
  }
  if (Size > MAX_FASTBOOT_COMMAND_SIZE)
    Size = MAX_FASTBOOT_COMMAND_SIZE;
  Data[Size] = '\0';

  DEBUG ((EFI_D_INFO, "Handling Cmd: %a\n", Data));

  if (!IsDisableParallelDownloadFlash ()) {
    /* Wait for flash finished before next command */
    if (AsciiStrnCmp (Data, "download", AsciiStrLen ("download"))) {
      StopUsbTimer ();
      if (!IsFlashComplete &&
          !IsUseMThreadParallel ()) {
        Status = AcceptCmdTimerInit (Size, Data);
        if (Status == EFI_SUCCESS) {
          return;
        }
      }
    }

    /* Check last flash result */
    if (FlashResult != EFI_SUCCESS) {
      AsciiSPrint (FlashResultStr, MAX_RSP_SIZE, "%a : %r",
                 "Error: Last flash failed", FlashResult);

      DEBUG ((EFI_D_ERROR, "%a\n", FlashResultStr));
      if (!AsciiStrnCmp (Data, "flash", AsciiStrLen ("flash")) ||
          !AsciiStrnCmp (Data, "download", AsciiStrLen ("download"))) {
        FastbootFail (FlashResultStr);
        FlashResult = EFI_SUCCESS;
        return;
      }
    }
  }

  for (cmd = cmdlist; cmd; cmd = cmd->next) {
    if (AsciiStrnCmp (Data, cmd->prefix, cmd->prefix_len))
      continue;

    cmd->handle ((CONST CHAR8 *)Data + cmd->prefix_len, (VOID *)mUsbDataBuffer,
                 (UINT32)mBytesReceivedSoFar);
    return;
  }
  DEBUG ((EFI_D_ERROR, "\nFastboot Send Fail\n"));
  FastbootFail ("unknown command");
}

STATIC EFI_STATUS
GetPartitionType (IN CHAR16 *PartName, OUT CHAR8 * PartType)
{
  CHAR8 AsciiPartName[MAX_GET_VAR_NAME_SIZE];


  if (PartName == NULL ||
      PartType == NULL) {
    DEBUG ((EFI_D_ERROR, "Invalid parameters to GetPartitionType\n"));
    return EFI_INVALID_PARAMETER;
  }

  /* By default copy raw to response */
  AsciiStrnCpyS (PartType, MAX_GET_VAR_NAME_SIZE,
                  RAW_FS_STR, AsciiStrLen (RAW_FS_STR));
  UnicodeStrToAsciiStr (PartName, AsciiPartName);
  return EFI_SUCCESS;
}

STATIC EFI_STATUS
GetPartitionSizeViaName (IN CHAR16 *PartName, OUT CHAR8 * PartSize)
{
  EFI_BLOCK_IO_PROTOCOL *BlockIo = NULL;
  EFI_HANDLE *Handle = NULL;
  EFI_STATUS Status = EFI_INVALID_PARAMETER;
  UINT64 PartitionSize;

  Status = PartitionGetInfo (PartName, &BlockIo, &Handle);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

  if (!BlockIo) {
    DEBUG ((EFI_D_ERROR, "BlockIo for %s is corrupted\n", PartName));
    return EFI_VOLUME_CORRUPTED;
  }

  PartitionSize = GetPartitionSize (BlockIo);
  if (!PartitionSize) {
    return EFI_BAD_BUFFER_SIZE;
  }

  AsciiSPrint (PartSize, MAX_RSP_SIZE, " 0x%llx", PartitionSize);
  return EFI_SUCCESS;

}

STATIC EFI_STATUS
PublishGetVarPartitionInfo (
                            IN struct GetVarPartitionInfo *PublishedPartInfo,
                            IN UINT32 NumParts)
{
  UINT32 PtnLoopCount;
  EFI_STATUS Status = EFI_INVALID_PARAMETER;
  EFI_STATUS RetStatus = EFI_SUCCESS;
  CHAR16 *PartitionNameUniCode = NULL;
  BOOLEAN PublishType;
  BOOLEAN PublishSize;

  /* Clear Published Partition Buffer */
  gBS->SetMem (PublishedPartInfo,
          sizeof (struct GetVarPartitionInfo) * MAX_NUM_PARTITIONS, 0);

  /* Loop will go through each partition entry
     and publish info for all partitions.*/
  for (PtnLoopCount = 0; PtnLoopCount < NumParts; PtnLoopCount++) {
    PublishType = FALSE;
    PublishSize = FALSE;
    PartitionNameUniCode = PtnEntries[PtnLoopCount].PartEntry.PartitionName;
    /* Skip Null/last partition */
    if (PartitionNameUniCode[0] == '\0') {
      continue;
    }
    UnicodeStrToAsciiStr (PtnEntries[PtnLoopCount].PartEntry.PartitionName,
                          (CHAR8 *)PublishedPartInfo[PtnLoopCount].part_name);

    /* Fill partition size variable and response string */
    AsciiStrnCpyS (PublishedPartInfo[PtnLoopCount].getvar_size_str,
                      MAX_GET_VAR_NAME_SIZE, "partition-size:",
                      AsciiStrLen ("partition-size:"));
    Status = AsciiStrnCatS (PublishedPartInfo[PtnLoopCount].getvar_size_str,
                            MAX_GET_VAR_NAME_SIZE,
                            PublishedPartInfo[PtnLoopCount].part_name,
                            AsciiStrLen (
                              PublishedPartInfo[PtnLoopCount].part_name));
    if (!EFI_ERROR (Status)) {
      Status = GetPartitionSizeViaName (
                            PartitionNameUniCode,
                            PublishedPartInfo[PtnLoopCount].size_response);
      if (Status == EFI_SUCCESS) {
        PublishSize = TRUE;
      }
    }

    /* Fill partition type variable and response string */
    AsciiStrnCpyS (PublishedPartInfo[PtnLoopCount].getvar_type_str,
                    MAX_GET_VAR_NAME_SIZE, "partition-type:",
                    AsciiStrLen ("partition-type:"));
    Status = AsciiStrnCatS (PublishedPartInfo[PtnLoopCount].getvar_type_str,
                              MAX_GET_VAR_NAME_SIZE,
                              PublishedPartInfo[PtnLoopCount].part_name,
                              AsciiStrLen (
                                PublishedPartInfo[PtnLoopCount].part_name));
    if (!EFI_ERROR (Status)) {
      Status = GetPartitionType (
                            PartitionNameUniCode,
                            PublishedPartInfo[PtnLoopCount].type_response);
      if (Status == EFI_SUCCESS) {
        PublishType = TRUE;
      }
    }

    if (PublishSize) {
      FastbootPublishVar (PublishedPartInfo[PtnLoopCount].getvar_size_str,
                              PublishedPartInfo[PtnLoopCount].size_response);
    } else {
        DEBUG ((EFI_D_ERROR, "Error Publishing size info for %s partition\n",
                                                        PartitionNameUniCode));
        RetStatus = EFI_INVALID_PARAMETER;
    }

    if (PublishType) {
      FastbootPublishVar (PublishedPartInfo[PtnLoopCount].getvar_type_str,
                              PublishedPartInfo[PtnLoopCount].type_response);
    } else {
        DEBUG ((EFI_D_ERROR, "Error Publishing type info for %s partition\n",
                                                        PartitionNameUniCode));
        RetStatus = EFI_INVALID_PARAMETER;
    }
  }
  return RetStatus;
}

/* Registers all Stock commands, Publishes all stock variables
 * and partitiion sizes. base and size are the respective parameters
 * to the Fastboot Buffer used to store the downloaded image for flashing
 */
STATIC EFI_STATUS
FastbootCommandSetup (IN VOID *Base, IN UINT64 Size)
{
  EFI_STATUS Status;
  CHAR8 HWPlatformBuf[MAX_RSP_SIZE] = "\0";
  CHAR8 DeviceType[MAX_RSP_SIZE] = "\0";
  UINT32 PartitionCount = 0;
  MemCardType Type = UNKNOWN;
  mDataBuffer = Base;
  mNumDataBytes = Size;
  mFlashNumDataBytes = Size;
  mUsbDataBuffer = Base;

  mFlashDataBuffer = (CheckRootDeviceType () == NAND) ?
                           Base : (Base + MaxUSBBufferSize);

  /* Find all Software Partitions in the User Partition */
  UINT32 i;
  UINT32 BlkSize = 0;

  struct FastbootCmdDesc cmd_list[] = {
      /* By Default enable list is empty */
      {"", NULL},
/*CAUTION(High): Enabling these commands will allow changing the partitions
 *like system,userdata,cachec etc...
 */
#ifdef ENABLE_UPDATE_PARTITIONS_CMDS
      {"flash:", CmdFlash},
      {"erase:", CmdErase},
      {"fetch:", CmdFetch},
#endif
/*
 *CAUTION(CRITICAL): Enabling this command will allow boot with different
 *bootimage.
 */
#ifdef ENABLE_BOOT_CMD
      {"boot", CmdBoot},
#endif
      {"reboot", CmdReboot},
      {"exit", CmdExitToMenu},
      {"oem exit", CmdExitToMenu},
      {"getvar:", CmdGetVar},
      {"download:", CmdDownload},
  };

  /* Register the commands only for non-user builds */
  /* Publish getvar variables */
  AsciiSPrint (MaxBufferSizeStr,
                  sizeof (MaxBufferSizeStr), "%ld", MaxUSBBufferSize);
  FastbootPublishVar ("max-download-size", MaxBufferSizeStr);
  FastbootPublishVar ("max-fetch-size", MaxBufferSizeStr);

  AsciiSPrint (FullProduct, sizeof (FullProduct), "%a", PRODUCT_NAME);
  FastbootPublishVar ("product", FullProduct);

  GetPartitionCount (&PartitionCount);
  Status = PublishGetVarPartitionInfo (PublishedPartInfo, PartitionCount);
  if (Status != EFI_SUCCESS)
    DEBUG ((EFI_D_ERROR, "Failed to publish part info for all partitions\n"));
  BoardHwPlatformName (HWPlatformBuf, sizeof (HWPlatformBuf));
  GetRootDeviceType (DeviceType, sizeof (DeviceType));
  AsciiSPrint (StrVariant, sizeof (StrVariant), "%a %a", HWPlatformBuf,
               DeviceType);
  FastbootPublishVar ("variant", StrVariant);
  GetPageSize (&BlkSize);
  AsciiSPrint (LogicalBlkSizeStr, sizeof (LogicalBlkSizeStr), " 0x%x", BlkSize);
  FastbootPublishVar ("logical-block-size", LogicalBlkSizeStr);
  Type = CheckRootDeviceType ();
  if (Type == NAND) {
    BlkSize = NAND_PAGES_PER_BLOCK * BlkSize;
  }

  AsciiSPrint (EraseBlkSizeStr, sizeof (EraseBlkSizeStr), " 0x%x", BlkSize);
  FastbootPublishVar ("erase-block-size", EraseBlkSizeStr);

  AsciiSPrint (StrSocVersion, sizeof (StrSocVersion), "%x",
                BoardPlatformChipVersion ());
  FastbootPublishVar ("hw-revision", StrSocVersion);

  if (IsDisableParallelDownloadFlash()) {
    FastbootPublishVar ("parallel-download-flash", "no");
  } else {
    FastbootPublishVar ("parallel-download-flash", "yes");
  }

  /* Register handlers for the supported commands*/
  UINT32 FastbootCmdCnt = sizeof (cmd_list) / sizeof (cmd_list[0]);
  for (i = 1; i < FastbootCmdCnt; i++)
    FastbootRegister (cmd_list[i].name, cmd_list[i].cb);
  return EFI_SUCCESS;
}

VOID *FastbootDloadBuffer (VOID)
{
  return (VOID *)mUsbDataBuffer;
}

ANDROID_FASTBOOT_STATE FastbootCurrentState (VOID)
{
  return mState;
}
