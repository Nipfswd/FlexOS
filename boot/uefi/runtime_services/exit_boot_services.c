/**
 * @file exit_boot_services.c
 * @brief Handles transition from UEFI Boot Services to OS runtime
 *
 * This implementation follows the UEFI specification requirement that
 * ExitBootServices() must be called with a *current* memory map key.
 * Firmware may invalidate the key between calls, so this code retries
 * correctly and defensively.
 *
 * UEFI Spec:
 *  - Section 7.4: ExitBootServices()
 *  - Section 7.2: GetMemoryMap()
 */

#include <uefi/types.h>
#include <uefi/status.h>
#include <uefi/system_table.h>
#include <uefi/boot_services.h>
#include <uefi/memory.h>

#include <boot/uefi/uefi_globals.h>
#include <boot/uefi/uefi_logging.h>
#include <boot/uefi/uefi_assert.h>

/*
 * Maximum retries before giving up.
 * Real firmware *does* race memory map changes.
 */
#define EXIT_BOOT_SERVICES_MAX_RETRIES 8

/**
 * exit_boot_services
 *
 * Safely exits UEFI Boot Services. This function:
 *  - Obtains the current memory map
 *  - Retries if firmware invalidates the key
 *  - Leaves the system in runtime-only state
 *
 * @param[in] ImageHandle The UEFI image handle
 *
 * @retval EFI_SUCCESS            Boot services exited successfully
 * @retval EFI_INVALID_PARAMETER  ImageHandle invalid
 * @retval EFI_ABORTED            Failed after multiple retries
 */
EFI_STATUS
exit_boot_services(EFI_HANDLE ImageHandle)
{
    EFI_STATUS Status;
    EFI_BOOT_SERVICES *BS;

    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;
    UINTN MemoryMapSize = 0;
    UINTN MapKey = 0;
    UINTN DescriptorSize = 0;
    UINT32 DescriptorVersion = 0;

    UINTN Retry;

    if (!ImageHandle) {
        UEFI_LOG_ERROR("ExitBootServices: ImageHandle is NULL");
        return EFI_INVALID_PARAMETER;
    }

    if (!gST || !gST->BootServices) {
        UEFI_LOG_CRITICAL("ExitBootServices: BootServices unavailable");
        return EFI_NOT_READY;
    }

    BS = gST->BootServices;

    for (Retry = 0; Retry < EXIT_BOOT_SERVICES_MAX_RETRIES; Retry++) {

        /*
         * First call to determine required buffer size
         */
        MemoryMapSize = 0;
        Status = BS->GetMemoryMap(
            &MemoryMapSize,
            NULL,
            &MapKey,
            &DescriptorSize,
            &DescriptorVersion
        );

        if (Status != EFI_BUFFER_TOO_SMALL) {
            UEFI_LOG_ERROR("GetMemoryMap(size probe) failed: %r", Status);
            return Status;
        }

        /*
         * Allocate memory map buffer
         * Add extra space to tolerate firmware changes
         */
        MemoryMapSize += DescriptorSize * 8;

        Status = BS->AllocatePool(
            EfiLoaderData,
            MemoryMapSize,
            (void **)&MemoryMap
        );

        if (EFI_ERROR(Status)) {
            UEFI_LOG_ERROR("AllocatePool for memory map failed: %r", Status);
            return Status;
        }

        /*
         * Retrieve actual memory map
         */
        Status = BS->GetMemoryMap(
            &MemoryMapSize,
            MemoryMap,
            &MapKey,
            &DescriptorSize,
            &DescriptorVersion
        );

        if (EFI_ERROR(Status)) {
            UEFI_LOG_ERROR("GetMemoryMap failed: %r", Status);
            BS->FreePool(MemoryMap);
            MemoryMap = NULL;
            continue;
        }

        /*
         * Attempt to exit boot services
         */
        Status = BS->ExitBootServices(ImageHandle, MapKey);

        if (Status == EFI_SUCCESS) {
            UEFI_LOG_INFO("ExitBootServices succeeded");
            return EFI_SUCCESS;
        }

        /*
         * Firmware changed memory map between calls
         * This is expected on real systems
         */
        UEFI_LOG_WARNING(
            "ExitBootServices retry %u failed: %r",
            Retry + 1,
            Status
        );

        BS->FreePool(MemoryMap);
        MemoryMap = NULL;
    }

    UEFI_LOG_CRITICAL("ExitBootServices failed after %u retries",
                      EXIT_BOOT_SERVICES_MAX_RETRIES);

    return EFI_ABORTED;
}
