/** @file
  FaultDxe - compact on-screen synchronous exception reporter for courbet.

  Registers AArch64 synchronous exception handlers via the CPU arch
  protocol immediately after CpuDxe in the DXE APRIORI sequence.  On the
  first exception it prints a short block (exception type, PC, FAR, ESR and
  the faulting image + PDB name) to the framebuffer serial and then keeps
  the machine alive (interrupts re-enabled, UEFI watchdog disabled) so the
  screen stays readable.  This turns an unreadable 50-line register dump
  (or a silent watchdog reset) into a few lines that fit on the screen.

  Copyright (c) 2026, qualixxe. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
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

STATIC volatile UINTN  mInFault = 0;

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

/* Clear the EL1 IRQ mask so timer/watchdog events keep running. */
STATIC
VOID
EnableInterrupts (
  VOID
  )
{
  __asm__ __volatile__ ("msr daifclr, #2" ::: "memory");
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

/* Bounds-checked little-endian 32-bit read from the loaded image. */
STATIC
BOOLEAN
InImage (
  IN UINT64  Base,
  IN UINT64  Size,
  IN UINT64  Addr,
  IN UINTN   Len
  )
{
  if (Addr < Base) {
    return FALSE;
  }

  if (Addr + Len < Addr) {
    return FALSE;
  }

  return ((Addr + Len) <= (Base + Size));
}

/**
  Best-effort extraction of the module name from the PE/COFF CodeView debug
  directory of the image containing "Base".  Everything is bounds-checked
  against [Base, Base+Size).  Prints nothing if no name is found.
**/
STATIC
VOID
PrintPdbName (
  IN UINT64  Base,
  IN UINT64  Size
  )
{
  UINT32  PeOff;
  UINT32  DbgRva;
  UINT32  DbgSize;
  UINT32  NumDirs;
  UINT32  OptOff;
  UINT32  Ent;
  UINT32  EntOff;
  UINT32  EntType;
  UINT32  EntRaw;
  UINT8   *P;

  /* DOS header: e_magic 'MZ' at 0, e_lfanew at 0x3C. */
  if (!InImage (Base, Size, Base + 0x3C, 4)) {
    return;
  }

  if (*(UINT16 *)(UINTN)Base != 0x5A4D) {
    return;
  }

  PeOff = *(UINT32 *)(UINTN)(Base + 0x3C);
  if (PeOff == 0) {
    return;
  }

  if (!InImage (Base, Size, Base + PeOff, 24)) {
    return;
  }

  P = (UINT8 *)(UINTN)(Base + PeOff);
  if ((P[0] != 'P') || (P[1] != 'E') || (P[2] != 0) || (P[3] != 0)) {
    return;
  }

  /* COFF header is 20 bytes; optional header follows. */
  OptOff = PeOff + 24;
  if (!InImage (Base, Size, Base + OptOff, 4)) {
    return;
  }

  /* Magic 0x20b = PE32+; NumDirs at optional header + 108. */
  if (!InImage (Base, Size, Base + OptOff + 108, 4)) {
    return;
  }

  NumDirs = *(UINT16 *)(UINTN)(Base + OptOff + 108);
  if (NumDirs < 7) {
    return;
  }

  /* DataDirectory[6] = IMAGE_DIRECTORY_ENTRY_DEBUG; PE32+ base is 0x70. */
  if (!InImage (Base, Size, Base + OptOff + 0x70 + 6 * 8, 8)) {
    return;
  }

  DbgRva  = *(UINT32 *)(UINTN)(Base + OptOff + 0x70 + 6 * 8);
  DbgSize = *(UINT32 *)(UINTN)(Base + OptOff + 0x70 + 6 * 8 + 4);
  if ((DbgRva == 0) || (DbgSize < 28)) {
    return;
  }

  for (Ent = 0; Ent < (DbgSize / 28) && Ent < 16; Ent++) {
    EntOff = DbgRva + Ent * 28;
    if (!InImage (Base, Size, Base + EntOff, 28)) {
      return;
    }

    EntType = *(UINT32 *)(UINTN)(Base + EntOff + 12);
    EntRaw  = *(UINT32 *)(UINTN)(Base + EntOff + 24);

    /* IMAGE_DEBUG_TYPE_CODEVIEW = 2 */
    if ((EntType != 2) || (EntRaw == 0)) {
      continue;
    }

    /* RSDS followed by GUID(16) + Age(4) + NUL-terminated path. */
    if (!InImage (Base, Size, Base + EntRaw, 24)) {
      continue;
    }

    if ((*(UINT32 *)(UINTN)(Base + EntRaw) != 0x53445352)) { /* 'RSDS' */
      continue;
    }

    {
      CHAR8  *Name;
      CHAR8  *Sep;
      CHAR8  *Cur;
      CHAR8   Out[44];
      UINTN   N;
      UINTN   k;

      Name = (CHAR8 *)(UINTN)(Base + EntRaw + 24);
      /* Path string is bounded by the image; find its length. */
      N = 0;
      while ((N < 200) && InImage (Base, Size, (UINT64)(UINTN)(Name + N), 1) && (Name[N] != 0)) {
        N++;
      }

      if (N == 0) {
        return;
      }

      /* Strip directory and keep the base name. */
      Sep = Name;
      for (Cur = Name; Cur < (Name + N); Cur++) {
        if ((*Cur == '\\') || (*Cur == '/') || (*Cur == ':')) {
          Sep = Cur + 1;
        }
      }

      k = 0;
      while (((Sep + k) < (Name + N)) && (k < sizeof (Out) - 1) &&
             (Sep[k] >= 0x20) && (Sep[k] < 0x7F))
      {
        Out[k] = Sep[k];
        k++;
      }
      Out[k] = 0;

      if (k != 0) {
        PrintStr ("IMG: ");
        PrintStr (Out);
        PrintStr ("\n");
      }
    }
    return;
  }
}

/**
  Print the loaded image containing Address (base + offset) and, if possible,
  its module name via the PE debug directory.
**/
STATIC
VOID
PrintImage (
  IN UINT64  Address
  )
{
  FAULT_DBG_IMG_TABLE        *Table;
  EFI_LOADED_IMAGE_PROTOCOL  *LoadedImage;
  UINTN                       Idx;

  Table = NULL;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiDebugImageInfoTableGuid, NULL, (VOID **)&Table))) {
    return;
  }

  if ((Table == NULL) || (Table->EfiDebugImageInfoTable == NULL)) {
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
      PrintStr ("IMG: base=0x");
      PrintHex ((UINT64)LoadedImage->ImageBase, 16);
      PrintStr (" +0x");
      PrintHex (Address - (UINT64)LoadedImage->ImageBase, 8);
      PrintStr ("\n");
      PrintPdbName ((UINT64)LoadedImage->ImageBase, LoadedImage->ImageSize);
      return;
    }
  }
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

  if (mInFault != 0) {
    /* Recursive fault while reporting: stay quiet and hang. */
    for ( ; ; ) {
      CpuDeadLoop ();
    }
  }

  mInFault = 1;

  Ctx = SystemContext.SystemContextAArch64;

  PrintStr ("\n===== FAULT =====\n");
  PrintStr ("TYPE: ");
  PrintStr (DescribeEsr (Ctx->ESR));
  PrintStr ("\n");

  PrintField ("PC  =", Ctx->ELR);
  PrintField ("FAR =", Ctx->FAR);
  PrintField ("ESR =", Ctx->ESR);
  PrintField ("X0  =", Ctx->X0);
  PrintField ("LR  =", Ctx->LR);
  PrintField ("SP  =", Ctx->SP);
  PrintImage (Ctx->ELR);

  /* Try to stop the platform from resetting so the screen persists. */
  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  for ( ; ; ) {
    EnableInterrupts ();
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
