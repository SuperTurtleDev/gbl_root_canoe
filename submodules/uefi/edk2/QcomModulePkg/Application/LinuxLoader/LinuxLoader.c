/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 - 2025 Qualcomm Innovation Center, Inc. All rights
 *  reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials provided
 *        with the distribution.
 *
 *      * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *        contributors may be used to endorse or promote products derived
 *        from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 *  HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 *  OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 *  DAMAGE.
 */

#include "AutoGen.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include "Library/ThreadStack.h"
#include <Protocol/EFICardInfo.h>
#include <Protocol/SimpleTextIn.h>
#include "SuperFbMenu.h"
#include <Library/SfbConfigStore.h>

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

/**
  Linux Loader Application EntryPoint

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.

 **/
/*
 * 开机时扫描音量上键（WaitForVolumeDownKey 的镜像）。
 *
 * 先清空输入缓冲区，再用 WaitForEvent 在超时窗口内等待一次真正的音量上键。
 * 关键在于：非目标按键（尤其是开机时按住、随后松开的电源键）会被跳过并继续
 * 等待，而不是结束扫描——所以电源键既不会被误当成输入，也不会遮挡音量键。
 *
 * @param TimeoutMs   扫描窗口（毫秒）
 * @return TRUE(1)     检测到音量上键
 * @return FALSE(0)    超时未检测到
 */
STATIC UINT8
WaitForVolumeUpKey (IN UINT32 TimeoutMs)
{
  EFI_STATUS    Status;
  EFI_EVENT     TimerEvent;
  EFI_EVENT     WaitList[2];
  UINTN         EventIndex;
  EFI_INPUT_KEY Key;
  UINT8         KeyDetected = 0;

  /* 先清空输入缓冲区 */
  gST->ConIn->Reset (gST->ConIn, FALSE);

  /* 创建定时器事件 */
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_CALLBACK,
                  NULL,
                  NULL,
                  &TimerEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "CreateEvent Timer failed: %r\n", Status));
    return FALSE;
  }

  /* 设置定时器：一次性触发，单位为 100ns */
  Status = gBS->SetTimer (
                  TimerEvent,
                  TimerRelative,
                  (UINT64)TimeoutMs * 10000   /* ms -> 100ns */
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "SetTimer failed: %r\n", Status));
    gBS->CloseEvent (TimerEvent);
    return FALSE;
  }

  /* 等待事件列表：按键事件 或 定时器超时 */
  WaitList[0] = gST->ConIn->WaitForKey;
  WaitList[1] = TimerEvent;

  while (TRUE) {
    Status = gBS->WaitForEvent (2, WaitList, &EventIndex);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "WaitForEvent failed: %r\n", Status));
      break;
    }

    if (EventIndex == 0) {
      /* 按键事件触发 */
      Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
      if (!EFI_ERROR (Status)) {
        DEBUG ((EFI_D_INFO, "Key detected: ScanCode=0x%x, UnicodeChar=0x%x\n",
                Key.ScanCode, Key.UnicodeChar));

        if (Key.ScanCode == SCAN_UP) { /* recovery / boot menu key */
          /* 检测到音量上键 */
          KeyDetected = 1;
          break;
        }
        /* 不是目标按键（电源键/音量下键等），忽略并继续等待 */
        DEBUG ((EFI_D_INFO, "Not volume up key, continue waiting...\n"));
      }
    } else {
      /* 定时器超时 */
      DEBUG ((EFI_D_INFO, "Timeout: %d ms expired, no volume up key\n",
              TimeoutMs));
      break;
    }
  }

  /* 清理定时器事件 */
  gBS->CloseEvent (TimerEvent);

  return KeyDetected;
}

EFI_STATUS EFIAPI  __attribute__ ( (no_sanitize ("safe-stack")))
LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{

  EFI_STATUS Status;

   /* Update stack check guard with random value for better security */
  /* SilentMode Boot */
  /* MultiSlot Boot */
  /* Flashless Boot */
  /* set ROT, BootState and VBH only once per boot*/

  /* RED = entry point reached */

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n",
         (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n",
         (UINTN)LinuxLoaderEntry));

  Status = InitThreadUnsafeStack ();

  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n",
            Status));
    goto stack_guard_update_default;
  }

  Status = EnumeratePartitions ();

  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n",
            Status));
    /* Leave the partition table alone; it was never populated. */
  } else {
    UpdatePartitionEntries ();
  }

  {
    UINT8  MenuRequested;

    /*
     * Scan for Volume Up held at power-on FIRST, before any other init disturbs
     * the console input. WaitForVolumeUpKey flushes stale input and then waits
     * for a genuine Volume Up press, skipping every other key (notably the
     * power key used to switch the device on) rather than being fooled by it.
     * Volume Up (the official recovery key slot) opens the boot menu; no Volume
     * Up within the window boots the stored entries in order.
     */
    MenuRequested = WaitForVolumeUpKey (1000);
    DEBUG ((EFI_D_INFO, "SFB: power-on volume-up detected=%u\n", MenuRequested));

    /*
     * Now bring up the embedded FAT/USB stack so every FAT and ext4 volume,
     * including one on a USB drive, is visible, then mount the efisp config
     * container that holds the stored boot entries. Neither is fatal: without
     * them the menu still offers fastboot.
     */
    Status = SfbStartFatStack ();
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Unable to start the FAT stack: %r\n", Status));
    }
    Status = SfbConfigStoreMount ();
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "Booting without a config store: %r\n", Status));
    }

    /*
     * Load (or build and store) the boot entries and, unless the menu was
     * requested, try them in order. Only returns when the menu should run:
     * nothing to boot, the menu key, or every entry failed.
     */
    SfbRunStartup ((BOOLEAN)(MenuRequested != 0));

    /*
     * Announce the menu and hold briefly so a still-held volume key is
     * released before the menu takes input, then run it. It only returns TRUE
     * when the user picked fastboot; FALSE means the menu is done. Fastboot
     * can be exited back to the menu (its "Exit to menu" row or "exit" from
     * the host), so the pair runs as a loop.
     */
    while (TRUE) {
      SfbShowEnteringMenu ();
      if (!SfbRunBootMenu ()) {
        Status = EFI_SUCCESS;
        goto stack_guard_update_default;
      }

      SfbShowFastbootMode ();
      DEBUG ((EFI_D_INFO, "Boot menu requested fastboot\n"));
      /* Fastboot may rewrite or erase efisp underneath the container: drop
       * the FAT view first and pick it up again afterwards. */
      SfbConfigStoreUnmount ();
      Status = FastbootInitialize ();
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
        goto stack_guard_update_default;
      }
      /* Fastboot exited back to the menu; loop around and show it again. */
      DEBUG ((EFI_D_INFO, "Fastboot exited to the boot menu\n"));
      (VOID)SfbConfigStoreMount ();
      (VOID)SfbLoadStoredEntries (SfbEntries ());
    }
  }

#ifdef AUTO_VIRT_ABL
  DEBUG ((EFI_D_INFO, "Rebooting the device.\n"));
  RebootDevice (NORMAL_MODE);
#endif
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  /*Update stack check guard with defualt value then return*/
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;

  DeInitThreadUnsafeStack ();

  return Status;
}
