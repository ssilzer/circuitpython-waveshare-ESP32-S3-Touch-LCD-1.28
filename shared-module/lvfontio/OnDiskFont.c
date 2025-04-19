// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2024 Scott Shawcroft for Adafruit Industries
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "shared-bindings/lvfontio/OnDiskFont.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "py/stream.h"
#include "py/objstr.h"
#include "py/gc.h"
#include "shared-bindings/displayio/Bitmap.h"
#include "extmod/vfs_fat.h"
#include "lib/oofatfs/ff.h"
#include "supervisor/shared/translate/translate.h"
#include "supervisor/port.h"
#include "supervisor/shared/serial.h"
#include "supervisor/filesystem.h"

// Helper functions for memory allocation
static inline void *allocate_memory(lvfontio_ondiskfont_t *self, size_t size) {
    void *ptr;
    if (self->use_gc_allocator) {
        ptr = m_malloc_maybe(size);
    } else {
        ptr = port_malloc(size, false);
    }
    if (ptr != NULL) {
        return ptr;
    }
    common_hal_lvfontio_ondiskfont_deinit(self);
    if (self->use_gc_allocator) {
        m_malloc_fail(size);
    }
    return NULL;
}

static inline void free_memory(lvfontio_ondiskfont_t *self, void *ptr) {
    if (self->use_gc_allocator) {
        m_free(ptr);
    } else {
        port_free(ptr);
    }
}

// Forward declarations for helper functions
static int16_t find_codepoint_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint);
static uint16_t find_free_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint);
static FRESULT read_bits(FIL *file, size_t num_bits, uint8_t *byte_val, uint8_t *remaining_bits, uint32_t *result);
static FRESULT read_glyph_dimensions(FIL *file, lvfontio_ondiskfont_t *self, uint32_t *advance_width, int32_t *bbox_x, int32_t *bbox_y, uint32_t *bbox_w, uint32_t *bbox_h, uint8_t *byte_val, uint8_t *remaining_bits);

// Load font header data from file
static bool load_font_header(lvfontio_ondiskfont_t *self, FIL *file, size_t *max_slots) {
    UINT bytes_read;
    FRESULT res;

    // Start at the beginning of the file
    res = f_lseek(file, 0);
    if (res != FR_OK) {
        return false;
    }

    uint8_t buffer[8];
    bool found_head = false;
    bool found_cmap = false;
    bool found_loca = false;
    bool found_glyf = false;


    size_t current_position = 0;

    // Read sections until we find all the sections we need or reach end of file
    while (true) {
        // Read section size (4 bytes)
        res = f_read(file, buffer, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            break; // Read error or end of file
        }

        uint32_t section_size = buffer[0] | (buffer[1] << 8) |
            (buffer[2] << 16) | (buffer[3] << 24);

        if (section_size == 0) {
            break; // End of sections marker
        }

        // Read section marker (4 bytes)
        res = f_read(file, buffer, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            break; // Read error or unexpected end of file
        }


        // Make a null-terminated copy of the section marker for debug printing
        char section_marker[5] = {0};
        memcpy(section_marker, buffer, 4);

        // Process different section types
        if (memcmp(buffer, "head", 4) == 0) {
            // Read head section data (35 bytes)
            uint8_t head_buf[35];
            res = f_read(file, head_buf, 35, &bytes_read);
            if (res != FR_OK || bytes_read < 35) {
                break;
            }

            // Skip version (4 bytes) and padding (1 byte)
            // Parse font metrics at offset 6
            self->header.font_size = head_buf[6] | (head_buf[7] << 8);
            self->header.ascent = head_buf[8] | (head_buf[9] << 8);
            self->header.default_advance_width = head_buf[22] | (head_buf[23] << 8);

            // Parse format information
            self->header.index_to_loc_format = head_buf[26];
            self->header.bits_per_pixel = head_buf[29];
            self->header.glyph_bbox_xy_bits = head_buf[30];
            self->header.glyph_bbox_wh_bits = head_buf[31];
            self->header.glyph_advance_bits = head_buf[32];

            // Calculate derived values
            self->header.glyph_header_bits = self->header.glyph_advance_bits +
                2 * self->header.glyph_bbox_xy_bits +
                2 * self->header.glyph_bbox_wh_bits;
            self->header.glyph_header_bytes = (self->header.glyph_header_bits + 7) / 8;

            found_head = true;
        } else if (memcmp(buffer, "cmap", 4) == 0) {
            // Read subtable count
            uint8_t cmap_header[4];
            res = f_read(file, cmap_header, 4, &bytes_read);
            if (res != FR_OK || bytes_read < 4) {
                break;
            }

            uint32_t subtable_count = cmap_header[0] | (cmap_header[1] << 8) |
                (cmap_header[2] << 16) | (cmap_header[3] << 24);

            // Allocate memory for cmap ranges
            self->cmap_range_count = subtable_count;
            self->cmap_ranges = allocate_memory(self, sizeof(lvfontio_cmap_range_t) * subtable_count);
            if (self->cmap_ranges == NULL) {
                return false;
            }

            // Read each subtable
            for (uint16_t i = 0; i < subtable_count; i++) {
                uint8_t subtable_buf[16];
                res = f_read(file, subtable_buf, 16, &bytes_read);
                if (res != FR_OK || bytes_read < 16) {
                    break;
                }

                // Read data_offset (4 bytes)
                uint32_t data_offset = subtable_buf[0] | (subtable_buf[1] << 8) |
                    (subtable_buf[2] << 16) | (subtable_buf[3] << 24);

                // Read range_start, range_length, glyph_offset
                uint32_t range_start = subtable_buf[4] | (subtable_buf[5] << 8) |
                    (subtable_buf[6] << 16) | (subtable_buf[7] << 24);
                uint16_t range_length = subtable_buf[8] | (subtable_buf[9] << 8);
                uint16_t glyph_offset = subtable_buf[10] | (subtable_buf[11] << 8);
                uint16_t entries_count = subtable_buf[12] | (subtable_buf[13] << 8);

                // Get format type (0=sparse mapping, 1=range mapping, 2=range to range, 3=direct mapping)
                uint8_t format_type = subtable_buf[14];
                // Check for supported format types (0, 2, and 3)
                if (format_type != 0 && format_type != 2 && format_type != 3) {
                    continue;
                }

                // Store the range information
                self->cmap_ranges[i].range_start = range_start;
                self->cmap_ranges[i].range_end = range_start + range_length;
                self->cmap_ranges[i].glyph_offset = glyph_offset;
                self->cmap_ranges[i].format_type = format_type;
                self->cmap_ranges[i].data_offset = current_position + data_offset;
                self->cmap_ranges[i].entries_count = entries_count;
            }

            found_cmap = true;
        } else if (memcmp(buffer, "loca", 4) == 0) {
            // Read max_cid
            uint8_t loca_header[4];
            res = f_read(file, loca_header, 4, &bytes_read);
            if (res != FR_OK || bytes_read < 4) {
                break;
            }

            // Store max_cid value
            self->max_cid = loca_header[0] | (loca_header[1] << 8) |
                (loca_header[2] << 16) | (loca_header[3] << 24);

            // Store location of the loca table offset data
            self->loca_table_offset = current_position + 12;

            found_loca = true;
        } else if (memcmp(buffer, "glyf", 4) == 0) {
            // Store start of glyf table
            self->glyf_table_offset = current_position;
            size_t advances[2] = {0, 0};
            size_t advance_count[2] = {0, 0};

            if (self->header.default_advance_width != 0) {
                advances[0] = self->header.default_advance_width;
            }

            // Set the default advance width based on the first character in the
            // file.
            size_t cid = 0;
            while (cid < self->max_cid - 1) {
                // Read glyph header fields
                uint32_t glyph_advance;
                int32_t bbox_x, bbox_y;
                uint32_t bbox_w, bbox_h;

                uint8_t byte_val = 0;
                uint8_t remaining_bits = 0;

                // Use the helper function to read glyph dimensions
                read_glyph_dimensions(file, self, &glyph_advance, &bbox_x, &bbox_y, &bbox_w, &bbox_h, &byte_val, &remaining_bits);

                // Throw away the bitmap bits.
                read_bits(file, self->header.bits_per_pixel * bbox_w * bbox_h, &byte_val, &remaining_bits, NULL);
                if (advances[0] == glyph_advance) {
                    advance_count[0]++;
                } else if (advances[1] == glyph_advance) {
                    advance_count[1]++;
                } else if (advance_count[0] == 0) {
                    advances[0] = glyph_advance;
                    advance_count[0] = 1;
                } else if (advance_count[1] == 0) {
                    advances[1] = glyph_advance;
                    advance_count[1] = 1;
                } else {
                    break;
                }
                cid++;
            }

            if (self->header.default_advance_width == 0) {
                if (advance_count[1] == 0) {
                    self->header.default_advance_width = advances[0];
                    *max_slots = advance_count[0];
                } else {
                    if (advances[0] > advances[1]) {
                        self->header.default_advance_width = advances[0] / 2;
                        *max_slots = advance_count[0] * 2 + advance_count[1];
                    } else {
                        self->header.default_advance_width = advances[1] / 2;
                        *max_slots = advance_count[1] * 2 + advance_count[0];
                    }
                }
            }


            found_glyf = true;
        }

        current_position += section_size;

        // Skip to the end of the section
        res = f_lseek(file, current_position);
        if (res != FR_OK) {
            break;
        }

        // If we found all needed sections, we can stop
        if (found_head && found_cmap && found_loca && found_glyf) {
            break;
        }
    }

    // Check if we found all required sections
    if (!found_head || !found_cmap || !found_loca || !found_glyf) {
        return false;
    }

    return true;
}

// Get character ID (glyph index) for a codepoint
static int32_t get_char_id(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    // Find codepoint in cmap ranges
    for (uint16_t i = 0; i < self->cmap_range_count; i++) {
        // Check if codepoint is in range for this subtable
        if (codepoint >= self->cmap_ranges[i].range_start &&
            codepoint < self->cmap_ranges[i].range_end) {

            // Handle according to format type
            switch (self->cmap_ranges[i].format_type) {
                case 0: { // Sparse mapping - need to look up in a sparse table
                    if (!self->file_is_open) {
                        return -1;
                    }

                    // Calculate the relative position within the range
                    uint32_t idx = codepoint - self->cmap_ranges[i].range_start;

                    if (idx >= self->cmap_ranges[i].entries_count) {
                        return -1;
                    }

                    // Calculate the absolute data position in the file
                    uint32_t data_pos = self->cmap_ranges[i].data_offset + idx; // 1 byte per entry
                    FRESULT res = f_lseek(&self->file, data_pos);
                    if (res != FR_OK) {
                        return -1;
                    }

                    // Read the glyph ID (1 byte)
                    uint8_t glyph_id;
                    UINT bytes_read;
                    res = f_read(&self->file, &glyph_id, 1, &bytes_read);

                    if (res != FR_OK || bytes_read < 1) {
                        return -1;
                    }


                    return self->cmap_ranges[i].glyph_offset + glyph_id;
                }

                case 2: // Range to range - calculate based on offset within range
                    uint16_t idx = codepoint - self->cmap_ranges[i].range_start;
                    uint16_t glyph_id = self->cmap_ranges[i].glyph_offset + idx;
                    return glyph_id;

                case 3: { // Direct mapping - need to look up in the table
                    if (!self->file_is_open) {
                        return -1;
                    }

                    FRESULT res;
                    res = f_lseek(&self->file, self->cmap_ranges[i].data_offset);
                    if (res != FR_OK) {
                        return -1;
                    }
                    uint16_t codepoint_delta = codepoint - self->cmap_ranges[i].range_start;

                    for (size_t j = 0; j < self->cmap_ranges[i].entries_count; j++) {
                        // Read code point at the index
                        uint16_t candidate_codepoint_delta;
                        res = f_read(&self->file, &candidate_codepoint_delta, 2, NULL);
                        if (res != FR_OK) {
                            return -1;
                        }

                        if (candidate_codepoint_delta == codepoint_delta) {
                            return self->cmap_ranges[i].glyph_offset + j;
                        }
                    }
                    return -1;
                }

                default:
                    return -1;
            }
        }
    }

    return -1; // Not found
}

// Load glyph bitmap data into a slot
// This function assumes the file is already open and positioned after reading the glyph dimensions
static bool load_glyph_bitmap(FIL *file, lvfontio_ondiskfont_t *self, uint32_t codepoint, uint16_t slot,
    uint32_t glyph_advance, int32_t bbox_x, int32_t bbox_y, uint32_t bbox_w, uint32_t bbox_h,
    uint8_t *byte_val, uint8_t *remaining_bits) {
    // Store codepoint at slot
    self->codepoints[slot] = codepoint;
    self->reference_counts[slot] = 1;

    // Read bitmap data pixel by pixel
    uint16_t x_offset = slot * self->header.default_advance_width;
    uint16_t y_offset = self->header.ascent - bbox_y - bbox_h;
    for (uint16_t y = 0; y < bbox_h; y++) {
        for (uint16_t x = 0; x < bbox_w; x++) {
            uint32_t pixel_value;
            FRESULT res = read_bits(file, self->header.bits_per_pixel, byte_val, remaining_bits, &pixel_value);
            if (res != FR_OK) {
                return false;
            }

            // Adjust for bbox position within the glyph bounding box
            int16_t bitmap_x = x_offset + x + bbox_x;
            int16_t bitmap_y = y_offset + y;

            // Make sure we're in bounds
            if (bitmap_x >= 0 &&
                bitmap_x < self->header.default_advance_width * self->max_glyphs &&
                bitmap_y >= 0 &&
                bitmap_y < self->header.font_size) {
                common_hal_displayio_bitmap_set_pixel(
                    self->bitmap,
                    bitmap_x,
                    bitmap_y,
                    pixel_value
                    );
            }
        }
    }

    return true;
}

// Constructor
void common_hal_lvfontio_ondiskfont_construct(lvfontio_ondiskfont_t *self,
    const char *file_path,
    uint16_t max_glyphs,
    bool use_gc_allocator) {

    // Store the allocation mode
    self->use_gc_allocator = use_gc_allocator;
    // Store parameters
    self->file_path = file_path; // Store the provided path string directly
    self->max_glyphs = max_glyphs;
    self->cmap_ranges = NULL;
    self->file_is_open = false;

    // Determine which filesystem to use based on the path
    const char *path_under_mount;
    fs_user_mount_t *vfs = filesystem_for_path(file_path, &path_under_mount);

    if (vfs == NULL) {
        if (self->use_gc_allocator) {
            mp_raise_ValueError(MP_ERROR_TEXT("File not found"));
        }
        return;
    }

    // Open the file and keep it open for the lifetime of the object
    FRESULT res = f_open(&vfs->fatfs, &self->file, path_under_mount, FA_READ);

    if (res != FR_OK) {
        if (self->use_gc_allocator) {
            mp_raise_ValueError(MP_ERROR_TEXT("File not found"));
        }
        return;
    }

    self->file_is_open = true;

    // Load font headers
    size_t max_slots;
    if (!load_font_header(self, &self->file, &max_slots)) {
        f_close(&self->file);
        self->file_is_open = false;
        if (self->use_gc_allocator) {
            mp_raise_ValueError_varg(MP_ERROR_TEXT("Invalid %q"), MP_QSTR_file);
        }
        return;
    }
    // Cap the number of slots to the number of slots needed by the font. That way
    // small font files don't need a bunch of extra cache space.
    max_glyphs = MIN(max_glyphs, max_slots);

    // Allocate codepoints array. allocate_memory will raise an exception if
    // allocation fails and the VM is active.
    self->codepoints = allocate_memory(self, sizeof(uint32_t) * max_glyphs);
    if (self->codepoints == NULL) {
        return;
    }

    // Initialize codepoints to invalid
    for (uint16_t i = 0; i < max_glyphs; i++) {
        self->codepoints[i] = LVFONTIO_INVALID_CODEPOINT;
    }

    // Allocate reference counts
    self->reference_counts = allocate_memory(self, sizeof(uint16_t) * max_glyphs);
    if (self->reference_counts == NULL) {
        return;
    }

    // Initialize reference counts to 0
    memset(self->reference_counts, 0, sizeof(uint16_t) * max_glyphs);

    self->half_width_px = self->header.default_advance_width;

    // Create bitmap for glyph cache
    displayio_bitmap_t *bitmap = allocate_memory(self, sizeof(displayio_bitmap_t));
    bitmap->base.type = &displayio_bitmap_type;
    if (bitmap == NULL) {
        return;
    }

    // Calculate bitmap stride
    uint32_t bits_per_pixel = 1 << self->header.bits_per_pixel;
    uint32_t width = self->header.default_advance_width * max_glyphs;
    uint32_t row_width = width * bits_per_pixel;
    uint16_t stride = (row_width + 31) / 32; // Align to uint32_t (32 bits)

    // Allocate buffer for bitmap data
    uint32_t buffer_size = stride * self->header.font_size * sizeof(uint32_t);
    uint32_t *bitmap_buffer = allocate_memory(self, buffer_size);
    if (bitmap_buffer == NULL) {
        return;
    }

    // Zero out bitmap buffer
    memset(bitmap_buffer, 0, buffer_size);

    // Construct bitmap with allocated buffer
    common_hal_displayio_bitmap_construct_from_buffer(bitmap,
        self->header.default_advance_width * max_glyphs,
        self->header.font_size,
        1 << self->header.bits_per_pixel,
            bitmap_buffer,
            false);
    self->bitmap = bitmap;
}

void common_hal_lvfontio_ondiskfont_deinit(lvfontio_ondiskfont_t *self) {
    if (!self->file_is_open) {
        return;
    }

    if (self->bitmap != NULL) {
        common_hal_displayio_bitmap_deinit(self->bitmap);
        self->bitmap = NULL;
    }

    if (self->codepoints != NULL) {
        free_memory(self, self->codepoints);
        self->codepoints = NULL;
    }

    if (self->reference_counts != NULL) {
        free_memory(self, self->reference_counts);
        self->reference_counts = NULL;
    }



    if (self->cmap_ranges != NULL) {
        free_memory(self, self->cmap_ranges);
        self->cmap_ranges = NULL;
    }

    f_close(&self->file);
    self->file_is_open = false;
}

bool common_hal_lvfontio_ondiskfont_deinited(lvfontio_ondiskfont_t *self) {
    return !self->file_is_open;
}

mp_obj_t common_hal_lvfontio_ondiskfont_get_bitmap(const lvfontio_ondiskfont_t *self) {
    return MP_OBJ_FROM_PTR(self->bitmap);
}

mp_obj_t common_hal_lvfontio_ondiskfont_get_bounding_box(const lvfontio_ondiskfont_t *self) {
    mp_obj_t bbox[2];
    bbox[0] = MP_OBJ_NEW_SMALL_INT(self->header.default_advance_width);
    bbox[1] = MP_OBJ_NEW_SMALL_INT(self->header.font_size);
    return mp_obj_new_tuple(2, bbox);
}

void common_hal_lvfontio_ondiskfont_get_dimensions(const lvfontio_ondiskfont_t *self,
    uint16_t *width, uint16_t *height) {
    if (width != NULL) {
        *width = self->header.default_advance_width;
    }
    if (height != NULL) {
        *height = self->header.font_size;
    }
}

int16_t common_hal_lvfontio_ondiskfont_cache_glyph(lvfontio_ondiskfont_t *self, uint32_t codepoint, bool *is_full_width) {
    // Check if already cached
    int16_t existing_slot = find_codepoint_slot(self, codepoint);
    if (existing_slot >= 0) {
        // Glyph is already cached, increment reference count
        self->reference_counts[existing_slot]++;

        // Check if this is a full-width character by looking for a second slot
        // with the same codepoint right after this one
        if (is_full_width != NULL) {
            if (existing_slot + 1 < self->max_glyphs &&
                self->codepoints[existing_slot + 1] == codepoint) {
                *is_full_width = true;
            } else {
                *is_full_width = false;
            }
        }

        return existing_slot;
    }

    // First check if the glyph is full-width before allocating slots
    // This way we know if we need one or two slots before committing
    bool is_full_width_glyph = false;

    // Check if file is already open
    if (!self->file_is_open) {

        return -1;
    }

    // Find character ID from codepoint
    int32_t char_id = get_char_id(self, codepoint);
    if (char_id < 0 || (uint32_t)char_id >= self->max_cid) {
        return -1; // Invalid character
    }

    // Get glyph offset from location table
    uint32_t glyph_offset = 0;
    uint32_t loca_offset = self->loca_table_offset + char_id *
        (self->header.index_to_loc_format == 1 ? 4 : 2);

    FRESULT res = f_lseek(&self->file, loca_offset);
    if (res != FR_OK) {
        return -1;
    }

    UINT bytes_read;
    if (self->header.index_to_loc_format == 1) {
        // 4-byte offset
        uint8_t offset_buf[4];
        res = f_read(&self->file, offset_buf, 4, &bytes_read);
        if (res != FR_OK || bytes_read < 4) {
            return -1;
        }
        glyph_offset = offset_buf[0] | (offset_buf[1] << 8) |
            (offset_buf[2] << 16) | (offset_buf[3] << 24);
    } else {
        // 2-byte offset
        uint8_t offset_buf[2];
        res = f_read(&self->file, offset_buf, 2, &bytes_read);
        if (res != FR_OK || bytes_read < 2) {
            return -1;
        }
        glyph_offset = offset_buf[0] | (offset_buf[1] << 8);
    }
    // Seek to glyph data
    res = f_lseek(&self->file, self->glyf_table_offset + glyph_offset);
    if (res != FR_OK) {
        return -1;
    }

    // Read glyph header fields to determine width
    uint32_t glyph_advance;
    int32_t bbox_x, bbox_y;
    uint32_t bbox_w, bbox_h;

    // Initialize bit reading state
    uint8_t byte_val = 0;
    uint8_t remaining_bits = 0;

    // Use the helper function to read glyph dimensions
    res = read_glyph_dimensions(&self->file, self, &glyph_advance, &bbox_x, &bbox_y, &bbox_w, &bbox_h, &byte_val, &remaining_bits);
    if (res != FR_OK) {
        return -1;
    }

    // Check if the glyph is full-width based on its advance width
    // Full-width characters typically have an advance width close to or greater than the font height
    is_full_width_glyph = glyph_advance > self->half_width_px;

    // Now we know if we need one or two slots
    uint16_t slots_needed = is_full_width_glyph ? 2 : 1;

    // Find an appropriate slot (or consecutive slots for full-width)
    uint16_t slot = UINT16_MAX;

    if (slots_needed == 1) {
        // For regular width, find a free slot starting at codepoint's position
        slot = find_free_slot(self, codepoint);
    } else {
        // For full-width, find two consecutive free slots
        for (uint16_t i = 0; i < self->max_glyphs - 1; i++) {
            if (self->codepoints[i] == LVFONTIO_INVALID_CODEPOINT &&
                self->reference_counts[i] == 0 &&
                self->codepoints[i + 1] == LVFONTIO_INVALID_CODEPOINT &&
                self->reference_counts[i + 1] == 0) {
                slot = i;
                break;
            }
        }
    }

    // Check if we found appropriate slot(s)
    if (slot == UINT16_MAX) {
        return -1; // No slots available
    }

    // Load glyph into the slot
    if (!load_glyph_bitmap(&self->file, self, codepoint, slot, glyph_advance,
        bbox_x, bbox_y, bbox_w, bbox_h, &byte_val, &remaining_bits)) {
        return -1; // Failed to load glyph
    }

    // For full-width characters, mark both slots with the same codepoint
    if (is_full_width_glyph && slot + 1 < self->max_glyphs) {
        self->codepoints[slot + 1] = codepoint;
        self->reference_counts[slot + 1] = 1;
    }

    if (is_full_width != NULL) {
        *is_full_width = is_full_width_glyph;
    }

    return slot;
}

void common_hal_lvfontio_ondiskfont_release_glyph(lvfontio_ondiskfont_t *self, uint32_t slot) {
    if (slot >= self->max_glyphs) {
        return;
    }

    if (self->reference_counts[slot] > 0) {
        self->reference_counts[slot]--;
    }
}

static int16_t find_codepoint_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    size_t offset = codepoint % self->max_glyphs;
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        int16_t slot = (i + offset) % self->max_glyphs;
        if (self->codepoints[slot] == codepoint) {
            return slot;
        }
    }
    return -1;
}

static uint16_t find_free_slot(lvfontio_ondiskfont_t *self, uint32_t codepoint) {
    size_t offset = codepoint % self->max_glyphs;

    // First look for completely unused slots, starting at the offset
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        int16_t slot = (i + offset) % self->max_glyphs;
        if (self->codepoints[slot] == LVFONTIO_INVALID_CODEPOINT && self->reference_counts[slot] == 0) {
            return slot;
        }
    }

    // If none found, look for slots with zero reference count, starting at the offset
    for (uint16_t i = 0; i < self->max_glyphs; i++) {
        int16_t slot = (i + offset) % self->max_glyphs;
        if (self->reference_counts[slot] == 0) {
            return slot;
        }
    }

    // No slots available
    return UINT16_MAX;
}

static FRESULT read_glyph_dimensions(FIL *file, lvfontio_ondiskfont_t *self,
    uint32_t *advance_width, int32_t *bbox_x, int32_t *bbox_y,
    uint32_t *bbox_w, uint32_t *bbox_h,
    uint8_t *byte_val, uint8_t *remaining_bits) {
    FRESULT res;
    uint32_t temp_value;

    // Read glyph_advance
    res = read_bits(file, self->header.glyph_advance_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    *advance_width = temp_value;

    // Read bbox_x (signed)
    res = read_bits(file, self->header.glyph_bbox_xy_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    // Convert to signed value if needed
    if (temp_value & (1 << (self->header.glyph_bbox_xy_bits - 1))) {
        *bbox_x = temp_value - (1 << self->header.glyph_bbox_xy_bits);
    } else {
        *bbox_x = temp_value;
    }

    // Read bbox_y (signed)
    res = read_bits(file, self->header.glyph_bbox_xy_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    // Convert to signed value if needed
    if (temp_value & (1 << (self->header.glyph_bbox_xy_bits - 1))) {
        *bbox_y = temp_value - (1 << self->header.glyph_bbox_xy_bits);
    } else {
        *bbox_y = temp_value;
    }

    // Read bbox_w
    res = read_bits(file, self->header.glyph_bbox_wh_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    *bbox_w = temp_value;

    // Read bbox_h
    res = read_bits(file, self->header.glyph_bbox_wh_bits, byte_val, remaining_bits, &temp_value);
    if (res != FR_OK) {
        return res;
    }
    *bbox_h = temp_value;

    return FR_OK;
}

static FRESULT read_bits(FIL *file, size_t num_bits, uint8_t *byte_val, uint8_t *remaining_bits, uint32_t *result) {
    FRESULT res = FR_OK;
    UINT bytes_read;

    uint32_t value = 0;
    // Bits will be lost when num_bits > 32. However, this is good for skipping bits.
    size_t bits_needed = num_bits;

    while (bits_needed > 0) {
        // If no bits remaining, read a new byte
        if (*remaining_bits == 0) {
            res = f_read(file, byte_val, 1, &bytes_read);
            if (res != FR_OK || bytes_read < 1) {
                return FR_DISK_ERR;
            }
            *remaining_bits = 8;
        }

        // Calculate how many bits to take from current byte
        uint8_t bits_to_take = (*remaining_bits < bits_needed) ? *remaining_bits : bits_needed;
        value = (value << bits_to_take) | (*byte_val >> (8 - bits_to_take));

        // Update state
        *remaining_bits -= bits_to_take;
        bits_needed -= bits_to_take;

        // Shift byte for next read
        *byte_val <<= bits_to_take;
        *byte_val &= 0xFF;
    }

    if (result != NULL) {
        *result = value;
    }
    return FR_OK;
}
