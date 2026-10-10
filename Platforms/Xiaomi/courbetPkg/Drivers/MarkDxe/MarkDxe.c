/** @file
  Early diagnostic marker driver for courbet crash localization.

  Three instances (MarkDxe01..MarkDxe03) print a short marker on the screen
  at very early APRIORI positions and return immediately (no stall).  Used to
  split the pre-staller window:

    MarkDxe01 - right after DxeMain + PcdDxe
    MarkDxe02 - right after EnvDxe/RtCode drivers, just before CpuDxe
    MarkDxe03 - at the very end of the APRIORI list (after console drivers)

  If the machine crashes inside CpuDxe/GicDxe/TimerDxe, MarkDxe01/MarkDxe02
  are visible but no "STALL" line appears.  If nothing is visible at all,
  the boot most likely died even earlier or never left fastboot handoff.

  Copyright (c) 2026, qualixxe. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SerialPortLib.h>

#ifndef MARK__IDX
#define MARK__IDX  0
#endif

/**
  Entry point of the diagnostic marker driver.

  @param  ImageHandle  The firmware allocated handle for the EFI image.
  @param  SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS  The entry point always succeeds.
**/
EFI_STATUS
EFIAPI
MarkDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  CHAR8  Marker[] = "MARK 00\n";

  Marker[5] = (CHAR8)('0' + (MARK__IDX / 10) % 10);
  Marker[6] = (CHAR8)('0' + (MARK__IDX % 10));
  (VOID) SerialPortWrite ((UINT8 *)Marker, sizeof (Marker) - 1);

  return EFI_SUCCESS;
}