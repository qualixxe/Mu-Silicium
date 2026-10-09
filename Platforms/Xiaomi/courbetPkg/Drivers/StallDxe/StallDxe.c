/** @file
  Diagnostic stall driver for courbet crash localization.

  Ten identical instances of this driver (StallDxe01..StallDxe10, each with
  a unique FILE_GUID) are inserted into the DXE APRIORI list at spread out
  positions.  When a position is reached, the entry point writes a "STALL"
  marker to the screen and busy-stalls for 3 seconds via gBS->Stall().

  After `fastboot boot`, the platform watchdog-resets to fastboot shortly
  after the crash.  The elapsed time (minus the fixed boot overhead) tells
  us how many stallers ran, i.e. how far the boot got inside the APRIORI
  sequence and thus which driver region crashes.

  Copyright (c) 2026, qualixxe. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/SerialPortLib.h>

STATIC CONST CHAR8  mStallMarker[] = "STALL\n";

/**
  Entry point of the diagnostic stall driver.

  @param  ImageHandle  The firmware allocated handle for the EFI image.
  @param  SystemTable  A pointer to the EFI System Table.

  @retval EFI_SUCCESS  The entry point always succeeds.
**/
EFI_STATUS
EFIAPI
StallDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  (VOID) SerialPortWrite ((UINT8 *)mStallMarker, sizeof (mStallMarker) - 1);

  // 3 seconds.  gBS->Stall() is available: MetronomeDxe + TimerDxe are
  // loaded earlier in the APRIORI sequence than every staller instance.
  (VOID) gBS->Stall (3000000);

  return EFI_SUCCESS;
}