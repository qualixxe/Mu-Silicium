/** @file
  FaultDxe - compact on-screen synchronous exception reporter for courbet.

  Registers AArch64 synchronous exception handlers via the CPU arch
  protocol immediately after CpuDxe in the DXE APRIORI sequence.  On the
  first exception it prints a short block (exception type, ESR fields, PC,
  FAR, X0/LR/SP and the faulting image name) to the framebuffer serial and
  halts.  This turns an unreadable 50-line register dump (or a silent
  watchdog reset) into a few lines that fit on the phone screen.

  Copyright (c) 2026, qualixxe. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PrintLib.h>
#include <Library/SerialPortLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/Cpu.h>
#include <Protocol/DebugSupport.h>
#include <Protocol/LoadedImage.h>
#include <Guid/DebugImageInfoTable.h>

STATIC
VOID
PrintStr (
  IN CONST CHAR8  *Str
  )
{
  SerialPortWrite ((UINT8 *)Str, AsciiStrLen (Str));
}

STATIC
VOID
PrintHexLine (
  IN CONST CHAR8  *Label,
  IN UINT64        Value
  )
{
  CHAR8  Buf[64];
  UINTN  N;

  N = AsciiSPrint (Buf, sizeof (Buf), "%a=0x%016lx\n", Label, Value);
  SerialPortWrite ((UINT8 *)Buf, N);
}

/**
  Decode ESR_ELx into a short human-readable exception description.

  @param[in]   Esr        ESR value captured by the exception vector entry.
  @param[out]  Detail     Buffer receiving "EC=.. ISS=.. DFSC=..".
  @param[in]   DetailSize Size of Detail.

  @return  Short type string (e.g. "DATA ABORT: Translation L3").
**/
STATIC
CONST CHAR8 *
DescribeEsr (
  IN  UINT64  Esr,
  OUT CHAR8   *Detail,
  IN  UINTN   DetailSize
  )
{
  UINTN  Ec;
  UINTN  Iss;
  UINTN  Dfsc;

  Ec   = (UINTN)(Esr >> 26);
  Iss  = (UINTN)(Esr & 0x1FFFFFF);
  Dfsc = Iss & 0x3F;

  AsciiSPrint (Detail, DetailSize, "EC=0x%02x ISS=0x%05x DFSC=0x%02x", Ec, Iss, Dfsc);

  switch (Ec) {
    case 0x15: return "SVC64";
    case 0x20:
    case 0x21: return "INSTRUCTION ABORT";
    case 0x22: return "PC ALIGNMENT FAULT";
    case 0x23: return "SP ALIGNMENT FAULT";
    case 0x18: return "SERROR";
    default:   return "UNKNOWN EC";
  }
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
  Print the name of the loaded image whose memory range contains Address.
  Uses the DXE Debug Image Info table published by the DXE core.
**/
STATIC
VOID
PrintImageName (
  IN UINT64  Address
  )
{
  EFI_DEBUG_IMAGE_INFO_TABLE  *Table;
  EFI_LOADED_IMAGE_PROTOCOL   *LoadedImage;
  CHAR16                      *Path;
  CHAR8                        AsciiPath[140];
  CHAR8                       *Sep;
  UINTN                        Idx;
  UINTN                        j;

  Table = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiDebugImageInfoTableGuid, NULL, (VOID **)&Table))) {
    PrintStr ("IMG=<notab>\n");
    return;
  }

  if (Table == NULL || Table->EfiDebugImageInfoTable == NULL) {
    PrintStr ("IMG=<null>\n");
    return;
  }

  for (Idx = 0; Idx < Table->TableSize; Idx++) {
    if (Table->EfiDebugImageInfoTable[Idx].ImageFormat != EFI_DEBUG_IMAGE_INFO_NORMAL) {
      continue;
    }

    LoadedImage = Table->EfiDebugImageInfoTable[Idx].ImageInfo.NormalImage->LoadedImageProtocolInstance;
    if (LoadedImage == NULL || LoadedImage->ImageBase == 0) {
      continue;
    }

    if (Address >= (UINT64)LoadedImage->ImageBase &&
        Address <  (UINT64)LoadedImage->ImageBase + LoadedImage->ImageSize) {
      Path = DevicePathToStr (LoadedImage->FilePath);
      if (Path == NULL) {
        PrintStr ("IMG=<nodp>\n");
        return;
      }

      j = 0;
      for (Idx = 0; Path[Idx] != 0 && j < sizeof (AsciiPath) - 1; Idx++) {
        if (Path[Idx] == L'\\' || Path[Idx] == L'/') {
          AsciiPath[j++] = '/';
        } else if (Path[Idx] > 0x20 && Path[Idx] < 0x7F) {
          AsciiPath[j++] = (CHAR8)Path[Idx];
        }
      }
      AsciiPath[j] = 0;

      Sep = AsciiPath;
      for (Idx = 0; AsciiPath[Idx] != 0; Idx++) {
        if (AsciiPath[Idx] == '/') {
          Sep = &AsciiPath[Idx + 1];
        }
      }

      PrintStr ("IMG: ");
      PrintStr (Sep);
      PrintStr ("\n");
      return;
    }
  }

  PrintStr ("IMG=<unknown>\n");
}

STATIC
VOID
EFIAPI
FaultHandler (
  IN EFI_EXCEPTION_TYPE   ExceptionType,
  IN EFI_SYSTEM_CONTEXT   SystemContext
  )
{
  EFI_SYSTEM_CONTEXT_AARCH64  *Ctx;
  CHAR8                        Detail[48];
  CONST CHAR8                  *Type;

  Ctx     = SystemContext.SystemContextAArch64;
  Detail[0] = '\0';

  PrintStr ("\n===== FAULT =====\n");

  if ((Ctx->ESR >> 26) == 0x24 || (Ctx->ESR >> 26) == 0x25) {
    Type = DescribeDfsc (Ctx->ESR);
  } else {
    Type = DescribeEsr (Ctx->ESR, Detail, sizeof (Detail));
  }

  PrintStr (Type);
  PrintStr ("\n");
  if (Detail[0] != '\0') {
    PrintStr (Detail);
    PrintStr ("\n");
  }
  PrintHexLine ("PC ", Ctx->ELR);
  PrintHexLine ("FAR", Ctx->FAR);
  PrintHexLine ("X0 ", Ctx->X0);
  PrintHexLine ("LR ", Ctx->LR);
  PrintHexLine ("SP ", Ctx->SP);
  PrintImageName (Ctx->ELR);

  // Freeze the screen so the cause stays readable (watchdog may reset later).
  for (;;) {
    CpuDeadLoop ();
  }
}

/**
  Entry point of the fault reporter.

  Hooks the AArch64 synchronous exception vectors so that any later
  fault prints the compact block above instead of the default dump.

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
  if (EFI_ERROR (Status) || CpuArch == NULL) {
    PrintStr ("FAULTDXE: no CpuArch\n");
    return EFI_SUCCESS;
  }

  CpuArch->RegisterInterruptHandler (EXCEPT_AARCH64_SYNCHRONOUS_EXCEPTIONS, FaultHandler);
  CpuArch->RegisterInterruptHandler (EXCEPT_AARCH64_SERROR, FaultHandler);

  return EFI_SUCCESS;
}