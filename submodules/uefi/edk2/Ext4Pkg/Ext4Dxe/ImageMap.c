/* Narrow initialized-file mapping for Canoe's FAT container.
 * SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "Ext4Dxe.h"
#include <Library/Ext4ImageMap.h>

#define MAX_MAP_GROUPS 4096U
#define MAX_MAP_NODES 4096U
#define SPARSE_SUPER2 0x200U

typedef struct {
  EXT4_FILE *File;
  EXT4_IMAGE_MAP *Map;
  UINT64 Logical;
  UINTN NodeCount;
  UINT64 Nodes[MAX_MAP_NODES];
} MAP_CONTEXT;

STATIC BOOLEAN PowerOf (UINT64 Value, UINTN Base) {
  while (Value > 1 && Value % Base == 0) Value /= Base;
  return Value == 1;
}
STATIC BOOLEAN HasSuper (EXT4_PARTITION *P, UINT64 Group) {
  if (Group == 0) return TRUE;
  if (P->FeaturesCompat & SPARSE_SUPER2)
    return Group == P->SuperBlock.s_backup_bgs[0] || Group == P->SuperBlock.s_backup_bgs[1];
  if (!(P->FeaturesRoCompat & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER)) return TRUE;
  return Group == 1 || PowerOf (Group, 3) || PowerOf (Group, 5) || PowerOf (Group, 7);
}
STATIC BOOLEAN Within (UINT64 Block, UINT64 Start, UINT64 Count) {
  return Block >= Start && Block - Start < Count;
}
STATIC BOOLEAN DataBlock (EXT4_PARTITION *P, UINT64 Block) {
  UINT64 G, TableBlocks, Descriptors;
  if (Block <= P->SuperBlock.s_first_data_block || Block >= P->NumberBlocks) return FALSE;
  TableBlocks = ((UINT64)P->SuperBlock.s_inodes_per_group * P->InodeSize + P->BlockSize - 1) / P->BlockSize;
  Descriptors = (P->NumberBlockGroups * P->DescSize + P->BlockSize - 1) / P->BlockSize;
  for (G = 0; G < P->NumberBlockGroups; G++) {
    EXT4_BLOCK_GROUP_DESC Copy;
    EXT4_BLOCK_GROUP_DESC *D = &Copy;
    ZeroMem (&Copy, sizeof (Copy));
    CopyMem (&Copy, Ext4GetBlockGroupDesc (P, (UINT32)G), P->DescSize);
    UINT64 Table = Ext4MakeBlockNumberFromHalfs (P, D->bg_inode_table_lo, D->bg_inode_table_hi);
    UINT64 Bitmap = Ext4MakeBlockNumberFromHalfs (P, D->bg_block_bitmap_lo, D->bg_block_bitmap_hi);
    UINT64 Inodes = Ext4MakeBlockNumberFromHalfs (P, D->bg_inode_bitmap_lo, D->bg_inode_bitmap_hi);
    UINT64 First = P->SuperBlock.s_first_data_block + G * P->SuperBlock.s_blocks_per_group;
    if (Block == Bitmap || Block == Inodes || Within (Block, Table, TableBlocks)) return FALSE;
    /* The disk header's historical `unused` field is s_reserved_gdt_blocks. */
    if (HasSuper (P, G) && Within (Block, First, 1 + Descriptors + P->SuperBlock.unused)) return FALSE;
  }
  return TRUE;
}

STATIC EFI_STATUS Walk (MAP_CONTEXT *C, EXT4_EXTENT_HEADER *H, UINTN Capacity, UINTN Depth) {
  EXT4_PARTITION *P = C->File->Partition;
  UINTN I, J;
  if (Depth > 5 || H->eh_magic != EXT4_EXTENT_HEADER_MAGIC || H->eh_depth != Depth ||
      H->eh_entries == 0 || H->eh_entries > H->eh_max ||
      H->eh_max > (Capacity - sizeof (*H)) / sizeof (EXT4_EXTENT)) return EFI_VOLUME_CORRUPTED;
  for (I = 0; I < H->eh_entries; I++) {
    if (Depth != 0) {
      EXT4_EXTENT_INDEX *X = (EXT4_EXTENT_INDEX *)(H + 1) + I;
      UINT64 Block = ((UINT64)X->ei_leaf_hi << 32) | X->ei_leaf_lo;
      EXT4_EXTENT_HEADER *Child;
      EFI_STATUS Status;
      if (X->ei_block != C->Logical || X->ei_unused != 0 || !DataBlock (P, Block) ||
          C->NodeCount == MAX_MAP_NODES) return EFI_VOLUME_CORRUPTED;
      for (J = 0; J < C->NodeCount; J++) if (C->Nodes[J] == Block) return EFI_VOLUME_CORRUPTED;
      C->Nodes[C->NodeCount++] = Block;
      Child = AllocatePool (P->BlockSize);
      if (Child == NULL) return EFI_OUT_OF_RESOURCES;
      Status = Ext4ReadBlocks (P, Child, 1, Block);
      /* Bound eh_max before the checksum helper locates the tail. */
      if (!EFI_ERROR (Status) &&
          (Child->eh_max > (P->BlockSize - sizeof (*Child) - sizeof (UINT32)) / sizeof (EXT4_EXTENT) ||
           !Ext4CheckExtentChecksum (Child, C->File))) Status = EFI_VOLUME_CORRUPTED;
      if (!EFI_ERROR (Status)) Status = Walk (C, Child, P->BlockSize - sizeof (UINT32), Depth - 1);
      FreePool (Child);
      if (EFI_ERROR (Status)) return Status;
    } else {
      EXT4_EXTENT *E = (EXT4_EXTENT *)(H + 1) + I;
      UINT64 Physical = ((UINT64)E->ee_start_hi << 32) | E->ee_start_lo;
      UINT64 Length = E->ee_len;
      EXT4_IMAGE_RANGE *R;
      if (E->ee_block != C->Logical || Length == 0 || Length > 32768 ||
          Length > C->Map->Bytes / P->BlockSize - C->Logical ||
          Physical >= P->NumberBlocks || Length > P->NumberBlocks - Physical ||
          C->Map->Count == EXT4_IMAGE_MAX_EXTENTS) return EFI_VOLUME_CORRUPTED;
      for (J = 0; J < Length; J++) if (!DataBlock (P, Physical + J)) return EFI_VOLUME_CORRUPTED;
      for (J = 0; J < C->Map->Count; J++) {
        EXT4_IMAGE_RANGE *Old = &C->Map->Ranges[J];
        if (Physical * P->BlockSize < Old->Physical + Old->Bytes &&
            Old->Physical < (Physical + Length) * P->BlockSize) return EFI_VOLUME_CORRUPTED;
      }
      R = &C->Map->Ranges[C->Map->Count++];
      R->Logical = C->Logical * P->BlockSize;
      R->Physical = Physical * P->BlockSize;
      R->Bytes = Length * P->BlockSize;
      C->Logical += Length;
    }
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS CheckAllocation (MAP_CONTEXT *C) {
  EXT4_PARTITION *P = C->File->Partition;
  UINT8 *Bitmap = AllocatePool (P->BlockSize);
  UINT64 G;
  EFI_STATUS Status = EFI_SUCCESS;
  if (Bitmap == NULL) return EFI_OUT_OF_RESOURCES;
  for (G = 0; G < P->NumberBlockGroups && !EFI_ERROR (Status); G++) {
    EXT4_BLOCK_GROUP_DESC Copy;
    EXT4_BLOCK_GROUP_DESC *D = &Copy;
    ZeroMem (&Copy, sizeof (Copy));
    CopyMem (&Copy, Ext4GetBlockGroupDesc (P, (UINT32)G), P->DescSize);
    UINT64 First = P->SuperBlock.s_first_data_block + G * P->SuperBlock.s_blocks_per_group;
    UINT64 End = First + P->SuperBlock.s_blocks_per_group;
    UINTN I;
    BOOLEAN Needed = FALSE;
    if (!Ext4VerifyBlockGroupDescChecksum (P, D, (UINT32)G)) { Status = EFI_VOLUME_CORRUPTED; break; }
    for (I = 0; I < C->Map->Count; I++) {
      EXT4_IMAGE_RANGE *R = &C->Map->Ranges[I];
      if (R->Physical / P->BlockSize < End && (R->Physical + R->Bytes) / P->BlockSize > First)
        Needed = TRUE;
    }
    for (I = 0; I < C->NodeCount; I++) if (Within (C->Nodes[I], First, End - First)) Needed = TRUE;
    if (!Needed) continue;
    if (D->bg_flags & 2) { Status = EFI_VOLUME_CORRUPTED; break; } /* BLOCK_UNINIT */
    Status = Ext4ReadBlocks (P, Bitmap, 1, Ext4MakeBlockNumberFromHalfs (P, D->bg_block_bitmap_lo, D->bg_block_bitmap_hi));
    if (EFI_ERROR (Status)) break;
    if (P->FeaturesRoCompat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
      UINT32 Expected = D->bg_block_bitmap_csum_lo;
      UINT32 Actual = CalculateCrc32c (Bitmap, P->SuperBlock.s_blocks_per_group / 8, P->InitialSeed);
      if (P->DescSize >= 64) Expected |= (UINT32)D->bg_block_bitmap_csum_hi << 16;
      else Actual &= 0xffff;
      if (Expected != Actual) { Status = EFI_VOLUME_CORRUPTED; break; }
    }
    for (I = 0; I < C->Map->Count && !EFI_ERROR (Status); I++) {
      EXT4_IMAGE_RANGE *R = &C->Map->Ranges[I];
      UINT64 Start = R->Physical / P->BlockSize;
      UINT64 Stop = (R->Physical + R->Bytes) / P->BlockSize;
      UINT64 B;
      if (Start < First) Start = First;
      if (Stop > End) Stop = End;
      for (B = Start; B < Stop; B++)
        if (!(Bitmap[(B - First) / 8] & (1U << ((B - First) % 8)))) { Status = EFI_VOLUME_CORRUPTED; break; }
    }
    for (I = 0; I < C->NodeCount; I++) {
      UINT64 B = C->Nodes[I];
      if (Within (B, First, End - First) && !(Bitmap[(B - First) / 8] & (1U << ((B - First) % 8))))
        Status = EFI_VOLUME_CORRUPTED;
    }
  }
  FreePool (Bitmap);
  return Status;
}

EFI_STATUS Ext4MapImage (EFI_FILE_PROTOCOL *Protocol, EXT4_IMAGE_MAP **Out) {
  EXT4_FILE *File;
  EXT4_PARTITION *P;
  MAP_CONTEXT *C;
  EXT4_INODE *Fresh = NULL;
  EXT4_SUPERBLOCK FreshSuper;
  EFI_STATUS Status;
  UINTN I, J;
  UINT32 OriginalMediaId;
  UINT64 DeviceBytes;
  UINT32 AllowedRo = EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT4_FEATURE_RO_COMPAT_LARGE_FILE |
                     EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM |
                     EXT4_FEATURE_RO_COMPAT_DIR_NLINK | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |
                     EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
  UINT32 AllowedCompat = EXT3_FEATURE_COMPAT_HAS_JOURNAL | EXT4_FEATURE_COMPAT_EXT_ATTR |
                         EXT4_FEATURE_COMPAT_RESIZE_INO | EXT4_FEATURE_COMPAT_DIR_INDEX | SPARSE_SUPER2;
  UINT32 AllowedIncompat = EXT4_FEATURE_INCOMPAT_FILETYPE | EXT4_FEATURE_INCOMPAT_EXTENTS |
                          EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG |
                          EXT4_FEATURE_INCOMPAT_RECOVER;
  if (Out == NULL) return EFI_INVALID_PARAMETER;
  *Out = NULL;
  if (Protocol == NULL || Protocol->Read != Ext4ReadFile) {
    DEBUG ((EFI_D_ERROR, "SFB: MARK image-map reason=foreign-file-protocol read=%p expected=%p\n",
            Protocol == NULL ? NULL : Protocol->Read, Ext4ReadFile));
    return EFI_UNSUPPORTED;
  }
  File = (EXT4_FILE *)Protocol;
  P = File->Partition;
  if (P == NULL || P->Unmounting || P->BlockIo == NULL || P->BlockIo->Media == NULL ||
      P->BlockIo->Media->ReadOnly || !P->BlockIo->Media->MediaPresent) return EFI_NOT_READY;
  OriginalMediaId = P->BlockIo->Media->MediaId;
  if (P->BlockIo->Media->BlockSize == 0 || P->BlockIo->Media->LastBlock == MAX_UINT64 ||
      P->BlockIo->Media->LastBlock + 1 > MAX_UINT64 / P->BlockIo->Media->BlockSize) return EFI_VOLUME_CORRUPTED;
  DeviceBytes = (P->BlockIo->Media->LastBlock + 1) * P->BlockIo->Media->BlockSize;
  if (P->BlockSize < 1024 || P->BlockSize > 65536 || (P->BlockSize & (P->BlockSize - 1)) != 0 ||
      P->NumberBlocks > MAX_UINT64 / P->BlockSize || P->NumberBlockGroups == 0 ||
      P->NumberBlockGroups > MAX_MAP_GROUPS || P->BlockGroups == NULL ||
      (P->DescSize != 32 && P->DescSize != 64) ||
      ((P->FeaturesIncompat & EXT4_FEATURE_INCOMPAT_64BIT) && P->DescSize != 64) ||
      P->NumberBlocks > DeviceBytes / P->BlockSize ||
      P->SuperBlock.s_first_data_block >= P->NumberBlocks || P->SuperBlock.s_blocks_per_group == 0 || P->SuperBlock.s_blocks_per_group % 8 != 0 ||
      P->SuperBlock.s_blocks_per_group > P->BlockSize * 8) {
    DEBUG ((EFI_D_ERROR, "SFB: MARK image-map reason=geometry block=%u groups=%Lu descriptor=%u\n",
            P->BlockSize, P->NumberBlockGroups, P->DescSize));
    return EFI_UNSUPPORTED;
  }
  /* Match Ext4Dxe: Android routinely leaves RECOVER set. Mapping an existing
   * initialized file neither replays the journal nor changes ext4 metadata.
   * The flag alone must not make readable boot files disappear. */
  if (P->FeaturesIncompat & ~AllowedIncompat ||
      P->FeaturesRoCompat & ~AllowedRo || P->FeaturesCompat & ~AllowedCompat) {
    DEBUG ((EFI_D_ERROR, "SFB: MARK image-map reason=features incompat=%x ro=%x compat=%x\n",
            P->FeaturesIncompat, P->FeaturesRoCompat, P->FeaturesCompat));
    return EFI_UNSUPPORTED;
  }
  if (P->SuperBlock.s_state != EXT4_FS_STATE_UNMOUNTED || P->SuperBlock.s_last_orphan != 0) {
    DEBUG ((EFI_D_ERROR, "SFB: MARK image-map reason=filesystem-state state=%x orphan=%u\n",
            P->SuperBlock.s_state, P->SuperBlock.s_last_orphan));
    return EFI_UNSUPPORTED;
  }
  if (P->NumberBlockGroups != (P->NumberBlocks - P->SuperBlock.s_first_data_block - 1) /
      P->SuperBlock.s_blocks_per_group + 1) return EFI_VOLUME_CORRUPTED;
  Status = Ext4ReadDiskIo (P, &FreshSuper, sizeof (FreshSuper), EXT4_SUPERBLOCK_OFFSET);
  if (EFI_ERROR (Status)) return Status;
  if (CompareMem (&FreshSuper, &P->SuperBlock, sizeof (FreshSuper)) != 0) return EFI_MEDIA_CHANGED;
  Status = Ext4ReadInode (P, File->InodeNum, &Fresh);
  if (EFI_ERROR (Status)) return Status;
  if (CompareMem (Fresh, File->Inode, P->InodeSize) != 0 || !Ext4FileIsReg (File) ||
      !(File->Inode->i_flags & EXT4_EXTENTS_FL) || File->Inode->i_links != 1 ||
      !Ext4ImageSizeValid (Ext4InodeSize (File->Inode))) {
    FreePool (Fresh);
    return EFI_VOLUME_CORRUPTED;
  }
  FreePool (Fresh);
  /* Benign inode hints (NOATIME, NODUMP, SYNC, etc.) do not change file bytes.
   * Direct backing-file writes cannot implement compression/encryption, inline
   * data, verity, data journaling, or immutable/append-only write semantics. */
  if (File->Inode->i_flags & (EXT4_COMPR_FL | EXT4_COMPRBLK_FL | EXT4_ECOMPR_FL |
       EXT4_IMMUTABLE_FL | EXT4_APPEND_FL | EXT4_JOURNAL_DATA_FL | EXT4_VERITY_FL |
       0x10000000U)) return EFI_UNSUPPORTED; /* INLINE_DATA */
  C = AllocateZeroPool (sizeof (*C));
  if (C == NULL) return EFI_OUT_OF_RESOURCES;
  C->Map = AllocateZeroPool (sizeof (*C->Map));
  if (C->Map == NULL) { FreePool (C); return EFI_OUT_OF_RESOURCES; }
  C->File = File;
  C->Map->Bytes = Ext4InodeSize (File->Inode);
  Status = Walk (C, (EXT4_EXTENT_HEADER *)File->Inode->i_data, sizeof (File->Inode->i_data),
                 ((EXT4_EXTENT_HEADER *)File->Inode->i_data)->eh_depth);
  if (!EFI_ERROR (Status) && C->Logical != C->Map->Bytes / P->BlockSize) Status = EFI_VOLUME_CORRUPTED;
  /* An extent must never alias a block holding its own extent tree. */
  for (I = 0; !EFI_ERROR (Status) && I < C->Map->Count; I++)
    for (J = 0; J < C->NodeCount; J++)
      if (Within (C->Nodes[J] * P->BlockSize, C->Map->Ranges[I].Physical, C->Map->Ranges[I].Bytes))
        Status = EFI_VOLUME_CORRUPTED;
  if (!EFI_ERROR (Status)) Status = CheckAllocation (C);
  if (P->BlockIo->Media->MediaId != OriginalMediaId || !P->BlockIo->Media->MediaPresent) Status = EFI_MEDIA_CHANGED;
  if (EFI_ERROR (Status)) FreePool (C->Map);
  else {
    C->Map->Parent = P->BlockIo;
    C->Map->MediaId = OriginalMediaId;
    CopyMem (C->Map->Identity, P->SuperBlock.s_uuid, 16);
    for (I = 0; I < 4; I++) {
      C->Map->Identity[16 + I] = (UINT8)(File->InodeNum >> (I * 8));
      C->Map->Identity[20 + I] = (UINT8)(File->Inode->i_generation >> (I * 8));
    }
    *Out = C->Map;
  }
  FreePool (C);
  return Status;
}
