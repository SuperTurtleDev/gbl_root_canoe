/**
  @file Driver entry point

  Copyright (c) 2021 Pedro Falcato All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#include "Ext4Dxe.h"
#include <Library/Ext4ImageMap.h>

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mExt4DriverNameTable[] = {
  {
    "eng;en",
    L"Ext4 File System Driver"
  },
  {
    NULL,
    NULL
  }
};

GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE  mExt4ControllerNameTable[] = {
  {
    "eng;en",
    L"Ext4 File System"
  },
  {
    NULL,
    NULL
  }
};

// Needed by gExt4ComponentName*

EFI_STATUS
EFIAPI
Ext4ComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  );

EFI_STATUS
EFIAPI
Ext4ComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL                     *This,
  IN  EFI_HANDLE                                      ControllerHandle,
  IN  EFI_HANDLE                                      ChildHandle        OPTIONAL,
  IN  CHAR8                                           *Language,
  OUT CHAR16                                          **ControllerName
  );

extern EFI_COMPONENT_NAME_PROTOCOL  gExt4ComponentName;

GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME_PROTOCOL  gExt4ComponentName = {
  Ext4ComponentNameGetDriverName,
  Ext4ComponentNameGetControllerName,
  "eng"
};

//
// EFI Component Name 2 Protocol
//
GLOBAL_REMOVE_IF_UNREFERENCED EFI_COMPONENT_NAME2_PROTOCOL  gExt4ComponentName2 = {
  (EFI_COMPONENT_NAME2_GET_DRIVER_NAME)Ext4ComponentNameGetDriverName,
  (EFI_COMPONENT_NAME2_GET_CONTROLLER_NAME)Ext4ComponentNameGetControllerName,
  "en"
};

// Needed by gExt4BindingProtocol

EFI_STATUS EFIAPI
Ext4IsBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  );

EFI_STATUS EFIAPI
Ext4Bind (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  );

EFI_STATUS EFIAPI
Ext4Stop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE ControllerHandle,
  IN UINTN NumberOfChildren,
  IN EFI_HANDLE *ChildHandleBuffer OPTIONAL
  );

EFI_DRIVER_BINDING_PROTOCOL  gExt4BindingProtocol =
{
  Ext4IsBindingSupported,
  Ext4Bind,
  Ext4Stop,
  EXT4_DRIVER_VERSION
};

/* The mapper consumes private EXT4_FILE state, so merely finding an SFS is
 * insufficient after RAM-loading a new BDS image. Select this driver's own
 * instance without disconnecting the lower DiskIo/BlockIo stack. */
EFI_STATUS
Ext4OpenImageFileSystem (EFI_HANDLE Controller, EFI_SIMPLE_FILE_SYSTEM_PROTOCOL **Fs)
{
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Current = NULL;
  EFI_OPEN_PROTOCOL_INFORMATION_ENTRY *Info = NULL;
  EFI_HANDLE Owner = NULL;
  EFI_HANDLE Drivers[2];
  EFI_STATUS Status, ConnectStatus;
  UINTN Count = 0, Index;

  if (Fs == NULL || Controller == NULL) return EFI_INVALID_PARAMETER;
  *Fs = NULL;
  Status = gBS->HandleProtocol (Controller, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Current);
  if (!EFI_ERROR (Status) && Current != NULL && Current->OpenVolume == Ext4OpenVolume) {
    *Fs = Current;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND && Status != EFI_UNSUPPORTED) return Status;
  if (!EFI_ERROR (Status) && Current == NULL) return EFI_DEVICE_ERROR;
  if (gExt4BindingProtocol.DriverBindingHandle == NULL) return EFI_NOT_READY;

  if (Current != NULL) {
    Status = gBS->OpenProtocolInformation (Controller, &gEfiDiskIoProtocolGuid, &Info, &Count);
    if (EFI_ERROR (Status)) return Status;
    for (Index = 0; Index < Count; ++Index) {
      if (!(Info[Index].Attributes & EFI_OPEN_PROTOCOL_BY_DRIVER) ||
          Info[Index].ControllerHandle != Controller) continue;
      if (Owner != NULL && Owner != Info[Index].AgentHandle) {
        FreePool (Info);
        return EFI_ACCESS_DENIED;
      }
      Owner = Info[Index].AgentHandle;
    }
    if (Info != NULL) FreePool (Info);
    if (Owner == NULL || Owner == gExt4BindingProtocol.DriverBindingHandle)
      return EFI_ACCESS_DENIED;
    Status = gBS->DisconnectController (Controller, Owner, NULL);
    if (EFI_ERROR (Status)) return Status;
    Current = NULL;
    Status = gBS->HandleProtocol (Controller, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Current);
    if (!EFI_ERROR (Status)) return EFI_ACCESS_DENIED;
    if (Status != EFI_NOT_FOUND && Status != EFI_UNSUPPORTED) return Status;
  }

  Drivers[0] = gExt4BindingProtocol.DriverBindingHandle;
  Drivers[1] = NULL;
  ConnectStatus = gBS->ConnectController (Controller, Drivers, NULL, FALSE);
  Current = NULL;
  Status = gBS->HandleProtocol (Controller, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Current);
  if (!EFI_ERROR (Status) && Current != NULL && Current->OpenVolume == Ext4OpenVolume) {
    *Fs = Current;
    return EFI_SUCCESS;
  }
  if (EFI_ERROR (ConnectStatus)) return ConnectStatus;
  return EFI_ERROR (Status) ? Status : EFI_UNSUPPORTED;
}

EFI_STATUS
Ext4ReleaseImageFileSystem (EFI_HANDLE Controller)
{
  EFI_STATUS Status;
  if (Controller == NULL) return EFI_INVALID_PARAMETER;
  if (gExt4BindingProtocol.DriverBindingHandle == NULL) return EFI_SUCCESS;
  Status = gBS->DisconnectController (
                  Controller, gExt4BindingProtocol.DriverBindingHandle, NULL);
  /* NOT_FOUND means this driver is not bound; do not compensate by tearing
   * down another provider or the lower disk stack. */
  return Status == EFI_NOT_FOUND ? EFI_SUCCESS : Status;
}

EFI_STATUS
EFIAPI
Ext4ComponentNameGetControllerName (
  IN  EFI_COMPONENT_NAME_PROTOCOL                     *This,
  IN  EFI_HANDLE                                      ControllerHandle,
  IN  EFI_HANDLE                                      ChildHandle        OPTIONAL,
  IN  CHAR8                                           *Language,
  OUT CHAR16                                          **ControllerName
  )
{
  // TODO: Do we need to test whether we're managing the handle, like FAT does?
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mExt4ControllerNameTable,
           ControllerName,
           (BOOLEAN)(This == &gExt4ComponentName)
           );
}

EFI_STATUS
EFIAPI
Ext4ComponentNameGetDriverName (
  IN  EFI_COMPONENT_NAME_PROTOCOL  *This,
  IN  CHAR8                        *Language,
  OUT CHAR16                       **DriverName
  )
{
  return LookupUnicodeString2 (
           Language,
           This->SupportedLanguages,
           mExt4DriverNameTable,
           DriverName,
           (BOOLEAN)(This == &gExt4ComponentName)
           );
}

EFI_STATUS EFIAPI
Ext4IsBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  );

EFI_STATUS EFIAPI
Ext4Bind (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  );

EFI_STATUS EFIAPI
Ext4Stop (
  IN EFI_DRIVER_BINDING_PROTOCOL *This,
  IN EFI_HANDLE ControllerHandle,
  IN UINTN NumberOfChildren,
  IN EFI_HANDLE *ChildHandleBuffer OPTIONAL
  )
{
  EFI_STATUS                       Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Sfs;
  EXT4_PARTITION                   *Partition;
  BOOLEAN                          HasDiskIo2;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&Sfs,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Partition = (EXT4_PARTITION *)Sfs;

  HasDiskIo2 = Ext4DiskIo2 (Partition) != NULL;

  /* A refused disconnect must leave the published interface usable. Do not
   * free its private state until the protocol has actually been withdrawn. */
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  ControllerHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  &Partition->Interface,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Ext4UnmountAndFreePartition (Partition);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  // Close all open protocols (DiskIo, DiskIo2, BlockIo)

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gEfiDiskIoProtocolGuid,
                  This->DriverBindingHandle,
                  ControllerHandle
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->CloseProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  This->DriverBindingHandle,
                  ControllerHandle
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  if(HasDiskIo2) {
    Status = gBS->CloseProtocol (
                    ControllerHandle,
                    &gEfiDiskIo2ProtocolGuid,
                    This->DriverBindingHandle,
                    ControllerHandle
                    );

    if (EFI_ERROR (Status)) {
      return Status;
    }
  }

  return Status;
}

EFI_STATUS
EFIAPI
Ext4EntryPoint (
  IN EFI_HANDLE         ImageHandle,
  IN EFI_SYSTEM_TABLE   *SystemTable
  )
{
  EFI_STATUS  Status;

  Status = EfiLibInstallAllDriverProtocols2 (
             ImageHandle,
             SystemTable,
             &gExt4BindingProtocol,
             ImageHandle,
             &gExt4ComponentName,
             &gExt4ComponentName2,
             NULL,
             NULL,
             NULL,
             NULL
             );

  if(EFI_ERROR (Status)) {
    return Status;
  }

  return Ext4InitialiseUnicodeCollation (ImageHandle);
}

EFI_STATUS
EFIAPI
Ext4Unload (
  IN EFI_HANDLE ImageHandle
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *DeviceHandleBuffer;
  UINTN       DeviceHandleCount;
  UINTN       Index;

  Status = gBS->LocateHandleBuffer (
                  AllHandles,
                  NULL,
                  NULL,
                  &DeviceHandleCount,
                  &DeviceHandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for(Index = 0; Index < DeviceHandleCount; Index++) {
    EFI_HANDLE  Handle;

    Handle = DeviceHandleBuffer[Index];

    Status = EfiTestManagedDevice (Handle, ImageHandle, &gEfiDiskIoProtocolGuid);

    if(Status == EFI_SUCCESS) {
      Status = gBS->DisconnectController (Handle, ImageHandle, NULL);

      if (EFI_ERROR (Status)) {
        break;
      }
    }
  }

  FreePool (DeviceHandleBuffer);

  Status = EfiLibUninstallAllDriverProtocols2 (
             &gExt4BindingProtocol,
             &gExt4ComponentName,
             &gExt4ComponentName2,
             NULL,
             NULL,
             NULL,
             NULL
             );

  return Status;
}

EFI_STATUS EFIAPI
Ext4IsBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  )
{
  // Note to self: EFI_OPEN_PROTOCOL_TEST_PROTOCOL lets us not close the
  // protocol and ignore the output argument entirely

  EFI_STATUS  Status;

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDiskIoProtocolGuid,
                  NULL,
                  BindingProtocol->ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  );

  if(EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  BindingProtocol->ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                  );
  return Status;
}

EFI_STATUS EFIAPI
Ext4Bind (
  IN EFI_DRIVER_BINDING_PROTOCOL *BindingProtocol,
  IN EFI_HANDLE ControllerHandle,
  IN EFI_DEVICE_PATH *RemainingDevicePath OPTIONAL
  )
{
  EFI_DISK_IO_PROTOCOL   *DiskIo;
  EFI_DISK_IO2_PROTOCOL  *DiskIo2;
  EFI_BLOCK_IO_PROTOCOL  *blockIo;
  EFI_STATUS             Status;

  DiskIo2 = NULL;

  DEBUG ((EFI_D_VERBOSE, "[Ext4] Binding to controller\n"));

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDiskIoProtocolGuid,
                  (VOID **)&DiskIo,
                  BindingProtocol->ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if(EFI_ERROR (Status)) {
    return Status;
  }

  DEBUG ((EFI_D_VERBOSE, "[Ext4] Controller supports DISK_IO\n"));

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDiskIo2ProtocolGuid,
                  (VOID **)&DiskIo2,
                  BindingProtocol->ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  // It's okay to not support DISK_IO2

  if(DiskIo2 != NULL) {
    DEBUG ((EFI_D_VERBOSE, "[Ext4] Controller supports DISK_IO2\n"));
  }

  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiBlockIoProtocolGuid,
                  (VOID **)&blockIo,
                  BindingProtocol->ImageHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if(EFI_ERROR (Status)) {
    goto Error;
  }

  DEBUG ((EFI_D_VERBOSE, "Opening partition\n"));

  Status = Ext4OpenPartition (ControllerHandle, DiskIo, DiskIo2, blockIo);

  if(!EFI_ERROR (Status)) {
    return Status;
  }

  /* ConnectController probes this driver against every disk handle. A mount
   * rejection is therefore normal discovery noise, not a runtime failure. */
  DEBUG ((EFI_D_VERBOSE, "[ext4] Mount probe rejected controller: %r\n",
          Status));

Error:
  if(DiskIo) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiDiskIoProtocolGuid,
           BindingProtocol->ImageHandle,
           ControllerHandle
           );
  }

  if(DiskIo2) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiDiskIo2ProtocolGuid,
           BindingProtocol->ImageHandle,
           ControllerHandle
           );
  }

  if(blockIo) {
    gBS->CloseProtocol (
           ControllerHandle,
           &gEfiBlockIoProtocolGuid,
           BindingProtocol->ImageHandle,
           ControllerHandle
           );
  }

  return Status;
}
