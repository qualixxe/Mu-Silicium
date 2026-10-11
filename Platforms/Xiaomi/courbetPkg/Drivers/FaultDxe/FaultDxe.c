/** @file
  FaultDxe - compact on-screen synchronous exception reporter for courbet.

  Registers AArch64 synchronous exception handlers via the CPU arch
  protocol immediately after CpuDxe in the DXE APRIORI sequence.  On the
  first exception it prints a short block (exception type, PC, FAR, the
  faulting image name and a few registers) to the framebuffer serial and
  halts.  This turns an unreadable 50-line register dump (or a silent
  watchdog reset) into a few lines that fit on the phone screen.

  Copyright (c) 2026, qualixxe. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DevicePathLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/LoadedImage.h>
#include <Guid/DebugImageInfoTable.h>

//
// Local definitions of the DXE debug image info table structures.  Only the
// GUID is provided by the public headers, so the layout we need (matching the
// DXE core implementation) is declared here.
//
#define FAULT_DBG_IMG_NORMAL  1

typedef struct {
  EFI_HANDLE                  Handle;
  EFI_LOADED_IMAGE_PROTOCOL   *LoadedImageProtocolInstance;
} FAULT_DBG_IMG_NORMAL_ENTRY;

typedef struct {
  UINTN                        ImageFormat;
  union {
    FAULT_DBG_IMG_NORMAL_ENTRY  *NormalImage;
    VOID                        *CarveoutImage;
  } ImageInfo;
} FAULT_DBG_IMG_ENTRY;

typedef struct {
  UINTN                TableSize;
  FAULT_DBG_IMG_ENTRY  *EfiDebugImageInfoTable;
} FAULT_DBG_IMG_TABLE;

/* Write a NUL-terminated ASCII string to the framebuffer serial. */
STATIC
VOID
PrintStr (
  IN CONST CHAR8  *Str
  )
{
  SerialPortWrite ((UINT8 *)Str, AsciiStrLen (Str));
}

/* Write Value as Digits hexadecimal characters (no 0x prefix). */
STATIC
VOID
PrintHex (
  IN UINT64  Value,
  IN UINTN   Digits
  )
{
  CHAR8  Buf[16];
  UINTN  i;

  if (Digits > sizeof (Buf)) {
    Digits = sizeof (Buf);
  }

  for (i = 0; i < Digits; i++) {
    UINTN  Nib;

    Nib = (UINTN)((Value >> ((Digits - 1 - i) * 4)) & 0xF);
    Buf[i] = (CHAR8)((Nib < 10) ? ('0' + Nib) : ('a' + (Nib - 10)));
  }

  SerialPortWrite ((UINT8 *)Buf, Digits);
}

/* Print "<Label>0x<16 hex digits>\n". */
STATIC
VOID
PrintField (
  IN CONST CHAR8  *Label,
  IN UINT64        Value
  )
{
  PrintStr (Label);
  PrintStr ("0x");
  PrintHex (Value, 16);
  PrintStr ("\n");
}

/**
  Decode the Data Abort DFSC field (EC 0x24/0x25) into a short text.
**/
STATIC
CONST CHAR8 *
DescribeDfsc (
  IN UINT64  Esr
  )
{
  UINTN  Dfsc;

  Dfsc = (UINTN)(Esr & 0x3F);

  switch (Dfsc) {
    case 0x04: return "DATA ABORT: Translation L0";
    case 0x05: return "DATA ABORT: Translation L1";
    case 0x06: return "DATA ABORT: Translation L2";
    case 0x07: return "DATA ABORT: Translation L3";
    case 0x09: return "DATA ABORT: Access flag L1";
    case 0x0a: return "DATA ABORT: Access flag L2";
    case 0x0b: return "DATA ABORT: Access flag L3";
    case 0x0d: return "DATA ABORT: Permission L1";
    case 0x0e: return "DATA ABORT: Permission L2";
    case 0x0f: return "DATA ABORT: Permission L3";
    case 0x10: return "DATA ABORT: Sync external";
    case 0x11: return "DATA ABORT: Async external";
    case 0x14: return "DATA ABORT: Sync ext (walk L0)";
    case 0x15: return "DATA ABORT: Sync ext (walk L1)";
    case 0x16: return "DATA ABORT: Sync ext (walk L2)";
    case 0x17: return "DATA ABORT: Sync ext (walk L3)";
    case 0x21: return "DATA ABORT: Alignment";
    case 0x30: return "DATA ABORT: TLB conflict";
    default:   return "DATA ABORT";
  }
}

/**
  Decode ESR_ELx into a short human-readable exception description.
**/
STATIC
CONST CHAR8 *
DescribeEsr (
  IN UINT64  Esr
  )
{
  UINTN  Ec;

  Ec = (UINTN)(Esr >> 26);

  switch (Ec) {
    case 0x15: return "SVC64 (software interrupt)";
    case 0x18: return "SERROR";
    case 0x20:
    case 0x21: return "INSTRUCTION ABORT";
    case 0x22: return "PC ALIGNMENT FAULT";
    case 0x23: return "SP ALIGNMENT FAULT";
    case 0x24:
    case 0x25: return DescribeDfsc (Esr);
    default:   return "UNKNOWN SYNC EXCEPTION";
  }
}

/**
  Print the name of the loaded image whose memory range contains Address.
  Uses the DXE debug image info table published by the DXE core.
**/
STATIC
VOID
PrintImageName (
  IN UINT64  Address
  )
{
  FAULT_DBG_IMG_TABLE        *Table;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  CHAR16                     *Path;
  CHAR8                       AsciiPath[140];
  CHAR8                      *Sep;
  UINTN                       Idx;
  UINTN                       j;
  UINTN                       k;

  Table = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiDebugImageInfoTableGuid, NULL, (VOID **)&Table))) {
    PrintStr ("IMG: <no table>\n");
    return;
  }

  if ((Table == NULL) || (Table->EfiDebugImageInfoTable == NULL)) {
    PrintStr ("IMG: <null>\n");
    return;
  }

  for (Idx = 0; Idx < Table->TableSize; Idx++) {
    if (Table->EfiDebugImageInfoTable[Idx].ImageFormat != FAULT_DBG_IMG_NORMAL) {
      continue;
    }

    if (Table->EfiDebugImageInfoTable[Idx].ImageInfo.NormalImage == NULL) {
      continue;
    }

    LoadedImage = Table->EfiDebugImageInfoTable[Idx].ImageInfo.NormalImage->LoadedImageProtocolInstance;
    if ((LoadedImage == NULL) || (LoadedImage->ImageBase == 0)) {
      continue;
    }

    if ((Address >= (UINT64)LoadedImage->ImageBase) &&
        (Address <  (UINT64)LoadedImage->ImageBase + LoadedImage->ImageSize)) {
      Path = ConvertDevicePathToText (LoadedImage->FilePath, FALSE, FALSE);
      if (Path == NULL) {
        PrintStr ("IMG: <no path>\n");
        return;
      }

      k = 0;
      for (j = 0; (Path[j] != 0) && (k < sizeof (AsciiPath) - 1); j++) {
        if ((Path[j] == L'\\') || (Path[j] == L'/')) {
          AsciiPath[k++] = '/';
        } else if ((Path[j] > 0x20) && (Path[j] < 0x7F)) {
          AsciiPath[k++] = (CHAR8)Path[j];
        }
      }
      AsciiPath[k] = 0;

      Sep = AsciiPath;
      for (j = 0; AsciiPath[j] != 0; j++) {
        if (AsciiPath[j] == '/') {
          Sep = &AsciiPath[j + 1];
        }
      }

      PrintStr ("IMG: ");
      PrintStr (Sep);
      PrintStr ("\n");
      return;
    }
  }

  PrintStr ("IMG: <unknown>\n");
}

STATIC
VOID
EFIAPI
FaultHandler (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *Ctx;

  Ctx = SystemContext.SystemContextAArch64;

  PrintStr ("\n===== FAULT =====\n");
  PrintStr ("TYPE: ");
  PrintStr (DescribeEsr (Ctx->ESR));
  PrintStr ("\n");

  PrintField ("PC  =", Ctx->ELR);
  PrintField ("FAR =", Ctx->FAR);
  PrintImageName (Ctx->ELR);
  PrintField ("X0  =", Ctx->X0);
  PrintField ("LR  =", Ctx->LR);
  PrintField ("SP  =", Ctx->SP);
  PrintField ("ESR =", Ctx->ESR);

  /* Freeze the screen so the cause stays readable (watchdog may reset later). */
  for ( ; ; ) {
    CpuDeadLoop ();
  }
}

/**
  Entry point of the fault reporter.

  Hooks the AArch64 synchronous exception vectors so that any later fault
  prints the compact block above instead of the default dump.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
FaultDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_CPU_ARCH_PROTOCOL  *CpuArch;
  EFI_STATUS             Status;

  PrintStr ("FAULTDXE READY\n");

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&CpuArch);
  if (EFI_ERROR (Status) || (CpuArch == NULL)) {
    PrintStr ("FAULTDXE: no CpuArch\n");
    return EFI_SUCCESS;
  }

  CpuArch->RegisterInterruptHandler (CpuArch, EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, FaultHandler);
  CpuArch->RegisterInterruptHandler (CpuArch, EXCEPT_AARCH64_SERROR, FaultHandler);

  return EFI_SUCCESS;
}
