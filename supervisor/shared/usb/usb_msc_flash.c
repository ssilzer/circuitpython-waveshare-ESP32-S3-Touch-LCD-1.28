// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2018 hathach for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include "tusb.h"
// // #include "supervisor/flash.h"

// For updating fatfs's cache
#include "extmod/vfs.h"
#include "extmod/vfs_fat.h"
#include "lib/oofatfs/diskio.h"
#include "lib/oofatfs/ff.h"
#include "py/gc.h"
#include "py/mpstate.h"

#include "shared-module/storage/__init__.h"
#include "supervisor/filesystem.h"
#include "supervisor/shared/reload.h"

#define MSC_FLASH_BLOCK_SIZE    512

#if CIRCUITPY_SAVES_PARTITION_SIZE > 0
#define SAVES_COUNT 1
#define SAVES_LUN (1)
#else
#define SAVES_COUNT 0
#endif

#if CIRCUITPY_SDCARDIO
#include "shared-module/sdcardio/__init__.h"

#define SDCARD_COUNT 1
#define SDCARD_LUN (1 + SAVES_COUNT)
#else
#define SDCARD_COUNT 0
#endif

#define LUN_COUNT (1 + SAVES_COUNT + SDCARD_COUNT)

// The ellipsis range in the designated initializer of `ejected` is not standard C,
// but it works in both gcc and clang.
static bool ejected[LUN_COUNT] = { [0 ... (LUN_COUNT - 1)] = true};
static bool eject_once[LUN_COUNT] = { [0 ... (LUN_COUNT - 1)] = false};
static bool locked[LUN_COUNT] = { [0 ... (LUN_COUNT - 1)] = false};

#include "tusb.h"

static const uint8_t usb_msc_descriptor_template[] = {
    // MSC Interface Descriptor
    0x09,        //  0 bLength
    0x04,        //  1 bDescriptorType (Interface)
    0xFF,        //  2 bInterfaceNumber [SET AT RUNTIME]
#define MSC_INTERFACE_INDEX (2)
    0x00,        //  3 bAlternateSetting
    0x02,        //  4 bNumEndpoints 2
    0x08,        //  5 bInterfaceClass: MSC
    0x06,        //  6 bInterfaceSubClass: TRANSPARENT
    0x50,        //  7 bInterfaceProtocol: BULK
    0xFF,        //  8 iInterface (String Index) [SET AT RUNTIME]
#define MSC_INTERFACE_STRING_INDEX (8)

    // MSC Endpoint IN Descriptor
    0x07,        //  9 bLength
    0x05,        // 10 bDescriptorType (Endpoint)
    0xFF,        // 11 bEndpointAddress (IN/D2H) [SET AT RUNTIME: 0x80 | number]
#define MSC_IN_ENDPOINT_INDEX (11)
    0x02,        // 12 bmAttributes (Bulk)
    #if USB_HIGHSPEED
    0x00, 0x02,  // 13,14 wMaxPacketSize 512
    #else
    0x40, 0x00,  // 13,14 wMaxPacketSize 64
    #endif
    0x00,        // 15 bInterval 0 (unit depends on device speed)

    // MSC Endpoint OUT Descriptor
    0x07,        // 16 bLength
    0x05,        // 17 bDescriptorType (Endpoint)
    0xFF,        // 18 bEndpointAddress (OUT/H2D) [SET AT RUNTIME]
#define MSC_OUT_ENDPOINT_INDEX (18)
    0x02,        // 19 bmAttributes (Bulk)
    #if USB_HIGHSPEED
    0x00, 0x02,  // 20,21 wMaxPacketSize 512
    #else
    0x40, 0x00,  // 20,21 wMaxPacketSize 64
    #endif
    0x00,        // 22 bInterval 0 (unit depends on device speed)
};

size_t usb_msc_descriptor_length(void) {
    return sizeof(usb_msc_descriptor_template);
}

static const char storage_interface_name[] = USB_INTERFACE_NAME " Mass Storage";

size_t usb_msc_add_descriptor(uint8_t *descriptor_buf, descriptor_counts_t *descriptor_counts, uint8_t *current_interface_string) {
    memcpy(descriptor_buf, usb_msc_descriptor_template, sizeof(usb_msc_descriptor_template));
    descriptor_buf[MSC_INTERFACE_INDEX] = descriptor_counts->current_interface;
    descriptor_counts->current_interface++;

    descriptor_buf[MSC_IN_ENDPOINT_INDEX] =
        0x80 | (USB_MSC_EP_NUM_IN ? USB_MSC_EP_NUM_IN : descriptor_counts->current_endpoint);
    descriptor_counts->num_in_endpoints++;
    // Some TinyUSB devices have issues with bi-directional endpoints
    #ifdef TUD_ENDPOINT_ONE_DIRECTION_ONLY
    descriptor_counts->current_endpoint++;
    #endif

    descriptor_buf[MSC_OUT_ENDPOINT_INDEX] =
        USB_MSC_EP_NUM_OUT ? USB_MSC_EP_NUM_OUT : descriptor_counts->current_endpoint;
    descriptor_counts->num_out_endpoints++;
    descriptor_counts->current_endpoint++;

    usb_add_interface_string(*current_interface_string, storage_interface_name);
    descriptor_buf[MSC_INTERFACE_STRING_INDEX] = *current_interface_string;
    (*current_interface_string)++;

    return sizeof(usb_msc_descriptor_template);
}

// We hardcode LUN -> mount mapping so that it doesn't changes with saves and
// SD card appearing and disappearing.
static fs_user_mount_t *get_vfs(int lun) {
    fs_user_mount_t *root = filesystem_circuitpy();
    if (lun == 0) {
        return root;
    }
    // Other filesystems must be native because we don't guard against exceptions.
    // They must also be off the VM heap so they don't disappear on autoreload.
    #ifdef SAVES_LUN
    if (lun == SAVES_LUN) {
        const char *path_under_mount;
        fs_user_mount_t *saves = filesystem_for_path("/saves", &path_under_mount);
        if (saves != root && (saves->blockdev.flags & MP_BLOCKDEV_FLAG_NATIVE) != 0 && gc_nbytes(saves) == 0) {
            return saves;
        }
    }
    #endif
    #ifdef SDCARD_LUN
    if (lun == SDCARD_LUN) {
        const char *path_under_mount;
        fs_user_mount_t *sdcard = filesystem_for_path("/sd", &path_under_mount);
        if (sdcard != root && (sdcard->blockdev.flags & MP_BLOCKDEV_FLAG_NATIVE) != 0 && gc_nbytes(sdcard) == 0) {
            return sdcard;
        } else {
            // Clear any ejected state so that a re-insert causes it to reappear.
            ejected[SDCARD_LUN] = false;
            locked[SDCARD_LUN] = false;
        }
    }
    #endif
    return NULL;
}

static void _usb_msc_uneject(void) {
    for (uint8_t i = 0; i < LUN_COUNT; i++) {
        ejected[i] = false;
        locked[i] = false;
    }
}

void usb_msc_mount(void) {
    _usb_msc_uneject();
}

void usb_msc_umount(void) {
    for (uint8_t i = 0; i < LUN_COUNT; i++) {
        fs_user_mount_t *vfs = get_vfs(i);
        if (vfs == NULL) {
            continue;
        }
        blockdev_unlock(vfs);
        locked[i] = false;
    }
}

void usb_msc_remount(fs_user_mount_t *fs_mount) {
    for (uint8_t i = 0; i < LUN_COUNT; i++) {
        fs_user_mount_t *vfs = get_vfs(i);
        if (vfs == NULL || vfs != fs_mount) {
            continue;
        }
        ejected[i] = false;
        eject_once[i] = true;
    }
}

uint8_t tud_msc_get_maxlun_cb(void) {
    return LUN_COUNT;
}

// Callback invoked when received an SCSI command not in built-in list below
// - READ_CAPACITY10, READ_FORMAT_CAPACITY, INQUIRY, TEST_UNIT_READY, START_STOP_UNIT, MODE_SENSE6, REQUEST_SENSE
// - READ10 and WRITE10 have their own callbacks
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize) {
    const void *response = NULL;
    int32_t resplen = 0;

    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            // Host is about to read/write etc ... better not to disconnect disk
            resplen = 0;
            break;

        default:
            // Set Sense = Invalid Command Operation
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);

            // negative means error -> tinyusb could stall and/or response with failed status
            resplen = -1;
            break;
    }

    // return len must not larger than bufsize
    if (resplen > bufsize) {
        resplen = bufsize;
    }

    // copy response to stack's buffer if any
    if (response && (resplen > 0)) {
        memcpy(buffer, response, resplen);
    }

    return resplen;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    fs_user_mount_t *vfs = get_vfs(lun);
    disk_ioctl(vfs, GET_SECTOR_COUNT, block_count);
    disk_ioctl(vfs, GET_SECTOR_SIZE, block_size);
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    if (lun >= LUN_COUNT) {
        return false;
    }

    fs_user_mount_t *vfs = get_vfs(lun);
    if (vfs == NULL) {
        return false;
    }
    if (vfs->blockdev.writeblocks[0] == MP_OBJ_NULL || !filesystem_is_writable_by_usb(vfs)) {
        return false;
    }
    // Lock the blockdev once we say we're writable.
    if (!locked[lun] && !blockdev_lock(vfs)) {
        return false;
    }
    locked[lun] = true;
    return true;
}

// Callback invoked when received READ10 command.
// Copy disk's data to buffer (up to bufsize) and return number of copied bytes.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;

    const uint32_t block_count = bufsize / MSC_FLASH_BLOCK_SIZE;

    fs_user_mount_t *vfs = get_vfs(lun);
    uint32_t disk_block_count;
    disk_ioctl(vfs, GET_SECTOR_COUNT, &disk_block_count);

    if (lba + block_count > disk_block_count) {
        return -1;
    }

    disk_read(vfs, buffer, lba, block_count);

    return block_count * MSC_FLASH_BLOCK_SIZE;
}

// Callback invoked when received WRITE10 command.
// Process data in buffer to disk's storage and return number of written bytes
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    (void)offset;
    autoreload_suspend(AUTORELOAD_SUSPEND_USB);

    const uint32_t block_count = bufsize / MSC_FLASH_BLOCK_SIZE;

    fs_user_mount_t *vfs = get_vfs(lun);
    disk_write(vfs, buffer, lba, block_count);
    // Since by getting here we assume the mount is read-only to
    // MicroPython let's update the cached FatFs sector if it's the one
    // we just wrote.
    #if FF_MAX_SS != FF_MIN_SS
    if (vfs->fatfs.ssize == MSC_FLASH_BLOCK_SIZE) {
    #else
    // The compiler can optimize this away.
    if (FF_MAX_SS == FILESYSTEM_BLOCK_SIZE) {
        #endif
        if (lba == vfs->fatfs.winsect && lba > 0) {
            memcpy(vfs->fatfs.win,
                buffer + MSC_FLASH_BLOCK_SIZE * (vfs->fatfs.winsect - lba),
                MSC_FLASH_BLOCK_SIZE);
        }
    }

    return block_count * MSC_FLASH_BLOCK_SIZE;
}

// Callback invoked when WRITE10 command is completed (status received and accepted by host).
// used to flush any pending cache.
void tud_msc_write10_complete_cb(uint8_t lun) {
    (void)lun;

    // This write is complete; initiate an autoreload.
    autoreload_resume(AUTORELOAD_SUSPEND_USB);
    autoreload_trigger();
}

// Invoked when received SCSI_CMD_INQUIRY
// Application fill vendor id, product id and revision with string up to 8, 16, 4 characters respectively
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;

    memcpy(vendor_id, CFG_TUD_MSC_VENDOR, strlen(CFG_TUD_MSC_VENDOR));
    memcpy(product_id, CFG_TUD_MSC_PRODUCT, strlen(CFG_TUD_MSC_PRODUCT));
    memcpy(product_rev, CFG_TUD_MSC_PRODUCT_REV, strlen(CFG_TUD_MSC_PRODUCT_REV));
}

// Invoked when received Test Unit Ready command.
// return true allowing host to read/write this LUN e.g SD card inserted
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (lun >= LUN_COUNT) {
        return false;
    }

    #if CIRCUITPY_SDCARDIO
    if (lun == SDCARD_LUN) {
        automount_sd_card();
    }
    #endif

    fs_user_mount_t *current_mount = get_vfs(lun);
    if (current_mount == NULL) {
        return false;
    }
    if (ejected[lun] || eject_once[lun]) {
        eject_once[lun] = false;
        // Set 0x3a for media not present.
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }

    return true;
}

// Invoked when received Start Stop Unit command
// - Start = 0 : stopped power mode, if load_eject = 1 : unload disk storage
// - Start = 1 : active mode, if load_eject = 1 : load disk storage
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    if (lun >= LUN_COUNT) {
        return false;
    }
    fs_user_mount_t *current_mount = get_vfs(lun);
    if (current_mount == NULL) {
        return false;
    }
    if (load_eject) {
        if (!start) {
            // Eject but first flush.
            if (disk_ioctl(current_mount, CTRL_SYNC, NULL) != RES_OK) {
                return false;
            } else {
                blockdev_unlock(current_mount);
                ejected[lun] = true;
                locked[lun] = false;
            }
        } else {
            // We can only load if it hasn't been ejected.
            return !ejected[lun];
        }
    } else {
        if (!start) {
            // Stop the unit but don't eject.
            if (disk_ioctl(current_mount, CTRL_SYNC, NULL) != RES_OK) {
                return false;
            }
        }
        // Always start the unit, even if ejected. Whether media is present is a separate check.
    }

    return true;
}
