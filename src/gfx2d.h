/**
 * @file gfx2d.h
 * @brief Public API for the Microchip SAM9X7 2D Graphics Engine (GFX2D) driver.
 * @version 1.0
 * @date 2025-08-11
 *
 * @details This driver provides a high-level interface to the GFX2D peripheral.
 * It uses a statically allocated command buffer and supports both blocking and
 * interrupt-driven workflows.
 *
 * @note Designed for Microchip SAM9X7 series MCUs.
 */

#ifndef GFX2D_H_
#define GFX2D_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "definitions.h"

// Size of the internal command buffer in bytes.
#define GFX2D_COMMAND_BUFFER_SIZE_BYTES (4096)

// Provides a hook for the platform-specific cache cleaning function.
#ifndef GFX2D_CLEAN_DCACHE_BY_ADDR
#define GFX2D_CLEAN_DCACHE_BY_ADDR(addr, size) SYS_CACHE_CleanDCache_by_Addr(addr, size)
#endif


// Type Definitions and Enums

/**
 * @brief Error codes returned by driver functions.
 */
typedef enum {
    GFX2D_SUCCESS = 0,               // Operation was successful.
    GFX2D_ERROR_INVALID_ARG,         // Invalid argument (e.g., NULL pointer, zero dimension).
    GFX2D_ERROR_NOT_INITIALIZED,     // Driver has not been initialized.
    GFX2D_ERROR_BUSY,                // A blocking operation timed out.
    GFX2D_ERROR_BUFFER_FULL,         // Not enough space in the command buffer for the operation.
} gfx2d_error_t;

/**
 * @brief Pixel formats for surfaces.
 */
typedef enum {
    GFX2D_PF_A4IDX4   = 0,  // 4-bit indexed color with 4-bit alpha.
    GFX2D_PF_A8       = 1,  // 8-bit alpha only.
    GFX2D_PF_IDX8     = 2,  // 8-bit indexed color (uses CLUT).
    GFX2D_PF_A8IDX8   = 3,  // 8-bit indexed color with 8-bit alpha.
    GFX2D_PF_RGB444   = 4,  // 12 bpp, 4 bits per channel.
    GFX2D_PF_ARGB4444 = 5,  // 16 bpp, 4 bits per channel including alpha.
    GFX2D_PF_RGB555   = 6,  // 15 bpp, 5 bits per channel.
    GFX2D_PF_TRGB1555 = 7,  // 16 bpp, 1-bit transparency, 5 bits per color.
    GFX2D_PF_RGBT5551 = 8,  // 16 bpp, 5 bits per color, 1-bit transparency.
    GFX2D_PF_RGB565   = 9,  // 16 bpp, 5-6-5 bits for R-G-B.
    GFX2D_PF_RGB888   = 10, // 24 bpp, 8 bits per pixel.
    GFX2D_PF_ARGB8888 = 11, // 32 bpp, 8 bits per channel including alpha.
    GFX2D_PF_RGBA8888 = 12, // 32 bpp, 8 bits per channel including alpha.
} gfx2d_pixel_format_t;

/**
 * @brief Blending factors for source and destination pixels.
 */
typedef enum {
    GFX2D_FACTOR_ZERO,
    GFX2D_FACTOR_ONE,
    GFX2D_FACTOR_SRC_COLOR,
    GFX2D_FACTOR_ONE_MINUS_SRC_COLOR,
    GFX2D_FACTOR_DST_COLOR,
    GFX2D_FACTOR_ONE_MINUS_DST_COLOR,
    GFX2D_FACTOR_SRC_ALPHA,
    GFX2D_FACTOR_ONE_MINUS_SRC_ALPHA,
    GFX2D_FACTOR_DST_ALPHA,
    GFX2D_FACTOR_ONE_MINUS_DST_ALPHA,
    GFX2D_FACTOR_CONSTANT_COLOR,
    GFX2D_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    GFX2D_FACTOR_CONSTANT_ALPHA,
    GFX2D_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    GFX2D_FACTOR_SRC_ALPHA_SATURATE,
} gfx2d_blend_factor_t;

/**
 * @brief Blending function to apply between surfaces.
 */
typedef enum {
    GFX2D_BLEND_FUNC_ADD,
    GFX2D_BLEND_FUNC_SUBTRACT,
    GFX2D_BLEND_FUNC_REVERSE,
    GFX2D_BLEND_FUNC_MIN,
    GFX2D_BLEND_FUNC_MAX,
    GFX2D_BLEND_FUNC_SPECIAL,
} gfx2d_blend_func_t;

/**
 * @brief Special (non-standard) blending modes.
 */
typedef enum {
    GFX2D_SPECIAL_BLEND_LIGHTEN,
    GFX2D_SPECIAL_BLEND_DARKEN,
    GFX2D_SPECIAL_BLEND_MULTIPLY,
    GFX2D_SPECIAL_BLEND_AVERAGE,
    GFX2D_SPECIAL_BLEND_ADD,
    GFX2D_SPECIAL_BLEND_SUBTRACT,
    GFX2D_SPECIAL_BLEND_DIFFERENCE,
    GFX2D_SPECIAL_BLEND_NEGATION,
    GFX2D_SPECIAL_BLEND_SCREEN,
    GFX2D_SPECIAL_BLEND_OVERLAY,
    GFX2D_SPECIAL_BLEND_DODGE,
    GFX2D_SPECIAL_BLEND_BURN,
    GFX2D_SPECIAL_BLEND_REFLECT,
    GFX2D_SPECIAL_BLEND_GLOW,
} gfx2d_special_blend_t;

/**
 * @brief Raster Operation (ROP) mode.
 */
typedef enum {
    GFX2D_ROP_MODE_ROP2 = 0, // Binary ROP (Destination, Source).
    GFX2D_ROP_MODE_ROP3 = 1, // Ternary ROP (Destination, Source, Pattern).
    GFX2D_ROP_MODE_ROP4 = 2, // Quaternary ROP (Destination, Source, Pattern, Mask).
} gfx2d_rop_mode_t;

/**
 * @brief A rectangular area.
 */
typedef struct {
    int16_t  x;      // Top-left X coordinate.
    int16_t  y;      // Top-left Y coordinate.
    uint16_t width;  // Width in pixels.
    uint16_t height; // Height in pixels.
} gfx2d_rect_t;

/**
 * @brief A 2D surface in memory.
 */
typedef struct {
    void* address;    // Pointer to pixel data.
    uint16_t width;   // Surface width in pixels.
    uint16_t height;  // Surface height in pixels.
    uint32_t stride;  // Length of a line in bytes (pitch).
    gfx2d_pixel_format_t format;  // Pixel format of the surface.
    uint8_t  clut_id; // Color Look-Up Table ID (0 or 1) for indexed formats.
} gfx2d_surface_t;

/**
 * @brief Configuration options for a BLEND operation.
 */
typedef struct {
    gfx2d_blend_factor_t  src_alpha_factor;
    gfx2d_blend_factor_t  dst_alpha_factor;
    gfx2d_blend_factor_t  src_color_factor;
    gfx2d_blend_factor_t  dst_color_factor;
    gfx2d_blend_func_t    blend_func;
    gfx2d_special_blend_t special_blend_mode;
    bool                  premult_src_alpha;   // Enable source premultiplied alpha.
    bool                  premult_dst_alpha;   // Enable destination premultiplied alpha.
} gfx2d_blend_options_t;

/**
 * @brief Configuration options for a ROP operation.
 */
typedef struct {
    gfx2d_rop_mode_t mode;   // ROP mode (ROP2, ROP3, or ROP4).
    uint8_t          rop_lo; // Lower 8 bits of the ROP code.
    uint8_t          rop_hi; // Upper 8 bits of the ROP code (for ROP4).
} gfx2d_rop_options_t;

/**
 * @brief Callback function pointer for completion notification.
 * @note This is called from an interrupt handler.
 */
typedef void (*gfx2d_callback_t)(void);


// Public Function Prototypes

/**
 * @brief Initializes the GFX2D peripheral and driver. Must be called first.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_init(void);

/**
 * @brief De-initializes the GFX2D peripheral.
 * @note Waits for pending operations to complete before disabling the hardware.
 */
void gfx2d_deinit(void);

/**
 * @brief Commits all queued commands to the hardware for execution.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_commit(void);

/**
 * @brief Waits until the GFX2D has processed all commands.
 * @return GFX2D_SUCCESS if idle, GFX2D_ERROR_BUSY on timeout.
 */
gfx2d_error_t gfx2d_wait_for_busy(void);

/**
 * @brief Checks if the GFX2D peripheral is currently busy.
 * @return True if busy, false if idle.
 */
bool gfx2d_is_busy(void);

/**
 * @brief Registers a callback function to be called upon command completion.
 * @param callback The function to call. Use NULL to unregister.
 */
void gfx2d_register_completion_callback(gfx2d_callback_t callback);

/**
 * @brief Interrupt Service Routine for the GFX2D peripheral.
 * @note This should be called from the main GFX2D interrupt vector.
 */
void gfx2d_isr_handler(void);

/**
 * @brief Queues a command to fill a rectangular area with a solid color.
 * @param dest_surface The destination surface.
 * @param dest_rect    The rectangular area to fill.
 * @param color        The 32-bit ARGB8888 color to use.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_fill(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect, uint32_t color);

/**
 * @brief Queues a command to copy a rectangular area from one surface to another.
 * @param dest_surface The destination surface.
 * @param dest_rect    The rectangular area to copy to.
 * @param src_surface  The source surface.
 * @param src_rect     The rectangular area to copy from.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_copy(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                         gfx2d_surface_t* src_surface, const gfx2d_rect_t* src_rect);

/**
 * @brief Queues a command to blend two source surfaces onto a destination surface.
 * @param dest_surface The destination surface (Surface 0).
 * @param dest_rect    The rectangular area on the destination.
 * @param src0_surface The first source surface (Surface 1).
 * @param src0_rect    The rectangular area on the first source.
 * @param src1_surface The second source surface (Surface 2).
 * @param src1_rect    The rectangular area on the second source.
 * @param options      Pointer to the blending options.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_blend(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                          gfx2d_surface_t* src0_surface, const gfx2d_rect_t* src0_rect,
                          gfx2d_surface_t* src1_surface, const gfx2d_rect_t* src1_rect,
                          const gfx2d_blend_options_t* options);

/**
 * @brief Queues a command to perform a raster operation (ROP).
 * @param dest_surface    The destination surface (D, Surface 0).
 * @param dest_rect       The rectangular area on the destination.
 * @param src_surface     The source surface (S, Surface 1).
 * @param src_rect        The rectangular area on the source.
 * @param pattern_surface The pattern surface (P, Surface 2).
 * @param pattern_rect    The rectangular area on the pattern.
 * @param mask_surface    The mask surface (M, Surface 3), only used for ROP4.
 * @param options         Pointer to the ROP options.
 * @return GFX2D_SUCCESS on success, otherwise an error code.
 */
gfx2d_error_t gfx2d_rop(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                        gfx2d_surface_t* src_surface, const gfx2d_rect_t* src_rect,
                        gfx2d_surface_t* pattern_surface, const gfx2d_rect_t* pattern_rect,
                        gfx2d_surface_t* mask_surface,
                        const gfx2d_rop_options_t* options);

#endif /* GFX2D_H_ */
