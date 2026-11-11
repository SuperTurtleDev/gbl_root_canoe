/*
 * Host unit tests for the boot menu's config store and boot entry logic.
 * Compiles the real firmware sources against the real MdePkg headers.
 *
 * Usage: test_sfb <artifact-dir>
 * Artifacts (raw images) are checked again, independently, by test_sfb.py.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* System headers first: X64 ProcessorBind.h pushes hidden visibility. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Uefi.h>
#include <Uefi/UefiGpt.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>

#include "SuperFbConfigDisk.h"
#include "SuperFbConfigFormat.h"
#include "SuperFbEntryList.h"
#include "SuperFbSynthDisk.h"


STATIC int         mFailures;
STATIC int         mChecks;
STATIC const char  *mOutDir;

#define CHECK(Cond)                                                       \
  do {                                                                    \
    mChecks++;                                                            \
    if (!(Cond)) {                                                        \
      mFailures++;                                                        \
      fprintf (stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond);   \
    }                                                                     \
  } while (0)

STATIC
VOID
Utf16 (CHAR16 *Out, UINTN Chars, const char *In)
{
  UINTN N = 0;
  while (In[N] && N + 1 < Chars) { Out[N] = (CHAR16)(UINT8)In[N]; N++; }
  Out[N] = 0;
}

STATIC
BOOLEAN
Eq16 (CONST CHAR16 *A, const char *B)
{
  CHAR16 Tmp[512];
  Utf16 (Tmp, 512, B);
  return StrCmp (A, Tmp) == 0;
}

/* ---- fake parent disk: a sparse file with an LBA fence ------------------- */

typedef struct {
  EFI_BLOCK_IO_PROTOCOL  BlockIo;
  EFI_BLOCK_IO_MEDIA     Media;
  int                    Fd;
  /* Every transfer must stay inside [FenceLo, FenceHi]; anything else counts
   * as a violation and fails, standing in for "wrote past EndingLBA". */
  EFI_LBA                FenceLo;
  EFI_LBA                FenceHi;
  UINTN                  Violations;
  UINTN                  Writes;
  UINTN                  Flushes;
  EFI_LBA                MaxWritten;
} FAKE_DISK;

STATIC
EFI_STATUS
FakeIo (FAKE_DISK *D, EFI_LBA Lba, UINTN Size, VOID *Buf, BOOLEAN Write)
{
  UINTN Blocks = Size / D->Media.BlockSize;

  if (Size % D->Media.BlockSize != 0 || Blocks == 0) {
    return EFI_BAD_BUFFER_SIZE;
  }
  if (D->Media.IoAlign > 1 && ((UINTN)Buf % D->Media.IoAlign) != 0) {
    D->Violations++;
    return EFI_INVALID_PARAMETER;
  }
  if (Lba < D->FenceLo || Lba + Blocks - 1 > D->FenceHi) {
    D->Violations++;
    fprintf (stderr, "fence: lba %llu+%llu outside [%llu,%llu]\n",
             (unsigned long long)Lba, (unsigned long long)Blocks,
             (unsigned long long)D->FenceLo, (unsigned long long)D->FenceHi);
    return EFI_DEVICE_ERROR;
  }
  if (Write) {
    D->Writes++;
    if (Lba + Blocks - 1 > D->MaxWritten) D->MaxWritten = Lba + Blocks - 1;
    return pwrite (D->Fd, Buf, Size, (off_t)(Lba * D->Media.BlockSize)) == (ssize_t)Size
             ? EFI_SUCCESS : EFI_DEVICE_ERROR;
  }
  memset (Buf, 0, Size);
  return pread (D->Fd, Buf, Size, (off_t)(Lba * D->Media.BlockSize)) >= 0
           ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS EFIAPI FakeRead (EFI_BLOCK_IO_PROTOCOL *T, UINT32 Id, EFI_LBA L, UINTN S, VOID *B)
{ (void)Id; return FakeIo ((FAKE_DISK *)T, L, S, B, FALSE); }
STATIC EFI_STATUS EFIAPI FakeWrite (EFI_BLOCK_IO_PROTOCOL *T, UINT32 Id, EFI_LBA L, UINTN S, VOID *B)
{ (void)Id; return FakeIo ((FAKE_DISK *)T, L, S, B, TRUE); }
STATIC EFI_STATUS EFIAPI FakeFlush (EFI_BLOCK_IO_PROTOCOL *T) { ((FAKE_DISK *)T)->Flushes++; return EFI_SUCCESS; }
STATIC EFI_STATUS EFIAPI FakeReset (EFI_BLOCK_IO_PROTOCOL *T, BOOLEAN E) { (void)T; (void)E; return EFI_SUCCESS; }

STATIC
VOID
FakeOpen (FAKE_DISK *D, const char *Name, UINT32 BlockSize, UINT64 Blocks, UINT32 IoAlign)
{
  char Path[1024];

  memset (D, 0, sizeof (*D));
  snprintf (Path, sizeof (Path), "%s/%s", mOutDir, Name);
  D->Fd = open (Path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (D->Fd < 0 || ftruncate (D->Fd, (off_t)(BlockSize * Blocks)) != 0) {
    perror (Path);
    exit (2);
  }
  D->Media.MediaId = 7;
  D->Media.MediaPresent = TRUE;
  D->Media.BlockSize = BlockSize;
  D->Media.LastBlock = Blocks - 1;
  D->Media.IoAlign = IoAlign;
  D->BlockIo.Media = &D->Media;
  D->BlockIo.Reset = FakeReset;
  D->BlockIo.ReadBlocks = FakeRead;
  D->BlockIo.WriteBlocks = FakeWrite;
  D->BlockIo.FlushBlocks = FakeFlush;
  D->FenceLo = 0;
  D->FenceHi = Blocks - 1;
}

STATIC
VOID
FakeFill (FAKE_DISK *D, EFI_LBA Lba, UINT8 Byte)
{
  UINT8 *B = malloc (D->Media.BlockSize);
  memset (B, Byte, D->Media.BlockSize);
  if (pwrite (D->Fd, B, D->Media.BlockSize, (off_t)(Lba * D->Media.BlockSize)) < 0) exit (2);
  free (B);
}

STATIC
BOOLEAN
FakeIsFilled (FAKE_DISK *D, EFI_LBA Lba, UINT8 Byte)
{
  UINT8   *B = malloc (D->Media.BlockSize);
  UINTN   I;
  BOOLEAN Same = TRUE;
  if (pread (D->Fd, B, D->Media.BlockSize, (off_t)(Lba * D->Media.BlockSize)) < 0) exit (2);
  for (I = 0; I < D->Media.BlockSize; I++) Same &= (B[I] == Byte);
  free (B);
  return Same;
}

/* ---- 1. GPT range and parsing -------------------------------------------- */

STATIC
VOID
TestContainerRange (VOID)
{
  EFI_LBA S = 0, E = 0;

  /* 512-byte blocks: 1 MiB is 2048 blocks; EndingLBA stays inclusive. */
  CHECK (SfbConfigContainerRange (34, 34 + 6143, 512, 100000, &S, &E) == EFI_SUCCESS);
  CHECK (S == 34 + 2048 && E == 34 + 6143);
  /* 4 KiB blocks: 1 MiB is 256 blocks. */
  CHECK (SfbConfigContainerRange (6, 6 + 767, 4096, 100000, &S, &E) == EFI_SUCCESS);
  CHECK (S == 6 + 256 && E == 6 + 767);
  /* Exactly the minimum container after the reservation. */
  CHECK (SfbConfigContainerRange (0, 2048 + 127, 512, 100000, &S, &E) == EFI_SUCCESS);
  CHECK (E - S + 1 == 128);
  CHECK (SfbConfigContainerRange (0, 2048 + 126, 512, 100000, &S, &E) == EFI_VOLUME_FULL);
  /* Nothing past the BDS reservation. */
  CHECK (SfbConfigContainerRange (0, 2047, 512, 100000, &S, &E) == EFI_VOLUME_FULL);
  /* 1 MiB not a whole number of blocks. */
  CHECK (SfbConfigContainerRange (0, 4000, 1u << 21, 100000, &S, &E) == EFI_UNSUPPORTED);
  CHECK (SfbConfigContainerRange (0, 4000, 520, 100000, &S, &E) == EFI_INVALID_PARAMETER);
  /* Entry off the end of the disk, or reversed. */
  CHECK (SfbConfigContainerRange (10, 100001, 512, 100000, &S, &E) == EFI_VOLUME_CORRUPTED);
  CHECK (SfbConfigContainerRange (5000, 4000, 512, 100000, &S, &E) == EFI_VOLUME_CORRUPTED);
}

STATIC
VOID
PutName (EFI_PARTITION_ENTRY *E, const char *Name)
{
  UINTN I;
  for (I = 0; Name[I]; I++) E->PartitionName[I] = (CHAR16)Name[I];
}

/* A primary GPT in RAM: header block and 128-entry array. */
STATIC
VOID
BuildGpt (UINT8 *Header, UINT8 *Entries, UINT32 BlockSize, EFI_LBA LastBlock)
{
  EFI_PARTITION_TABLE_HEADER *H = (EFI_PARTITION_TABLE_HEADER *)Header;
  EFI_PARTITION_ENTRY        *E = (EFI_PARTITION_ENTRY *)Entries;
  STATIC CONST EFI_GUID      Type = { 0x11111111, 0, 0, { 1 } };
  UINTN                      EntryBlocks = (128 * 128 + BlockSize - 1) / BlockSize;

  memset (Header, 0, BlockSize);
  memset (Entries, 0, 128 * 128);

  E[0].PartitionTypeGUID = Type; E[0].StartingLBA = 100; E[0].EndingLBA = 199; PutName (&E[0], "esp");
  E[1].PartitionTypeGUID = Type; E[1].StartingLBA = 200; E[1].EndingLBA = 200 + 1023; PutName (&E[1], "efisp");
  E[3].PartitionTypeGUID = Type; E[3].StartingLBA = 2000; E[3].EndingLBA = 2999; PutName (&E[3], "efispx");

  H->Header.Signature = EFI_PTAB_HEADER_ID;
  H->Header.Revision = 0x00010000;
  H->Header.HeaderSize = 92;
  H->MyLBA = 1;
  H->AlternateLBA = LastBlock;
  H->FirstUsableLBA = 2 + EntryBlocks;
  H->LastUsableLBA = LastBlock - 1 - EntryBlocks;
  H->PartitionEntryLBA = 2;
  H->NumberOfPartitionEntries = 128;
  H->SizeOfPartitionEntry = 128;
  H->PartitionEntryArrayCRC32 = CalculateCrc32 (Entries, 128 * 128);
  H->Header.CRC32 = 0;
  H->Header.CRC32 = CalculateCrc32 (H, 92);
}

STATIC
VOID
TestGpt (VOID)
{
  STATIC UINT8 Header[4096];
  STATIC UINT8 Entries[128 * 128];
  EFI_LBA      EntriesLba = 0;
  UINTN        EntriesBytes = 0;
  EFI_LBA      S = 0, E = 0;
  EFI_PARTITION_TABLE_HEADER *H = (EFI_PARTITION_TABLE_HEADER *)Header;

  BuildGpt (Header, Entries, 4096, 10000);
  CHECK (SfbConfigGptCheckHeader (Header, 4096, 10000, &EntriesLba, &EntriesBytes) == EFI_SUCCESS);
  CHECK (EntriesLba == 2 && EntriesBytes == 128 * 128);
  CHECK (SfbConfigGptFindContainer (Header, Entries, L"efisp", 4096, 10000, &S, &E) == EFI_SUCCESS);
  /* Exact name match only ("efispx" is not "efisp"), bounds from the entry. */
  CHECK (S == 200 + 256 && E == 200 + 1023);
  CHECK (SfbConfigGptFindContainer (Header, Entries, L"nope", 4096, 10000, &S, &E) == EFI_NOT_FOUND);

  /* Corrupt entry array: CRC refuses it. */
  Entries[200] ^= 1;
  CHECK (SfbConfigGptFindContainer (Header, Entries, L"efisp", 4096, 10000, &S, &E) == EFI_CRC_ERROR);
  Entries[200] ^= 1;

  /* Corrupt header CRC. */
  H->LastUsableLBA++;
  CHECK (SfbConfigGptCheckHeader (Header, 4096, 10000, &EntriesLba, &EntriesBytes) == EFI_CRC_ERROR);
  H->LastUsableLBA--;
  /* Bad signature. */
  Header[0] ^= 0xFF;
  CHECK (SfbConfigGptCheckHeader (Header, 4096, 10000, &EntriesLba, &EntriesBytes) == EFI_VOLUME_CORRUPTED);
  Header[0] ^= 0xFF;

  /* efisp entry extending past the disk is refused. */
  BuildGpt (Header, Entries, 4096, 1100);
  CHECK (SfbConfigGptFindContainer (Header, Entries, L"efisp", 4096, 1100, &S, &E) == EFI_VOLUME_CORRUPTED);
}

/* ---- 2. bounded Block I/O ------------------------------------------------ */

STATIC
VOID
TestDiskBounds (UINT32 ParentBlockSize, UINT32 IoAlign, const char *Name)
{
  FAKE_DISK        Parent;
  SFB_CONFIG_DISK  Disk;
  UINT32           PerBlock = ParentBlockSize / 512;
  EFI_LBA          Start = 300, End = 300 + 63;       /* 64 parent blocks */
  UINT64           Sectors = 64ULL * PerBlock;
  UINT8            *Buf = malloc (8192 + 1);
  UINT8            *Unaligned = Buf + 1;
  UINT8            Back[1024];

  FakeOpen (&Parent, Name, ParentBlockSize, 1000, IoAlign);
  FakeFill (&Parent, Start - 1, 0xA5);
  FakeFill (&Parent, End + 1, 0x5A);
  /* The container may only ever touch its own blocks. */
  Parent.FenceLo = Start;
  Parent.FenceHi = End;

  CHECK (SfbConfigDiskInit (&Disk, &Parent.BlockIo, Start, End) == EFI_SUCCESS);
  CHECK (Disk.Sectors == Sectors);
  CHECK (Disk.Media.BlockSize == 512 && Disk.Media.LastBlock == Sectors - 1);
  CHECK (Disk.Media.LogicalPartition);

  /* Last legal sector: written and read back. */
  memset (Unaligned, 0xC3, 512);
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, Sectors - 1, 512, Unaligned) == EFI_SUCCESS);
  memset (Back, 0, 512);
  CHECK (Disk.BlockIo.ReadBlocks (&Disk.BlockIo, Disk.Media.MediaId, Sectors - 1, 512, Back) == EFI_SUCCESS);
  CHECK (memcmp (Back, Unaligned, 512) == 0);
  CHECK (Parent.MaxWritten == End);

  /* First sector past the end, and a run straddling the end: refused before
   * any parent I/O. */
  Parent.Writes = 0;
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, Sectors, 512, Unaligned) == EFI_INVALID_PARAMETER);
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, Sectors - 1, 1024, Unaligned) == EFI_INVALID_PARAMETER);
  CHECK (Disk.BlockIo.ReadBlocks (&Disk.BlockIo, Disk.Media.MediaId, Sectors, 512, Back) == EFI_INVALID_PARAMETER);
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, (EFI_LBA)-1, 512, Unaligned) == EFI_INVALID_PARAMETER);
  CHECK (Parent.Writes == 0);

  /* Wrong media id and partial sectors. */
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId + 1, 0, 512, Unaligned) == EFI_MEDIA_CHANGED);
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, 0, 100, Unaligned) == EFI_BAD_BUFFER_SIZE);

  /* A run crossing a parent block boundary, from a misaligned buffer. */
  {
    UINTN I;
    for (I = 0; I < 8192; I++) Unaligned[I] = (UINT8)(I * 7 + 1);
    CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, PerBlock - 1, 8192, Unaligned) == EFI_SUCCESS);
    {
      UINT8 *Check = malloc (8192);
      CHECK (Disk.BlockIo.ReadBlocks (&Disk.BlockIo, Disk.Media.MediaId, PerBlock - 1, 8192, Check) == EFI_SUCCESS);
      CHECK (memcmp (Check, Unaligned, 8192) == 0);
      free (Check);
    }
  }

  /* First sector maps to StartLba, i.e. StartingLBA + 1 MiB. */
  memset (Unaligned, 0x77, 512);
  CHECK (Disk.BlockIo.WriteBlocks (&Disk.BlockIo, Disk.Media.MediaId, 0, 512, Unaligned) == EFI_SUCCESS);
  {
    UINT8 Raw[512];
    CHECK (pread (Parent.Fd, Raw, 512, (off_t)(Start * ParentBlockSize)) == 512);
    CHECK (Raw[0] == 0x77 && Raw[511] == 0x77);
  }

  CHECK (Disk.BlockIo.FlushBlocks (&Disk.BlockIo) == EFI_SUCCESS && Parent.Flushes == 1);

  /* Neighbours untouched, nothing tried to cross the fence. */
  CHECK (FakeIsFilled (&Parent, Start - 1, 0xA5));
  CHECK (FakeIsFilled (&Parent, End + 1, 0x5A));
  CHECK (Parent.Violations == 0);

  /* Init refuses a range off the disk. */
  {
    SFB_CONFIG_DISK Bad;
    CHECK (SfbConfigDiskInit (&Bad, &Parent.BlockIo, 900, 1000) == EFI_INVALID_PARAMETER);
  }

  SfbConfigDiskFree (&Disk);
  close (Parent.Fd);
  free (Buf);
}

/* ---- 3. formatter -------------------------------------------------------- */

STATIC
UINT32
Le16 (CONST UINT8 *P)
{
  return P[0] | (P[1] << 8);
}

STATIC
UINT32
Le32 (CONST UINT8 *P)
{
  return Le16 (P) | (Le16 (P + 2) << 16);
}

STATIC
VOID
TestPlan (VOID)
{
  SFB_FAT_LAYOUT L;
  UINT8          Bs[512];

  CHECK (SfbFatPlan (127, &L) == EFI_VOLUME_FULL);
  CHECK (SfbFatPlan (128, &L) == EFI_SUCCESS && L.FatType == 12);

  /* QEMU efisp: 3 MiB partition, 2 MiB container. */
  CHECK (SfbFatPlan (4096, &L) == EFI_SUCCESS);
  CHECK (L.FatType == 12 && L.Clusters <= 4084 && L.SectorsPerCluster == 1);
  CHECK (L.DataStart + L.Clusters * L.SectorsPerCluster <= 4096);

  CHECK (SfbFatPlan (16384, &L) == EFI_SUCCESS);          /* 8 MiB */
  CHECK (L.FatType == 16 && L.Clusters >= 4085 && L.Clusters <= 65524);
  CHECK (L.FatSectors * 512 >= (L.Clusters + 2) * 2);

  CHECK (SfbFatPlan (1228800, &L) == EFI_SUCCESS);        /* 600 MiB */
  CHECK (L.FatType == 32 && L.Clusters >= 65525 && L.SectorsPerCluster == 8);
  CHECK (L.ReservedSectors == 32 && L.RootEntries == 0);
  CHECK (L.FatSectors * 512 >= (L.Clusters + 2) * 4);

  /* Every size in a sweep lands in its type's cluster range. */
  {
    UINT64 Sectors;
    int    Bad = 0;
    for (Sectors = 128; Sectors < 2200000; Sectors = Sectors * 11 / 10 + 1) {
      if (SfbFatPlan (Sectors, &L) != EFI_SUCCESS) { Bad++; continue; }
      if (L.FatType == 12 && !(L.Clusters <= 4084)) Bad++;
      if (L.FatType == 16 && !(L.Clusters >= 4085 && L.Clusters <= 65524)) Bad++;
      if (L.FatType == 32 && !(L.Clusters >= 65525)) Bad++;
      if (L.DataStart + (UINT64)L.Clusters * L.SectorsPerCluster > Sectors) Bad++;
      {
        UINT64 Need = L.FatType == 12 ? ((UINT64)(L.Clusters + 2) * 3 + 1) / 2
                                      : (UINT64)(L.Clusters + 2) * (L.FatType / 8);
        if ((UINT64)L.FatSectors * 512 < Need) Bad++;
      }
    }
    CHECK (Bad == 0);
  }

  /* BPB encoding. */
  CHECK (SfbFatPlan (16384, &L) == EFI_SUCCESS);
  SfbFatBuildBootSector (&L, 0x12345678, Bs);
  CHECK (Bs[510] == 0x55 && Bs[511] == 0xAA);
  CHECK (Le16 (Bs + 11) == 512 && Bs[13] == L.SectorsPerCluster);
  CHECK (Le16 (Bs + 14) == L.ReservedSectors && Bs[16] == 2);
  CHECK (Le16 (Bs + 17) == 512 && Le16 (Bs + 19) == 16384 && Le32 (Bs + 32) == 0);
  CHECK (Bs[21] == 0xF8 && Le16 (Bs + 22) == L.FatSectors);
  CHECK (Bs[38] == 0x29 && Le32 (Bs + 39) == 0x12345678);
  CHECK (memcmp (Bs + 43, "SFBCONFIG  ", 11) == 0 && memcmp (Bs + 54, "FAT16   ", 8) == 0);
  CHECK (SfbFatBootSectorUsable (Bs, 16384));
  CHECK (!SfbFatBootSectorUsable (Bs, 16383));            /* larger than the container */

  CHECK (SfbFatPlan (1228800, &L) == EFI_SUCCESS);
  SfbFatBuildBootSector (&L, 1, Bs);
  CHECK (Le16 (Bs + 19) == 0 && Le32 (Bs + 32) == 1228800 && Le16 (Bs + 22) == 0);
  CHECK (Le32 (Bs + 36) == L.FatSectors && Le32 (Bs + 44) == 2 && Le16 (Bs + 48) == 1 && Le16 (Bs + 50) == 6);
  CHECK (Bs[66] == 0x29 && memcmp (Bs + 82, "FAT32   ", 8) == 0);

  /* Garbage, zeros and a foreign sector size are not usable. */
  memset (Bs, 0, 512);
  CHECK (!SfbFatBootSectorUsable (Bs, 16384));
  memset (Bs, 0xFF, 512);
  CHECK (!SfbFatBootSectorUsable (Bs, 16384));
}

/* Format a container of ContainerBytes behind a 1 MiB BDS area on a disk of
 * ParentBlockSize blocks, leaving the raw parent image for the Python side. */
STATIC
VOID
TestFormat (const char *Name, UINT32 ParentBlockSize, UINT64 ContainerBytes, UINT8 WantType)
{
  FAKE_DISK        Parent;
  SFB_CONFIG_DISK  Disk;
  EFI_LBA          PartStart = 2048 / (ParentBlockSize / 512) ;  /* partition at 1 MiB */
  EFI_LBA          Start = PartStart + SIZE_1MB / ParentBlockSize;
  EFI_LBA          End = Start + ContainerBytes / ParentBlockSize - 1;
  UINT8            Sector[512];
  SFB_FAT_LAYOUT   L;

  FakeOpen (&Parent, Name, ParentBlockSize, End + 1 + 16, 4096);
  FakeFill (&Parent, PartStart, 0xEE);      /* first block of the "BDS image" */
  FakeFill (&Parent, End + 1, 0xDD);
  Parent.FenceLo = Start;
  Parent.FenceHi = End;

  CHECK (SfbConfigDiskInit (&Disk, &Parent.BlockIo, Start, End) == EFI_SUCCESS);
  CHECK (SfbFatFormat (&Disk.BlockIo) == EFI_SUCCESS);
  CHECK (Parent.Violations == 0 && Parent.Flushes >= 1);
  CHECK (FakeIsFilled (&Parent, PartStart, 0xEE));
  CHECK (FakeIsFilled (&Parent, End + 1, 0xDD));

  CHECK (Disk.BlockIo.ReadBlocks (&Disk.BlockIo, Disk.Media.MediaId, 0, 512, Sector) == EFI_SUCCESS);
  CHECK (SfbFatBootSectorUsable (Sector, Disk.Sectors));
  CHECK (SfbFatPlan (Disk.Sectors, &L) == EFI_SUCCESS && L.FatType == WantType);
  CHECK (memcmp (Sector + (WantType == 32 ? 82 : 54), WantType == 12 ? "FAT12   " :
                 WantType == 16 ? "FAT16   " : "FAT32   ", 8) == 0);

  /* Tell the Python side where the container sits in this image. */
  {
    char Path[1024];
    FILE *F;
    snprintf (Path, sizeof (Path), "%s/%s.meta", mOutDir, Name);
    F = fopen (Path, "w");
    fprintf (F, "%llu %llu %u %u\n", (unsigned long long)(Start * ParentBlockSize),
             (unsigned long long)ContainerBytes, ParentBlockSize, WantType);
    fclose (F);
  }

  SfbConfigDiskFree (&Disk);
  close (Parent.Fd);
}

/* ---- 4. ENTRIES.CFG ------------------------------------------------------- */

STATIC
VOID
TestParse (VOID)
{
  SFB_ENTRY_SPEC S;
  CHAR8          Line[SFB_ENTRY_LINE_BYTES];
  CHAR16         Child[SFB_NAME_CHARS];
  BOOLEAN        Folder;

  CHECK (SfbEntryParseLine ("Android Tools\\Reboot Tools:(1,1)\\efisp\\tools\\RebootTools.efi", &S));
  CHECK (Eq16 (S.Name, "Android Tools\\Reboot Tools"));
  CHECK (S.HasAddress && S.Disk == 1 && S.Part == 1);
  CHECK (Eq16 (S.Path, "\\efisp\\tools\\RebootTools.efi") && S.Args[0] == 0);

  /* Disk omitted: the volume holding the file. */
  CHECK (SfbEntryParseLine ("Android Tools\\Reboot Tools:\\efisp\\tools\\RebootTools.efi", &S));
  CHECK (!S.HasAddress && Eq16 (S.Path, "\\efisp\\tools\\RebootTools.efi"));

  /* Whitespace, '/' separators, duplicate separators, multi-digit address, args. */
  CHECK (SfbEntryParseLine ("  Linux \\ Mainline :( 12 , 34 )/EFI//linux/Image.efi  --dtb a.dtb   root=/dev/sda ", &S));
  CHECK (Eq16 (S.Name, "Linux\\Mainline"));
  CHECK (S.HasAddress && S.Disk == 12 && S.Part == 34);
  CHECK (Eq16 (S.Path, "\\EFI\\linux\\Image.efi"));
  CHECK (Eq16 (S.Args, "--dtb a.dtb   root=/dev/sda"));

  /* Rejected lines. */
  CHECK (!SfbEntryParseLine ("", &S));
  CHECK (!SfbEntryParseLine ("   # comment:\\x.efi", &S));
  CHECK (!SfbEntryParseLine ("no separator", &S));
  CHECK (!SfbEntryParseLine (":\\x.efi", &S));
  CHECK (!SfbEntryParseLine ("Name:", &S));
  CHECK (!SfbEntryParseLine ("Name:\\", &S));
  CHECK (!SfbEntryParseLine ("Name:(1,x)\\a.efi", &S));
  CHECK (!SfbEntryParseLine ("Name:(1)\\a.efi", &S));
  CHECK (!SfbEntryParseLine ("Name:(1,2\\a.efi", &S));
  /* Legacy markers are not syntax any more: '$'/'%' are part of the name. */
  CHECK (SfbEntryParseLine ("$Tools:\\t.efi", &S) && Eq16 (S.Name, "$Tools"));

  /* Round trip, address and args preserved. */
  CHECK (SfbEntryParseLine ("A\\B:(0,2)\\EFI\\BOOT\\BOOTAA64.EFI -v", &S));
  CHECK (SfbEntryFormatLine (&S, Line, sizeof (Line)));
  CHECK (strcmp (Line, "A\\B:(0,2)\\EFI\\BOOT\\BOOTAA64.EFI -v\n") == 0);
  CHECK (SfbEntryParseLine ("Cee:\\x.efi", &S) && SfbEntryFormatLine (&S, Line, sizeof (Line)));
  CHECK (strcmp (Line, "Cee:\\x.efi\n") == 0);
  CHECK (!SfbEntryFormatLine (&S, Line, 5));

  /* Several lines in file order, CRLF and blank lines tolerated. */
  {
    const char  *Text = "First:\\a.efi\r\n\r\n# skip\nSecond:(0,1)\\b.efi\nThird:\\c.efi";
    CONST CHAR8 *Cursor = Text;
    const char  *Want[] = { "First", "Second", "Third" };
    int         Got = 0;
    while (SfbEntryNextLine (&Cursor, Line, sizeof (Line))) {
      if (SfbEntryParseLine (Line, &S)) {
        CHECK (Got < 3 && Eq16 (S.Name, Want[Got]));
        Got++;
      }
    }
    CHECK (Got == 3);
  }

  /* Name hierarchy. */
  CHECK (SfbEntryNameChild (L"Android Tools\\Reboot Tools", L"", Child, SFB_NAME_CHARS, &Folder));
  CHECK (Eq16 (Child, "Android Tools") && Folder);
  CHECK (SfbEntryNameChild (L"Android Tools\\Reboot Tools", L"Android Tools", Child, SFB_NAME_CHARS, &Folder));
  CHECK (Eq16 (Child, "Reboot Tools") && !Folder);
  CHECK (!SfbEntryNameChild (L"Android Toolsx\\R", L"Android Tools", Child, SFB_NAME_CHARS, &Folder));
  CHECK (!SfbEntryNameChild (L"Android Tools", L"Android Tools", Child, SFB_NAME_CHARS, &Folder));
  CHECK (SfbEntryNameChild (L"EFIBOOT (0,2)", L"", Child, SFB_NAME_CHARS, &Folder) && !Folder);
}

STATIC
VOID
MakeEntry (SFB_BOOT_ENTRY *E, const char *Line)
{
  memset (E, 0, sizeof (*E));
  if (!SfbEntryParseLine (Line, &E->Spec)) {
    fprintf (stderr, "bad fixture %s\n", Line);
    exit (2);
  }
  /* Owned allocation, so leaks/double frees show up under sanitizers. */
  E->DevicePath = AllocateZeroPool (4);
}

STATIC
BOOLEAN
ListIs (SFB_ENTRY_LIST *L, const char *const *Names, UINTN Count)
{
  UINTN I;
  if (L->Count != Count) return FALSE;
  for (I = 0; I < Count; I++) {
    if (!Eq16 (L->Entry[I].Spec.Name, Names[I])) return FALSE;
  }
  return TRUE;
}

STATIC
VOID
TestSync (VOID)
{
  STATIC SFB_ENTRY_LIST List, Found;
  BOOLEAN               Valid[SFB_MAX_ENTRIES];
  SFB_BOOT_ENTRY        E;

  memset (&List, 0, sizeof (List));
  memset (&Found, 0, sizeof (Found));
  MakeEntry (&E, "A:(0,1)\\a.efi"); CHECK (SfbEntryListAppend (&List, &E));
  MakeEntry (&E, "B:(0,1)\\b.efi"); CHECK (SfbEntryListAppend (&List, &E));
  MakeEntry (&E, "C:(0,2)\\c.efi"); CHECK (SfbEntryListAppend (&List, &E));
  MakeEntry (&E, "Dee:\\d.efi");    CHECK (SfbEntryListAppend (&List, &E));
  /* Same target under another name or case is a duplicate. */
  MakeEntry (&E, "A again:(0,1)\\A.EFI"); CHECK (!SfbEntryListAppend (&List, &E));
  CHECK (E.DevicePath == NULL);
  /* Different args make a different entry. */
  MakeEntry (&E, "A args:(0,1)\\a.efi x"); CHECK (SfbEntryListAppend (&List, &E));

  /* Scan found C (already present), E and F (new), A under a new name. */
  MakeEntry (&E, "E:(1,0)\\e.efi");    CHECK (SfbEntryListAppend (&Found, &E));
  MakeEntry (&E, "C new:(0,2)\\c.efi"); CHECK (SfbEntryListAppend (&Found, &E));
  MakeEntry (&E, "F:(0,3)\\f.efi");    CHECK (SfbEntryListAppend (&Found, &E));
  MakeEntry (&E, "A2:(0,1)\\a.efi");   CHECK (SfbEntryListAppend (&Found, &E));

  /* B became invalid. */
  Valid[0] = TRUE; Valid[1] = FALSE; Valid[2] = TRUE; Valid[3] = TRUE; Valid[4] = TRUE;
  CHECK (SfbEntryListSync (&List, Valid, &Found));
  {
    const char *const Want[] = { "A", "C", "Dee", "A args", "E", "F" };
    CHECK (ListIs (&List, Want, 6));
  }
  CHECK (Found.Count == 0);

  /* Nothing new and all valid: unchanged. */
  {
    UINTN I;
    for (I = 0; I < List.Count; I++) Valid[I] = TRUE;
    CHECK (!SfbEntryListSync (&List, Valid, &Found));
  }

  /* Priority edits. */
  SfbEntryListMove (&List, 5, 0);
  {
    const char *const Want[] = { "F", "A", "C", "Dee", "A args", "E" };
    CHECK (ListIs (&List, Want, 6));
  }
  SfbEntryListMove (&List, 0, 2);
  {
    const char *const Want[] = { "A", "C", "F", "Dee", "A args", "E" };
    CHECK (ListIs (&List, Want, 6));
  }
  SfbEntryListRemove (&List, 1);
  {
    const char *const Want[] = { "A", "F", "Dee", "A args", "E" };
    CHECK (ListIs (&List, Want, 5));
  }

  /* Capacity. */
  {
    UINTN I;
    char  Line[64];
    SfbEntryListFree (&List);
    for (I = 0; I < SFB_MAX_ENTRIES; I++) {
      snprintf (Line, sizeof (Line), "N%u:\\%u.efi", (unsigned)I, (unsigned)I);
      MakeEntry (&E, Line);
      CHECK (SfbEntryListAppend (&List, &E));
    }
    MakeEntry (&E, "Over:\\over.efi");
    CHECK (!SfbEntryListAppend (&List, &E));
    /* A full list still serializes within the file limit. */
    {
      CHAR8 *Text = calloc (1, SFB_ENTRIES_MAX_BYTES + 1);
      UINTN Size = 0;
      for (I = 0; I < List.Count; I++) {
        CHECK (SfbEntryFormatLine (&List.Entry[I].Spec, Text + Size, SFB_ENTRIES_MAX_BYTES + 1 - Size));
        Size += strlen (Text + Size);
      }
      free (Text);
    }
  }
  SfbEntryListFree (&List);
}

/* Boot in order: only non-PE/COFF entries are dropped, one store each. */
typedef struct {
  char   Launched[16][16];
  UINTN  LaunchCount;
  char   Stored[8][256];
  UINTN  StoreCount;
} BOOT_LOG;

STATIC
EFI_STATUS
FakeLaunch (CONST SFB_BOOT_ENTRY *Entry, BOOLEAN *NotPeImage, VOID *Ctx)
{
  BOOT_LOG *Log = Ctx;
  UINTN    I;
  for (I = 0; Entry->Spec.Name[I] && I < 15; I++) Log->Launched[Log->LaunchCount][I] = (char)Entry->Spec.Name[I];
  Log->LaunchCount++;
  /* Fixture convention: the path names the outcome. */
  if (Eq16 (Entry->Spec.Path, "\\notpe.efi")) { *NotPeImage = TRUE; return EFI_UNSUPPORTED; }
  if (Eq16 (Entry->Spec.Path, "\\missing.efi")) return EFI_NOT_FOUND;
  if (Eq16 (Entry->Spec.Path, "\\denied.efi")) return EFI_SECURITY_VIOLATION;
  if (Eq16 (Entry->Spec.Path, "\\nomem.efi")) return EFI_OUT_OF_RESOURCES;
  return EFI_SUCCESS;   /* started and returned */
}

STATIC
EFI_STATUS
FakeStore (CONST SFB_ENTRY_LIST *List, VOID *Ctx)
{
  BOOT_LOG *Log = Ctx;
  UINTN    I;
  CHAR8    *Out = Log->Stored[Log->StoreCount++];
  Out[0] = 0;
  for (I = 0; I < List->Count; I++) {
    CHAR8 Line[SFB_ENTRY_LINE_BYTES];
    SfbEntryFormatLine (&List->Entry[I].Spec, Line, sizeof (Line));
    strncat (Out, Line, 255 - strlen (Out));
  }
  return EFI_SUCCESS;
}

STATIC
VOID
TestBootInOrder (VOID)
{
  STATIC SFB_ENTRY_LIST List;
  STATIC BOOT_LOG       Log;
  SFB_BOOT_ENTRY        E;
  const char           *Fixture[] = {
    "P1:\\notpe.efi", "Miss:\\missing.efi", "Sec:\\denied.efi", "P2:(0,1)\\notpe.efi",
    "Mem:\\nomem.efi", "OK:\\ok.efi"
  };
  UINTN I;

  memset (&List, 0, sizeof (List));
  memset (&Log, 0, sizeof (Log));
  for (I = 0; I < 6; I++) { MakeEntry (&E, Fixture[I]); CHECK (SfbEntryListAppend (&List, &E)); }

  SfbEntryListBootInOrder (&List, FakeLaunch, FakeStore, &Log);

  CHECK (Log.LaunchCount == 6);
  for (I = 0; I < 6; I++) {
    char Want[16];
    snprintf (Want, sizeof (Want), "%.*s", (int)(strchr (Fixture[I], ':') - Fixture[I]), Fixture[I]);
    CHECK (strcmp (Log.Launched[I], Want) == 0);
  }
  CHECK (Log.StoreCount == 2);
  CHECK (strcmp (Log.Stored[0], "Miss:\\missing.efi\nSec:\\denied.efi\nP2:(0,1)\\notpe.efi\nMem:\\nomem.efi\nOK:\\ok.efi\n") == 0);
  CHECK (strcmp (Log.Stored[1], "Miss:\\missing.efi\nSec:\\denied.efi\nMem:\\nomem.efi\nOK:\\ok.efi\n") == 0);
  {
    const char *const Want[] = { "Miss", "Sec", "Mem", "OK" };
    CHECK (ListIs (&List, Want, 4));
  }
  SfbEntryListFree (&List);
}

/* ---- 5. USB export ------------------------------------------------------- */

STATIC
VOID
TestExport (VOID)
{
  FAKE_DISK        Parent;
  SFB_CONFIG_DISK  Disk;
  SFB_SYNTH_DISK   *Usb = NULL;
  EFI_LBA          Start = 2048 + 2048, End = Start + 4096 - 1;   /* 512 B blocks, 2 MiB */
  STATIC CONST EFI_GUID Esp = { 0xC12A7328, 0xF81F, 0x11D2, { 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B } };
  UINT8            *Image;
  UINT64           Bytes;
  UINT64           Lba;
  char             Path[1024];
  FILE             *F;

  FakeOpen (&Parent, "export-parent.img", 512, End + 1 + 64, 1);
  Parent.FenceLo = Start;
  Parent.FenceHi = End;
  CHECK (SfbConfigDiskInit (&Disk, &Parent.BlockIo, Start, End) == EFI_SUCCESS);
  CHECK (SfbFatFormat (&Disk.BlockIo) == EFI_SUCCESS);

  CHECK (SfbSynthDiskCreate (&Disk.BlockIo, 0, Disk.Media.LastBlock + 1, &Esp, L"efisp", FALSE, &Usb) == EFI_SUCCESS);
  if (Usb == NULL) return;
  CHECK (Usb->Media.BlockSize == 512);
  CHECK (Usb->DataStartLba == 34 && Usb->DataBlocks == 4096);
  CHECK (Usb->Media.LastBlock == 34 + 4096 + 32);

  Bytes = (Usb->Media.LastBlock + 1) * 512;
  Image = malloc (Bytes);
  for (Lba = 0; Lba <= Usb->Media.LastBlock; Lba += 8) {
    UINTN N = (UINTN)MIN (8, Usb->Media.LastBlock + 1 - Lba);
    CHECK (Usb->BlockIo.ReadBlocks (&Usb->BlockIo, Usb->Media.MediaId, Lba, N * 512, Image + Lba * 512) == EFI_SUCCESS);
  }
  /* The data area is the container, byte for byte. */
  {
    UINT8 Sector0[512];
    CHECK (Disk.BlockIo.ReadBlocks (&Disk.BlockIo, Disk.Media.MediaId, 0, 512, Sector0) == EFI_SUCCESS);
    CHECK (memcmp (Image + 34 * 512, Sector0, 512) == 0);
  }
  /* A host write into the GPT area is swallowed; data writes land in the
   * container and nowhere else. */
  {
    UINT8 Junk[512];
    memset (Junk, 0x99, sizeof (Junk));
    CHECK (Usb->BlockIo.WriteBlocks (&Usb->BlockIo, Usb->Media.MediaId, 1, 512, Junk) == EFI_SUCCESS);
    CHECK (Usb->BlockIo.WriteBlocks (&Usb->BlockIo, Usb->Media.MediaId, 34 + 4095, 512, Junk) == EFI_SUCCESS);
    CHECK (Usb->BlockIo.WriteBlocks (&Usb->BlockIo, Usb->Media.MediaId, Usb->Media.LastBlock + 1, 512, Junk) != EFI_SUCCESS);
    CHECK (Parent.MaxWritten == End && Parent.Violations == 0);
  }

  snprintf (Path, sizeof (Path), "%s/export.img", mOutDir);
  F = fopen (Path, "wb");
  fwrite (Image, 1, Bytes, F);
  fclose (F);
  free (Image);

  SfbSynthDiskDestroy (Usb);
  SfbConfigDiskFree (&Disk);
  close (Parent.Fd);
}

int
main (int argc, char **argv)
{
  if (argc != 2) {
    fprintf (stderr, "usage: %s <artifact-dir>\n", argv[0]);
    return 2;
  }
  mOutDir = argv[1];

  TestContainerRange ();
  TestGpt ();
  TestDiskBounds (512, 1, "bounds-512.img");
  TestDiskBounds (4096, 4096, "bounds-4096.img");
  TestPlan ();
  TestFormat ("fat12.img", 4096, 2 * SIZE_1MB, 12);
  TestFormat ("fat16.img", 512, 8 * SIZE_1MB, 16);
  TestFormat ("fat32.img", 4096, 600ULL * SIZE_1MB, 32);
  TestParse ();
  TestSync ();
  TestBootInOrder ();
  TestExport ();

  printf ("%d checks, %d failures\n", mChecks, mFailures);
  return mFailures == 0 ? 0 : 1;
}
