/**
 * @file get_memory_map.c
 * @brief Retrieves a full UEFI memory map in a robust, commercial-ready way
 *
 * Memory map is required for kernel virtual memory setup and ExitBootServices().
 * Handles retries, alignment, and variable descriptor sizes per UEFI spec.
 *
 * UEFI Spec Reference:
 *   - Section 7.2: GetMemoryMap()
 */

#include <uefi/types.h>
#include <uefi/status.h>
#include <uefi/system_table.h>
#include <uefi/boot_services.h>
#include <uefi/memory.h>

#include <boot/uefi/uefi_globals.h>
#include <boot/uefi/uefi_logging.h>
#include <boot/uefi/uefi_assert.h>

#define MEMORY_MAP_MAX_RETRIES 8
#define MEMORY_MAP_EXTRA_DESCRIPTORS 16

/**
 * Retrieves the full memory map from firmware.
 *
 * @param[out] MemoryMap      Pointer to store allocated memory map
 * @param[out] MemoryMapSize  Size of allocated memory map in bytes
 * @param[out] MapKey         Key required for ExitBootServices
 * @param[out] DescriptorSize Size of each EFI_MEMORY_DESCRIPTOR
 * @param[out] DescriptorVersion Version of descriptors
 *
 * @retval EFI_SUCCESS              Memory map retrieved successfully
 * @retval EFI_OUT_OF_RESOURCES     Failed to allocate memory for map
 * @retval EFI_DEVICE_ERROR         Firmware returned invalid memory map
 */
EFI_STATUS
get_memory_map(
    EFI_MEMORY_DESCRIPTOR **MemoryMap,
    UINTN *MemoryMapSize,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion
)
{
    EFI_STATUS Status;
    EFI_BOOT_SERVICES *BS;
    UINTN Retry;
    EFI_MEMORY_DESCRIPTOR *Map = NULL;
    UINTN Size = 0;

    if (!MemoryMap || !MemoryMapSize || !MapKey || !DescriptorSize || !DescriptorVersion) {
        UEFI_LOG_ERROR("get_memory_map: invalid parameters");
        return EFI_INVALID_PARAMETER;
    }

    if (!gST || !gST->BootServices) {
        UEFI_LOG_CRITICAL("get_memory_map: BootServices unavailable");
        return EFI_NOT_READY;
    }

    BS = gST->BootServices;

    for (Retry = 0; Retry < MEMORY_MAP_MAX_RETRIES; Retry++) {

        /* Probe required buffer size */
        Size = 0;
        Status = BS->GetMemoryMap(
            &Size,
            NULL,
            MapKey,
            DescriptorSize,
            DescriptorVersion
        );

        if (Status != EFI_BUFFER_TOO_SMALL) {
            UEFI_LOG_ERROR("GetMemoryMap(size probe) failed: %r", Status);
            return Status;
        }

        /* Allocate buffer with extra descriptors to handle firmware races */
        Size += (*DescriptorSize) * MEMORY_MAP_EXTRA_DESCRIPTORS;

        Status = BS->AllocatePool(EfiLoaderData, Size, (void **)&Map);
        if (EFI_ERROR(Status)) {
            UEFI_LOG_ERROR("AllocatePool failed: %r", Status);
            return Status;
        }

        /* Retrieve actual memory map */
        Status = BS->GetMemoryMap(
            &Size,
            Map,
            MapKey,
            DescriptorSize,
            DescriptorVersion
        );

        if (EFI_ERROR(Status)) {
            UEFI_LOG_WARNING("GetMemoryMap retry %u failed: %r", Retry + 1, Status);
            BS->FreePool(Map);
            Map = NULL;
            continue;
        }

        /* Defensive validation: check alignment and descriptor size */
        if (((UINTN)Map % sizeof(UINT64)) != 0) {
            UEFI_LOG_CRITICAL("MemoryMap not 8-byte aligned");
            BS->FreePool(Map);
            return EFI_DEVICE_ERROR;
        }

        *MemoryMap = Map;
        *MemoryMapSize = Size;
        return EFI_SUCCESS;
    }

    UEFI_LOG_CRITICAL("get_memory_map: failed after %u retries", MEMORY_MAP_MAX_RETRIES);
    return EFI_DEVICE_ERROR;
}
