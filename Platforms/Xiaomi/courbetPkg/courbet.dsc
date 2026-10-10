##
#  Copyright (c) 2011 - 2022, ARM Limited. All rights reserved.
#  Copyright (c) 2014, Linaro Limited. All rights reserved.
#  Copyright (c) 2015 - 2020, Intel Corporation. All rights reserved.
#  Copyright (c) 2018, Bingxing Wang. All rights reserved.
#  Copyright (c) Microsoft Corporation.
#
#  SPDX-License-Identifier: BSD-2-Clause-Patent
##

################################################################################
#
# Defines Section - statements that will be processed to create a Makefile.
#
################################################################################
[Defines]
  PLATFORM_NAME                  = courbet
  PLATFORM_GUID                  = 44C9F7A6-ECED-468B-B2A0-F6C200DF7B01
  PLATFORM_VERSION               = 0.1
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/courbetPkg
  SUPPORTED_ARCHITECTURES        = AARCH64
  BUILD_TARGETS                  = RELEASE|DEBUG
  SKUID_IDENTIFIER               = DEFAULT
  FLASH_DEFINITION               = courbetPkg/courbet.fdf
  USE_CUSTOM_DISPLAY_DRIVER      = 0

  #
  # 0 = SM7150
  # 1 = SM7150-AB
  # 2 = SM7150-AC
  #
  SOC_TYPE                       = 2

!include MooreaPkg/MooreaPkg.dsc.inc

[PcdsFixedAtBuild]
  #
  # DDR Memory
  #
  gArmTokenSpaceGuid.PcdSystemMemoryBase|0x80000000

  #
  # UEFI Stack
  #
  gArmPlatformTokenSpaceGuid.PcdCPUCoresStackBase|0x9FF90000
  gArmPlatformTokenSpaceGuid.PcdCPUCorePrimaryStackSize|0x40000

  #
  # SMBIOS
  #
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemManufacturer|"Xiaomi"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemModel|"Mi 11 Lite 4G"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailModel|"courbet"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemRetailSku|"Mi_11_Lite_4G_courbet"
  gSiliciumPkgTokenSpaceGuid.PcdSmbiosSystemBoardModel|"Mi 11 Lite 4G"

  #
  # Simple Frame Buffer
  #
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferWidth|1080
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferHeight|2400
  gSiliciumPkgTokenSpaceGuid.PcdFrameBufferColorDepth|32

  #
  # Platform PEI
  #
  gQcomPkgTokenSpaceGuid.PcdPlatformType|"WP"

[LibraryClasses]
  #
  # Memory Libraries
  #
  MemoryMapLib|courbetPkg/Library/MemoryMapLib/MemoryMapLib.inf

  #
  # QCOM Libraries
  #
  ConfigurationMapLib|courbetPkg/Library/ConfigurationMapLib/ConfigurationMapLib.inf

[Components]
  #
  # ACPI Tables
  #
  #courbet/AcpiTables.inf

  #
  # Diagnostic stall drivers (crash localization)
  #
  courbetPkg/Drivers/StallDxe/StallDxe01.inf
  courbetPkg/Drivers/StallDxe/StallDxe02.inf
  courbetPkg/Drivers/StallDxe/StallDxe03.inf
  courbetPkg/Drivers/StallDxe/StallDxe04.inf
  courbetPkg/Drivers/StallDxe/StallDxe05.inf
  courbetPkg/Drivers/StallDxe/StallDxe06.inf
  courbetPkg/Drivers/StallDxe/StallDxe07.inf
  courbetPkg/Drivers/StallDxe/StallDxe08.inf
  courbetPkg/Drivers/StallDxe/StallDxe09.inf
  courbetPkg/Drivers/StallDxe/StallDxe10.inf
  courbetPkg/Drivers/MarkDxe/MarkDxe01.inf
  courbetPkg/Drivers/MarkDxe/MarkDxe02.inf
  courbetPkg/Drivers/MarkDxe/MarkDxe03.inf
