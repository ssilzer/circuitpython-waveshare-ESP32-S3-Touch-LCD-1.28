// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Jacob Rigby
//
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include "esp_err.h"

#include "driver/sdmmc_host.h"
#include "ports/espressif/esp-idf/components/sdmmc/include/sdmmc_cmd.h"

#include "shared-bindings/microcontroller/Pin.h"
#include "shared-bindings/util.h"
#include "shared-bindings/sdioio/SDCard.h"
#include "py/mperrno.h"
#include "py/runtime.h"

#include "esp_log.h"
static const char *TAG = "SDCard.c";

static bool slot_in_use[2];
static bool never_reset_sdio[2] = { false, false };

static void common_hal_sdioio_sdcard_check_for_deinit(sdioio_sdcard_obj_t *self) {
    if (common_hal_sdioio_sdcard_deinited(self)) {
        raise_deinited_error();
    }
}

static int check_pins(const mcu_pin_obj_t *clock, const mcu_pin_obj_t *command, const uint8_t num_data, const mcu_pin_obj_t **data) {
    #ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    // ESP32-S3 and P4 can use any pin for any SDMMC func in either slot
    // Default to SLOT_1 for SD cards
    ESP_LOGI(TAG, "Using chip with CONFIG_SOC_SDMMC_USE_GPIO_MATRIX");
    if (!slot_in_use[1]) {
        return SDMMC_HOST_SLOT_1;
    } else if (!slot_in_use[0]) {
        return SDMMC_HOST_SLOT_0;
    }
    #else
    if (command->number == GPIO_NUM_11 && clock->number == GPIO_NUM_6 && data[0]->number == GPIO_NUM_7) {
        // Might be slot 0
        if (num_data == 1 || (num_data == 4 && data[1]->number == GPIO_NUM_8 && data[2]->number == GPIO_NUM_9 && data[3]->number == GPIO_NUM_10)) {
            return SDMMC_HOST_SLOT_0;
        }
    } else if (command->number == GPIO_NUM_15 && clock->number == GPIO_NUM_14 && data[0]->number == 2) {
        // Might be slot 1
        if (num_data == 1 || (num_data == 4 && data[1]->number == GPIO_NUM_4 && data[2]->number == GPIO_NUM_12 && data[3]->number == GPIO_NUM_13)) {
            return SDMMC_HOST_SLOT_1;
        }
    }
    #endif
    return -1;
}

uint8_t get_slot_index(sdioio_sdcard_obj_t *self) {
    if (self->slot == SDMMC_HOST_SLOT_0) {
        return 0;
    } else {
        return 1;
    }
}

void common_hal_sdioio_sdcard_construct(sdioio_sdcard_obj_t *self,
    const mcu_pin_obj_t *clock, const mcu_pin_obj_t *command,
    uint8_t num_data, const mcu_pin_obj_t **data, uint32_t frequency) {
    if (num_data != 4 && num_data != 1) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Number of data_pins must be %d or %d, not %d"), 1, 4, num_data);
    }

    self->num_data = num_data;
    int sd_slot = check_pins(clock, command, num_data, data);
    if (sd_slot != SDMMC_HOST_SLOT_0 && sd_slot != SDMMC_HOST_SLOT_1) {
        // Bad pin combo
        raise_ValueError_invalid_pins();
    }

    // max 40Mhz frequency
    mp_arg_validate_int_max(frequency, 40000000, MP_QSTR_frequency);
    ESP_LOGI(TAG, "Using slot %d", sd_slot);
    self->slot = (uint8_t)sd_slot;
    esp_err_t err = ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = frequency / 1000;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = num_data;
    slot_config.clk = clock->number;
    self->clock = clock->number;
    slot_config.cmd = command->number;
    self->command = command->number;
    slot_config.d0 = data[0]->number;
    self->data[0] = data[0]->number;
    if (num_data == 4) {
        slot_config.d1 = data[1]->number;
        self->data[1] = data[1]->number;
        slot_config.d2 = data[2]->number;
        self->data[2] = data[2]->number;
        slot_config.d3 = data[3]->number;
        self->data[3] = data[3]->number;
    }

    ESP_LOGI(TAG, "slot_config:\nwidth: %d, clk: %d, cmd: %d\nd0: %d, d1: %d, d2: %d, d3: %d",
        slot_config.width, slot_config.clk, slot_config.cmd,
        slot_config.d0, slot_config.d1, slot_config.d2, slot_config.d3);

    if (!slot_in_use[0] && !slot_in_use[1]) {
        err = sdmmc_host_init();
        if (err != ESP_OK) {
            mp_raise_OSError_msg_varg(MP_ERROR_TEXT("SDIO Init Error %x"), err);
        }
    }

    err = sdmmc_host_init_slot(sd_slot, &slot_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize SDMMC slot: %x", err);
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("SDIO Init Error %x"), err);
    }
    // sdmmc_card_t card;
    // self->card = malloc(sizeof(sdmmc_card_t));
    err = sdmmc_card_init(&host, &self->card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize SDMMC card: %x", err);
        mp_raise_OSError_msg_varg(MP_ERROR_TEXT("SDIO Init Error %x"), err);
    }

    common_hal_sdioio_sdcard_check_for_deinit(self);

    slot_in_use[get_slot_index(self)] = true;

    claim_pin(clock);
    claim_pin(command);
    for (size_t i = 0; i < num_data; i++) {
        claim_pin(data[i]);
    }

    ESP_LOGI(TAG, "Initialized SD card with ID %d:%d-%s",
        self->card.cid.mfg_id, self->card.cid.oem_id, self->card.cid.name);

    ESP_LOGI(TAG, "Number of sectors: %d with sector_size: %d",
        self->card.csd.capacity, self->card.csd.sector_size);

    self->frequency = self->card.real_freq_khz;
    ESP_LOGI(TAG, "Real frequency is %lu", self->frequency);
    self->capacity = self->card.csd.capacity;  // Reported number of sectors
    ESP_LOGI(TAG, "Reported capacity is %lu", self->capacity);

    return;
}

uint32_t common_hal_sdioio_sdcard_get_count(sdioio_sdcard_obj_t *self) {
    return self->capacity;
}

uint32_t common_hal_sdioio_sdcard_get_frequency(sdioio_sdcard_obj_t *self) {
    return self->frequency;
}

uint8_t common_hal_sdioio_sdcard_get_width(sdioio_sdcard_obj_t *self) {
    return self->num_data;
}

static void check_whole_block(mp_buffer_info_t *bufinfo, int sector_size) {
    if (bufinfo->len % sector_size) {
        mp_raise_ValueError_varg(MP_ERROR_TEXT("Buffer must be a multiple of %d bytes"), sector_size);
    }
}

int common_hal_sdioio_sdcard_writeblocks(sdioio_sdcard_obj_t *self, uint32_t start_block, mp_buffer_info_t *bufinfo) {
    common_hal_sdioio_sdcard_check_for_deinit(self);
    check_whole_block(bufinfo, self->card.csd.sector_size);
    esp_err_t err;
    ESP_LOGI(TAG, "in common_hal_sdioio_sdcard_writeblocks");
    // err = sdmmc_io_write_blocks(&self->card, 1, start_block, bufinfo->buf, bufinfo->len);
    err = sdmmc_write_sectors(&self->card, bufinfo->buf, start_block, bufinfo->len / self->card.csd.sector_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to write blocks with err 0x%X", err);
    }
    return 0;
}

int common_hal_sdioio_sdcard_readblocks(sdioio_sdcard_obj_t *self, uint32_t start_block, mp_buffer_info_t *bufinfo) {
    common_hal_sdioio_sdcard_check_for_deinit(self);
    check_whole_block(bufinfo, self->card.csd.sector_size);
    esp_err_t err;
    ESP_LOGI(TAG, "in common_hal_sdioio_sdcard_readblocks");
    // err = sdmmc_io_read_blocks(&self->card, 1, start_block, bufinfo->buf, bufinfo->len);
    err = sdmmc_read_sectors(&self->card, bufinfo->buf, start_block, bufinfo->len / self->card.csd.sector_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read blocks with err 0x%X", err);
    }
    return 0;
}

bool common_hal_sdioio_sdcard_configure(sdioio_sdcard_obj_t *self, uint32_t frequency, uint8_t bits) {
    return true;
}

bool common_hal_sdioio_sdcard_deinited(sdioio_sdcard_obj_t *self) {
    return self->command == COMMON_HAL_MCU_NO_PIN;
}

void common_hal_sdioio_sdcard_deinit(sdioio_sdcard_obj_t *self) {
    if (common_hal_sdioio_sdcard_deinited(self)) {
        return;
    }

    never_reset_sdio[get_slot_index(self)] = false;
    slot_in_use[get_slot_index(self)] = false;

    if (!slot_in_use[0] && !slot_in_use[1]) {
        sdmmc_host_deinit();
    }

    reset_pin_number(self->command);
    self->command = COMMON_HAL_MCU_NO_PIN;
    reset_pin_number(self->clock);
    self->clock = COMMON_HAL_MCU_NO_PIN;
    for (size_t i = 0; i < self->num_data; i++) {
        reset_pin_number(self->data[i]);
        self->data[i] = COMMON_HAL_MCU_NO_PIN;
    }

    return;
}

void common_hal_sdioio_sdcard_never_reset(sdioio_sdcard_obj_t *self) {
    if (common_hal_sdioio_sdcard_deinited(self)) {
        return;
    }

    if (never_reset_sdio[get_slot_index(self)]) {
        return;
    }

    never_reset_sdio[get_slot_index(self)] = true;

    never_reset_pin_number(self->command);
    never_reset_pin_number(self->clock);

    for (size_t i = 0; i < self->num_data; i++) {
        never_reset_pin_number(self->data[i]);
    }
}

void sdioio_reset() {
    for (size_t i = 0; i < MP_ARRAY_SIZE(slot_in_use); i++) {
        if (!never_reset_sdio[i]) {
            slot_in_use[i] = false;
        }
    }
    if (!slot_in_use[0] && !slot_in_use[1]) {
        sdmmc_host_deinit();
    }

    return;
}
