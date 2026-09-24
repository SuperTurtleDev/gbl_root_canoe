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

/* Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
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
 * Changes from Qualcomm Innovation Center are provided under the following license:
 *
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
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
 *   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/Debug.h>
#include <Library/LinuxLoaderLib.h>
#include <Library/PcdLib.h>
#include <Library/StackCanary.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/EFIUsbDevice.h>
#include <Protocol/SimpleTextIn.h>

#include "FastbootCmds.h"
#include "FastbootMain.h"
#include "UsbDescriptors.h"

#define USB_BUFF_SIZE USB_BUFFER_SIZE

/* Global fastboot data */
static FastbootDeviceData Fbd;
static USB_DEVICE_DESCRIPTOR_SET DescSet;

STATIC
CONST
struct {
  EFI_USB_BOS_DESCRIPTOR BosDescriptor;
  EFI_USB_USB_20_EXTENSION_DESCRIPTOR Usb2ExtDescriptor;
  EFI_USB_SUPERSPEED_USB_DESCRIPTOR SsUsbDescriptor;
  EFI_USB_SUPERSPEEDPLUS_USB_DESCRIPTOR SspUsbDescriptor;
} BinaryObjectStore = {
    // BOS Descriptor
    {
        sizeof (EFI_USB_BOS_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_BOS,               // Descriptor Type
        sizeof (BinaryObjectStore),      // Total Length
        3                                // Number of device capabilities
    },
    // USB2 Extension Desc
    {
        sizeof (EFI_USB_USB_20_EXTENSION_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY,   // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_USB_20_EXTENSION, // USB 2.0 Extension Capability Type
        0x6                                // Supported device level features
    },
    // Super Speed Device Capability Desc
    {
        sizeof (EFI_USB_SUPERSPEED_USB_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY, // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_SUPERSPEED_USB, // SuperSpeed Device Capability Type
        0x00,                            // Supported device level features
        0x0E, // Speeds Supported by the device: SS, HS and FS
        0x01, // Functionality support
        0x07, // U1 Device Exit Latency
        0x65  // U2 Device Exit Latency
    },
    // Super Speed Plus Device Capability Desc
    {
        sizeof (EFI_USB_SUPERSPEEDPLUS_USB_DESCRIPTOR), // Descriptor Size
        USB_DESC_TYPE_DEVICE_CAPABILITY, // Device Capability Type descriptor
        USB_DEV_CAP_TYPE_SUPERSPEEDPLUS_USB, //SuperSpeedPlus Device Capability
        0x00, // Reserved
        0x00000001, // Attributes
        0x1100, // Functionality Support
        0x00, // Reserved
        {0x000A4030, 0x000A40B0}, // Sublink Speed Attribute
    }
};

FastbootDeviceData *GetFastbootDeviceData (VOID)
{
  return &Fbd;
}

/* Dummy function needed for event notification callback */
STATIC VOID
DummyNotify (IN EFI_EVENT Event, IN VOID *Context)
{
}

STATIC EFI_STATUS FastbootUsbDeviceStart (VOID)
{
  EFI_STATUS Status;
  EFI_USB_BUS_SPEED UsbMaxSupportSpeed;
  UINTN UsbSpeedDataSize;
  USB_DEVICE_DESCRIPTOR *DevDesc;
  USB_DEVICE_DESCRIPTOR *SSDevDesc;
  VOID *Descriptors;
  VOID *SSDescriptors;
  EFI_EVENT UsbConfigEvt;
  EFI_GUID UsbDeviceProtolGuid = {
      0xd9d9ce48,
      0x44b8,
      0x4f49,
      {0x8e, 0x3e, 0x2a, 0x3b, 0x92, 0x7d, 0xc6, 0xc1}};
  EFI_GUID InitUsbControllerGuid = {
      0x1c0cffce,
      0xfc8d,
      0x4e44,
      {0x8c, 0x78, 0x9c, 0x9e, 0x5b, 0x53, 0xd, 0x36}};

  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DummyNotify,
                               NULL, &InitUsbControllerGuid, &UsbConfigEvt);
  if (EFI_ERROR (Status)) {
    DEBUG (
        (EFI_D_ERROR, "Usb controller init event not signaled: %r\n", Status));
    return Status;
  } else {
    gBS->SignalEvent (UsbConfigEvt);
    gBS->CloseEvent (UsbConfigEvt);
  }

  /* Locate the USBFastboot  Protocol from DXE */
  Status = gBS->LocateProtocol (&UsbDeviceProtolGuid, NULL,
                                (VOID **)&Fbd.UsbDeviceProtocol);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "couldnt find USB device protocol, exiting now"));
    return Status;
  }

  /* Register fastboot commands, allocate usb buffer*/
  Status = FastbootCmdsInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "couldnt init fastboot , exiting"));
    return Status;
  }

  /* Build the descriptor for fastboot */
  Status = BuildDefaultDescriptors (&DevDesc, &Descriptors, &SSDevDesc, &SSDescriptors);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Fastboot USB identity or descriptors unavailable: %r\n", Status));
    return Status;
  }
  UsbSpeedDataSize = sizeof (UsbMaxSupportSpeed);
  Status = gRT->GetVariable ((CHAR16 *)L"UsbfnMaxSpeed",
                             &gQcomTokenSpaceGuid,
                             NULL,
                             &UsbSpeedDataSize,
                             &UsbMaxSupportSpeed);

  if ((!EFI_ERROR (Status)) &&
      (UsbMaxSupportSpeed == UsbBusSpeedSuperPlus)) {
     SSDevDesc->BcdUSB = 0x0310;
  }

  DescSet.DeviceDescriptor = DevDesc;
  DescSet.Descriptors = Descriptors;
  DescSet.SSDeviceDescriptor = SSDevDesc;
  DescSet.SSDescriptors = SSDescriptors;
  DescSet.DeviceQualifierDescriptor = &DeviceQualifier;
  DescSet.BinaryDeviceOjectStore = (VOID *)&BinaryObjectStore;
  DescSet.StringDescriptorCount = 5;
  DescSet.StringDescritors = StrDescriptors;

  /* Start the usb device */
  Status = Fbd.UsbDeviceProtocol->StartEx (&DescSet);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "Error start the usb device, cannot enter fastboot mode\n"));
    return EFI_NOT_STARTED;
  }

  /* Allocate buffers required to receive the data from Host*/
  Status = Fbd.UsbDeviceProtocol->AllocateTransferBuffer (USB_BUFF_SIZE,
                                                          &Fbd.gRxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "Error Allocate RX buffer, cannot enter fastboot mode\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  /* Allocate buffers required to send data from device to Host*/
  Status = Fbd.UsbDeviceProtocol->AllocateTransferBuffer (USB_BUFF_SIZE,
                                                          &Fbd.gTxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR,
            "Error Allocate TX buffer, cannot enter fastboot mode\n"));
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((EFI_D_INFO, "Fastboot: Processing commands\n"));

  return Status;
}

/* API to stop USB device when booting to kernel, used for "fastboot boot" */
EFI_STATUS
FastbootUsbDeviceStop (VOID)
{
  EFI_STATUS Status;

  /* Free the Rx & Tx Buffers */
  Status = Fbd.UsbDeviceProtocol->FreeTransferBuffer (Fbd.gTxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Fastboot USB: Unable to free Tx Buffer\n"));
    return Status;
  }
  Status = Fbd.UsbDeviceProtocol->FreeTransferBuffer (Fbd.gRxBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Fastboot USB: Unable to free Rx Buffer\n"));
    return Status;
  }

  FreePool ((DescSet.Descriptors));
  FreePool ((DescSet.SSDescriptors));
  return Status;
}

/* Process bulk transfer out come for Rx */
STATIC EFI_STATUS
ProcessBulkXfrCompleteRx (IN USB_DEVICE_TRANSFER_OUTCOME *Uto)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // switch on the transfer status
  switch (Uto->Status) {
  case UsbDeviceTransferStatusCompleteOK:
    if (FastbootCurrentState () == ExpectDataState)
      DataReady (Uto->BytesCompleted, FastbootDloadBuffer ());
    else
      DataReady (Uto->BytesCompleted, Fbd.gRxBuffer);
    break;

  case UsbDeviceTransferStatusCancelled:
    // if usb connected, retry, otherwise wait to get connected, then retry
    DEBUG ((EFI_D_ERROR, "Bulk in XFR aborted\n"));
    Status = EFI_ABORTED;
    break;

  default: // Other statuses should not occur
    Status = EFI_DEVICE_ERROR;
    break;
  }
  return Status;
}

/* Process bulk transfer out come for Tx */
STATIC EFI_STATUS
ProcessBulkXfrCompleteTx (IN USB_DEVICE_TRANSFER_OUTCOME *Uto)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Switch on the transfer status
  switch (Uto->Status) {
  case UsbDeviceTransferStatusCompleteOK:
    DEBUG ((EFI_D_VERBOSE, "UsbDeviceTransferStatusCompleteOK\n"));
    /* Just Queue the next recieve, must be a Command */
    if (FastbootCurrentState () == ExpectDataState)
      Status = Fbd.UsbDeviceProtocol->Send (ENDPOINT_IN, GetXfrSize (),
                                            FastbootDloadBuffer ());
    else
      Status = Fbd.UsbDeviceProtocol->Send (ENDPOINT_IN, GetXfrSize (),
                                            Fbd.gRxBuffer);
    break;

  case UsbDeviceTransferStatusCancelled:
    DEBUG ((EFI_D_ERROR, "Bulk in xfr aborted"));
    Status = EFI_ABORTED;
    break;

  default: // Other statuses should not occur
    DEBUG ((EFI_D_ERROR, "unhandled trasnfer status"));
    Status = EFI_DEVICE_ERROR;
    break;
  }
  return Status;
}

/* Handle USB events, this will keep looking for events from USB protocol */
EFI_STATUS HandleUsbEvents (VOID)
{
  EFI_STATUS Status = EFI_SUCCESS;
  USB_DEVICE_EVENT Msg;
  USB_DEVICE_EVENT_DATA Payload;
  UINTN PayloadSize;

  /* Look for Event from Usb device protocol */
  Fbd.UsbDeviceProtocol->HandleEvent (&Msg, &PayloadSize, &Payload);
  if (UsbDeviceEventDeviceStateChange == Msg) {
    if (UsbDeviceStateConnected == Payload.DeviceState) {
      DEBUG ((EFI_D_VERBOSE, "Fastboot Device connected\n"));
      /* Queue receive buffer */
      Status = Fbd.UsbDeviceProtocol->Send (0x1, 511, Fbd.gRxBuffer);
    }
    if (UsbDeviceStateDisconnected == Payload.DeviceState) {
      DEBUG ((EFI_D_VERBOSE, "Fastboot Device disconnected\n"));
    }
  } else if (UsbDeviceEventTransferNotification == Msg) {
    /* Check if the transfer notification is on the Bulk EP and process it*/
    if (1 == USB_INDEX_TO_EP (Payload.TransferOutcome.EndpointIndex)) {
      /* If the direction is from host to device then process RX */
      if (USB_ENDPOINT_DIRECTION_OUT ==
          USB_INDEX_TO_EPDIR (Payload.TransferOutcome.EndpointIndex)) {

        Status = ProcessBulkXfrCompleteRx (&Payload.TransferOutcome);
        if (EFI_ERROR (Status)) {
          /* Should not happen, even if it happens we keep waiting for USB to be
           * connected */
          DEBUG ((EFI_D_ERROR,
                  "Error, should not happen! Check your USB connection"));
        }
      } else {
        /* Else the direction is from device to host,  process TX */
        Status = ProcessBulkXfrCompleteTx (&Payload.TransferOutcome);
        if (EFI_ERROR (Status)) {
          /* Should not happen, even if it happens we keep waiting for USB to be
           * connected */
          DEBUG ((EFI_D_ERROR,
                  "Error, should not happen! Check your USB connection"));
        }
      }
    }
  }
  return Status;
}

/* Initialize and start fastboot */
/*
 * On-device fastboot mode screen.
 *
 * While fastboot waits for a USB host it also offers two on-device actions -
 * power off and restart - driven by the same volume/power keys as the boot
 * menu. The USB event loop polls the console between transfers, so a host
 * connecting and issuing commands is unaffected.
 */
VOID RebootDevice (UINT8 RebootReason);
VOID ShutdownDevice (VOID);

/* Reboot reason values, mirroring ShutdownServices.h's RebootReasonType. The
 * header is deliberately not pulled in here just for these constants. */
#define NORMAL_MODE  0x0

#define FB_ACTION_ROWS  3

STATIC CONST CHAR16 *mFbActionRow[FB_ACTION_ROWS] = {
  L"Power Off",
  L"Restart",
  L"Exit to menu",
};
STATIC UINTN mFbActionCursor = 0;

/*
 * Set by the exit-to-menu command; the fastboot event loop observes it, tears
 * the gadget down through the ordinary exit path and returns to its caller,
 * which is the boot menu.
 */
STATIC BOOLEAN mFbExitToMenu = FALSE;

VOID
FastbootRequestExitToMenu (VOID)
{
  mFbExitToMenu = TRUE;
}

#define FB_ATTR_NORMAL    EFI_TEXT_ATTR (EFI_LIGHTGRAY, EFI_BLACK)
#define FB_ATTR_SELECTED  EFI_TEXT_ATTR (EFI_BLACK, EFI_LIGHTGRAY)
#define FB_ATTR_TITLE     EFI_TEXT_ATTR (EFI_WHITE, EFI_BLACK)

typedef enum {
  FbActionNone = 0,
  FbActionPowerOff,
  FbActionRestart,
  FbActionExitMenu
} FB_ACTION;

STATIC
VOID
FastbootDrawModeScreen (VOID)
{
  UINTN  Index;

  gST->ConOut->SetAttribute (gST->ConOut, FB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"FASTBOOT MODE\r\n\r\n");

  for (Index = 0; Index < FB_ACTION_ROWS; Index++) {
    gST->ConOut->SetAttribute (gST->ConOut,
                               (Index == mFbActionCursor) ? FB_ATTR_SELECTED
                                                           : FB_ATTR_NORMAL);
    Print (L"%s %s\r\n",
           (Index == mFbActionCursor) ? L">" : L" ",
           mFbActionRow[Index]);
  }

  gST->ConOut->SetAttribute (gST->ConOut, FB_ATTR_NORMAL);
  Print (L"\r\nVol Up/Down: move   Power: select\r\n");
}

STATIC
VOID
FastbootShowActionScreen (IN CONST CHAR16 *Text)
{
  gST->ConOut->SetAttribute (gST->ConOut, FB_ATTR_TITLE);
  gST->ConOut->ClearScreen (gST->ConOut);
  gST->ConOut->EnableCursor (gST->ConOut, FALSE);

  Print (L"%s\r\n", Text);

  gST->ConOut->SetAttribute (gST->ConOut, FB_ATTR_NORMAL);
}

/*
 * Non-blocking console poll. Returns the action to take, or FbActionNone when
 * no key is pending. Volume up/down move the highlight (and redraw); anything
 * else counts as confirm, matching how the boot menu treats the power key.
 */
STATIC
FB_ACTION
FastbootPollActionKey (VOID)
{
  EFI_STATUS     Status;
  EFI_INPUT_KEY  Key;

  Status = gBS->CheckEvent (gST->ConIn->WaitForKey);
  if (Status == EFI_NOT_READY || EFI_ERROR (Status)) {
    return FbActionNone;
  }

  Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  if (EFI_ERROR (Status)) {
    return FbActionNone;
  }

  if (Key.ScanCode == SCAN_UP) {
    mFbActionCursor = (mFbActionCursor == 0) ? FB_ACTION_ROWS - 1
                                              : mFbActionCursor - 1;
    FastbootDrawModeScreen ();
    return FbActionNone;
  }

  if (Key.ScanCode == SCAN_DOWN) {
    mFbActionCursor = (mFbActionCursor + 1 >= FB_ACTION_ROWS)
                        ? 0 : mFbActionCursor + 1;
    FastbootDrawModeScreen ();
    return FbActionNone;
  }

  switch (mFbActionCursor) {
  case 0:
    return FbActionPowerOff;
  case 1:
    return FbActionRestart;
  default:
    return FbActionExitMenu;
  }
}

EFI_STATUS FastbootInitialize (VOID)
{
  EFI_STATUS Status = EFI_SUCCESS;

  DEBUG ((EFI_D_INFO, "Fastboot Build Info: %a %a\n", __DATE__, __TIME__));

  /* Start the USB device enumeration */
  Status = FastbootUsbDeviceStart ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "couldnt Start fastboot usb device, exiting"));
    return Status;
  }
  StoreRootDeviceType ();

  /*
   * Draw the on-device fastboot mode screen with the power/restart actions. A
   * key held while entering fastboot (the power press that confirmed "Enter
   * Fastboot") is released and drained first, with a brief pause, so it cannot
   * fire a spurious confirm on the highlighted action row.
   */
  gBS->Stall (1000000);
  gST->ConIn->Reset (gST->ConIn, FALSE);
  mFbActionCursor = 0;
  FastbootDrawModeScreen ();

  /* Wait for USB events in tight loop */
  while (1) {
    Status = HandleUsbEvents ();
    if (EFI_ERROR (Status) && (Status != EFI_ABORTED)) {
      DEBUG ((EFI_D_ERROR, "Error, failed to handle USB event\n"));
      break;
    }

    /* The host asked for the menu back; take the ordinary exit path below. */
    if (mFbExitToMenu) {
      mFbExitToMenu = FALSE;
      FastbootShowActionScreen (L"Returning to Boot Menu...");
      break;
    }

    switch (FastbootPollActionKey ()) {
    case FbActionPowerOff:
      FastbootShowActionScreen (L"Powering off...");
      ShutdownDevice ();
      return EFI_SUCCESS;

    case FbActionRestart:
      FastbootShowActionScreen (L"Restarting...");
      RebootDevice (NORMAL_MODE);
      return EFI_SUCCESS;

    case FbActionExitMenu:
      FastbootShowActionScreen (L"Returning to Boot Menu...");
      /* The switch's own break only leaves this case; the flag is what takes
       * the while (1) below through its ordinary exit path. */
      mFbExitToMenu = TRUE;
      break;

    default:
      break;
    }

    if (FastbootFatal ()) {
      DEBUG ((EFI_D_ERROR, "Continue detected, Exiting App...\n"));
      break;
    }
  }

  /* Close the fastboot app and stop USB device */
  Status = FastbootCmdsUnInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "couldnt uninit fastboot\n"));
    return Status;
  }

  Status = FastbootUsbDeviceStop ();
  return Status;
}
