/*
 * efi.h - just enough of the UEFI 2.x interfaces for the bdos loader.
 *
 * Written from the UEFI specification; no third-party headers. Built with
 * clang -target x86_64-unknown-windows, so function pointers already use
 * the Microsoft x64 calling convention that UEFI requires.
 */
#ifndef BDOS_EFI_H
#define BDOS_EFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint64_t	UINTN;
typedef uint64_t	EFI_STATUS;
typedef void		*EFI_HANDLE;
typedef void		*EFI_EVENT;
typedef uint16_t	CHAR16;
typedef uint8_t		BOOLEAN;
typedef uint64_t	EFI_PHYSICAL_ADDRESS;

#define EFIAPI	/* MS ABI is the default for this target */

#define EFI_SUCCESS		0
#define EFI_ERR(x)		(0x8000000000000000ULL | (x))
#define EFI_BUFFER_TOO_SMALL	EFI_ERR(5)
#define EFI_INVALID_PARAMETER	EFI_ERR(2)
#define EFI_ERROR(s)		(((int64_t)(s)) < 0)

typedef struct {
	uint32_t Data1;
	uint16_t Data2;
	uint16_t Data3;
	uint8_t  Data4[8];
} EFI_GUID;

typedef struct {
	uint64_t Signature;
	uint32_t Revision;
	uint32_t HeaderSize;
	uint32_t CRC32;
	uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ---- Simple text output ------------------------------------------------ */

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
	EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
	void *TestString;
	void *QueryMode;
	void *SetMode;
	EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
	EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
	void *SetCursorPosition;
	void *EnableCursor;
	void *Mode;
};

/* ---- Memory ------------------------------------------------------------- */

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
} EFI_ALLOCATE_TYPE;

typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
} EFI_MEMORY_TYPE;

typedef struct {
	uint32_t Type;
	uint32_t _pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	uint64_t VirtualStart;
	uint64_t NumberOfPages;
	uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---- Boot services ------------------------------------------------------ */

typedef enum {
	AllHandles,
	ByRegisterNotify,
	ByProtocol,
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
	EFI_TABLE_HEADER Hdr;

	void *RaiseTPL;
	void *RestoreTPL;

	EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE, EFI_MEMORY_TYPE,
	    UINTN, EFI_PHYSICAL_ADDRESS *);
	EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS, UINTN);
	EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *, EFI_MEMORY_DESCRIPTOR *,
	    UINTN *, UINTN *, uint32_t *);
	EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
	EFI_STATUS (EFIAPI *FreePool)(void *);

	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;

	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;

	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE, UINTN);

	void *GetNextMonotonicCount;
	EFI_STATUS (EFIAPI *Stall)(UINTN);
	EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN, uint64_t, UINTN, CHAR16 *);

	void *ConnectController;
	void *DisconnectController;

	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;

	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *, void *, void **);
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;

	void *CalculateCrc32;
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* ---- System table ------------------------------------------------------- */

typedef struct {
	EFI_GUID VendorGuid;
	void *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	uint32_t FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- Loaded image ------------------------------------------------------- */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{ 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
	uint32_t Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	void *FilePath;
	void *Reserved;
	uint32_t LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	uint64_t ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- File system -------------------------------------------------------- */

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{ 0x964E5B22, 0x6459, 0x11d2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

#define EFI_FILE_MODE_READ	0x0000000000000001ULL

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
	uint64_t Revision;
	EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *, EFI_FILE_PROTOCOL **,
	    CHAR16 *, uint64_t, uint64_t);
	EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *);
	void *Delete;
	EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *, UINTN *, void *);
	void *Write;
	EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *, uint64_t *);
	EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *, uint64_t);
	void *GetInfo;
	void *SetInfo;
	void *Flush;
};

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	uint64_t Revision;
	EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *,
	    EFI_FILE_PROTOCOL **);
};

/* ---- Graphics output ---------------------------------------------------- */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{ 0x9042A9DE, 0x23DC, 0x4A38, { 0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A } }

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	uint32_t Version;
	uint32_t HorizontalResolution;
	uint32_t VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	uint32_t MaxMode;
	uint32_t Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
	EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t,
	    UINTN *, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
	EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t);
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- ACPI --------------------------------------------------------------- */

#define EFI_ACPI_20_TABLE_GUID \
	{ 0x8868E871, 0xE4F1, 0x11D3, { 0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81 } }
#define EFI_ACPI_10_TABLE_GUID \
	{ 0xEB9D2D30, 0x2D88, 0x11D3, { 0x9A, 0x16, 0x00, 0x90, 0x27, 0x3F, 0xC1, 0x4D } }

#endif
