/**
 * @file gfx2d.c
 * @brief Implementation of the Microchip SAM9X7 2D Graphics Engine (GFX2D) Driver.
 * @version 1.0
 */

#include "gfx2d.h" // Public driver API

// Instruction Opcodes
#define OPCODE_LDR   0x8
#define OPCODE_FILL  0xB
#define OPCODE_COPY  0xC
#define OPCODE_BLEND 0xD
#define OPCODE_ROP   0xE

// Instruction Word Bitfield Positions
#define GFX2D_INSTR_OPCODE_Pos 28
#define GFX2D_INSTR_IE_Pos     24
#define GFX2D_INSTR_REG_Pos    16
#define GFX2D_INSTR_ARGS_Pos   0

// GFX2D Register Indices (for LDR instruction)
#define GFX2D_REG_PA0      0
#define GFX2D_REG_PITCH0   1
#define GFX2D_REG_CFG0     2
#define GFX2D_REG_PA1      3
#define GFX2D_REG_PITCH1   4
#define GFX2D_REG_CFG1     5
#define GFX2D_REG_PA2      6
#define GFX2D_REG_PITCH2   7
#define GFX2D_REG_CFG2     8
#define GFX2D_REG_PA3      9
#define GFX2D_REG_PITCH3   10
#define GFX2D_REG_CFG3     11

// Driver State
static uint8_t __attribute__ ((aligned (256))) g_ring_buffer_mem[GFX2D_COMMAND_BUFFER_SIZE_BYTES];
static uint32_t* const g_ring_buffer_base = (uint32_t*)g_ring_buffer_mem;

static uint32_t         g_ring_buffer_size_words = 0;
static uint32_t         g_ring_buffer_head = 0;    // Tracks the software (CPU) end of the buffer.
static uint32_t         g_last_instr_offset = 0;   // Word offset of the last command's header.

static bool             g_is_initialized = false;
static volatile bool    g_is_processing = false;   // Flag indicating hardware is busy.
static gfx2d_callback_t g_completion_callback = NULL;

// Internal Helper Functions

/** @brief Calculates the number of free 32-bit words in the ring buffer. */
static uint32_t _gfx2d_get_free_space(void) {
    uint32_t head = g_ring_buffer_head;
    uint32_t tail = GFX2D_REGS->GFX2D_TAIL; // Hardware's read position.

    if (head >= tail) {
        return g_ring_buffer_size_words - (head - tail) - 1;
    } else {
        return tail - head - 1;
    }
}

/** @brief Queues a sequence of words into the ring buffer. */
static void _gfx2d_queue_words(const uint32_t* words, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        g_ring_buffer_base[g_ring_buffer_head] = words[i];
        g_ring_buffer_head = (g_ring_buffer_head + 1) % g_ring_buffer_size_words;
    }
}

// Public Function Implementations

gfx2d_error_t gfx2d_init(void) {
    // Wait for any previous activity to finish and disable the engine.
    while ((GFX2D_REGS->GFX2D_GS & GFX2D_GS_BUSY_Msk));
    GFX2D_REGS->GFX2D_GD = GFX2D_GD_DISABLE_Msk;

    g_ring_buffer_size_words = GFX2D_COMMAND_BUFFER_SIZE_BYTES / sizeof(uint32_t);
    g_ring_buffer_head = 0;
    g_last_instr_offset = 0;

    // Configure the hardware with the ring buffer's location and size.
    GFX2D_REGS->GFX2D_BASE = (uint32_t)g_ring_buffer_base;
    GFX2D_REGS->GFX2D_LEN = GFX2D_LEN_LEN((GFX2D_COMMAND_BUFFER_SIZE_BYTES / 256) - 1);
    GFX2D_REGS->GFX2D_HEAD = GFX2D_HEAD_HEAD(0);
    GFX2D_REGS->GFX2D_TAIL = GFX2D_TAIL_TAIL(0);

    // Enable the engine.
    GFX2D_REGS->GFX2D_GE = GFX2D_GE_ENABLE_Msk;

    g_is_initialized = true;
    g_is_processing = false;
    g_completion_callback = NULL;

    return GFX2D_SUCCESS;
}

void gfx2d_deinit(void) {
    if (!g_is_initialized) return;
    gfx2d_wait_for_busy();
    GFX2D_REGS->GFX2D_GD = GFX2D_GD_DISABLE_Msk;
    g_is_initialized = false;
}

gfx2d_error_t gfx2d_commit(void) {
    if (!g_is_initialized) return GFX2D_ERROR_NOT_INITIALIZED;
    if (g_is_processing) return GFX2D_ERROR_BUSY;
    if (g_ring_buffer_head == GFX2D_REGS->GFX2D_TAIL) return GFX2D_SUCCESS; // Nothing to commit.

    g_is_processing = true;

    // Set the Interrupt Enable (IE) bit on the very last instruction.
    // This ensures we get a single interrupt when the whole batch is done.
    g_ring_buffer_base[g_last_instr_offset] |= (1u << GFX2D_INSTR_IE_Pos);

    // Ensure GFX2D sees the new commands by cleaning the CPU data cache.
    GFX2D_CLEAN_DCACHE_BY_ADDR((void*)g_ring_buffer_base, GFX2D_COMMAND_BUFFER_SIZE_BYTES);

    // Enable the "End of Execution" interrupt.
    GFX2D_REGS->GFX2D_IE = GFX2D_IE_EXEND_Msk;

    // Update the head pointer, which starts the hardware processing.
    GFX2D_REGS->GFX2D_HEAD = GFX2D_HEAD_HEAD(g_ring_buffer_head);

    return GFX2D_SUCCESS;
}

void gfx2d_isr_handler(void) {
    // Check if the "End of Execution" interrupt occurred.
    if ((GFX2D_REGS->GFX2D_IS & GFX2D_IS_EXEND_Msk) == 0) return;

    // Acknowledge and disable the interrupt to prevent re-entry.
    GFX2D_REGS->GFX2D_ID = GFX2D_ID_EXEND_Msk;

    // Notify the application via callback, if one is registered.
    if (g_completion_callback) {
        g_completion_callback();
    }

    g_is_processing = false;
}

gfx2d_error_t gfx2d_wait_for_busy(void) {
    volatile uint32_t timeout = 0xFFFFFF;
    while (g_is_processing && (timeout-- > 0));

    // As a safeguard, also check the hardware's busy bit.
    while ((GFX2D_REGS->GFX2D_GS & GFX2D_GS_BUSY_Msk) && (timeout-- > 0));

    return (timeout > 0) ? GFX2D_SUCCESS : GFX2D_ERROR_BUSY;
}

bool gfx2d_is_busy(void) {
    // The driver is considered busy if the hardware is running or if an interrupt is pending.
    return (GFX2D_REGS->GFX2D_GS & GFX2D_GS_BUSY_Msk) || g_is_processing;
}

void gfx2d_register_completion_callback(gfx2d_callback_t callback) {
    g_completion_callback = callback;
}

gfx2d_error_t gfx2d_fill(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect, uint32_t color) {
    if (!g_is_initialized) return GFX2D_ERROR_NOT_INITIALIZED;
    if (dest_surface == NULL || dest_rect == NULL || dest_surface->address == NULL) return GFX2D_ERROR_INVALID_ARG;
    if (dest_rect->width == 0 || dest_rect->height == 0 || dest_surface->stride == 0) return GFX2D_ERROR_INVALID_ARG;

    const uint32_t cmd_size = 10; // 3 LDRs (6 words) + 1 FILL (4 words)
    if (_gfx2d_get_free_space() < cmd_size) {
        return GFX2D_ERROR_BUFFER_FULL;
    }

    uint32_t cmd_group[10];
    uint32_t cmd_idx = 0;

    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)dest_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = dest_surface->stride;

    // Track this instruction's header offset. It will be the last one before the FILL command.
    g_last_instr_offset = (g_ring_buffer_head + cmd_idx) % g_ring_buffer_size_words;

    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(dest_surface->format) | GFX2D_CFG_IDXCX(dest_surface->clut_id);

    // FILL Instruction
    cmd_group[cmd_idx++] = (OPCODE_FILL << GFX2D_INSTR_OPCODE_Pos) | (2 << GFX2D_INSTR_ARGS_Pos);
    cmd_group[cmd_idx++] = ((uint32_t)(dest_rect->height - 1) << 16) | (uint32_t)(dest_rect->width - 1);
    cmd_group[cmd_idx++] = ((uint32_t)dest_rect->y << 16) | (uint32_t)dest_rect->x;
    cmd_group[cmd_idx++] = color;

    _gfx2d_queue_words(cmd_group, cmd_idx);
    return GFX2D_SUCCESS;
}

gfx2d_error_t gfx2d_copy(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                         gfx2d_surface_t* src_surface, const gfx2d_rect_t* src_rect) {
    if (!g_is_initialized) return GFX2D_ERROR_NOT_INITIALIZED;
    if (dest_surface == NULL || dest_rect == NULL || dest_surface->address == NULL ||
        src_surface == NULL || src_rect == NULL || src_surface->address == NULL) return GFX2D_ERROR_INVALID_ARG;
    if (dest_rect->width == 0 || dest_rect->height == 0 || dest_surface->stride == 0 || src_surface->stride == 0) return GFX2D_ERROR_INVALID_ARG;

    const uint32_t cmd_size = 16; // 6 LDRs (12 words) + 1 COPY (4 words)
    if (_gfx2d_get_free_space() < cmd_size) {
        return GFX2D_ERROR_BUFFER_FULL;
    }

    uint32_t cmd_group[16];
    uint32_t cmd_idx = 0;

    // Destination Surface (0)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)dest_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = dest_surface->stride;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(dest_surface->format) | GFX2D_CFG_IDXCX(dest_surface->clut_id);

    // Source Surface (1)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)src_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = src_surface->stride;

    // Track this instruction's header offset. It will be the last one before the COPY command.
    g_last_instr_offset = (g_ring_buffer_head + cmd_idx) % g_ring_buffer_size_words;

    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(src_surface->format) | GFX2D_CFG_IDXCX(src_surface->clut_id);

    // COPY Instruction
    cmd_group[cmd_idx++] = (OPCODE_COPY << GFX2D_INSTR_OPCODE_Pos) | (2 << GFX2D_INSTR_ARGS_Pos);
    cmd_group[cmd_idx++] = ((uint32_t)(dest_rect->height - 1) << 16) | (uint32_t)(dest_rect->width - 1);
    cmd_group[cmd_idx++] = ((uint32_t)dest_rect->y << 16) | (uint32_t)dest_rect->x;
    cmd_group[cmd_idx++] = ((uint32_t)src_rect->y << 16) | (uint32_t)src_rect->x;

    _gfx2d_queue_words(cmd_group, cmd_idx);
    return GFX2D_SUCCESS;
}

gfx2d_error_t gfx2d_blend(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                          gfx2d_surface_t* src0_surface, const gfx2d_rect_t* src0_rect,
                          gfx2d_surface_t* src1_surface, const gfx2d_rect_t* src1_rect,
                          const gfx2d_blend_options_t* options) {
    if (!g_is_initialized) return GFX2D_ERROR_NOT_INITIALIZED;
    if (dest_surface == NULL || dest_rect == NULL || dest_surface->address == NULL ||
        src0_surface == NULL || src0_rect == NULL || src0_surface->address == NULL ||
        src1_surface == NULL || src1_rect == NULL || src1_surface->address == NULL ||
        options == NULL) return GFX2D_ERROR_INVALID_ARG;
    if (dest_rect->width == 0 || dest_rect->height == 0 || dest_surface->stride == 0 ||
        src0_surface->stride == 0 || src1_surface->stride == 0) return GFX2D_ERROR_INVALID_ARG;

    const uint32_t cmd_size = 24; // 9 LDRs (18 words) + 1 BLEND (6 words)
    if (_gfx2d_get_free_space() < cmd_size) {
        return GFX2D_ERROR_BUFFER_FULL;
    }

    uint32_t cmd_group[24];
    uint32_t cmd_idx = 0;

    // Surface 0 (Destination)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)dest_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = dest_surface->stride;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(dest_surface->format) | GFX2D_CFG_IDXCX(dest_surface->clut_id);

    // Surface 1 (Source 0)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)src0_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = src0_surface->stride;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(src0_surface->format) | GFX2D_CFG_IDXCX(src0_surface->clut_id);

    // Surface 2 (Source 1)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)src1_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = src1_surface->stride;

    // Track this instruction's header offset. It will be the last one before the BLEND command.
    g_last_instr_offset = (g_ring_buffer_head + cmd_idx) % g_ring_buffer_size_words;

    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(src1_surface->format) | GFX2D_CFG_IDXCX(src1_surface->clut_id);

    // BLEND Instruction
    cmd_group[cmd_idx++] = (OPCODE_BLEND << GFX2D_INSTR_OPCODE_Pos) | (4 << GFX2D_INSTR_ARGS_Pos);
    cmd_group[cmd_idx++] = ((uint32_t)(dest_rect->height - 1) << 16) | (uint32_t)(dest_rect->width - 1);
    cmd_group[cmd_idx++] = ((uint32_t)dest_rect->y << 16) | (uint32_t)dest_rect->x;
    cmd_group[cmd_idx++] = ((uint32_t)src0_rect->y << 16) | (uint32_t)src0_rect->x;
    cmd_group[cmd_idx++] = ((uint32_t)src1_rect->y << 16) | (uint32_t)src1_rect->x;
    cmd_group[cmd_idx++] = ((options->premult_dst_alpha ? 1u : 0) << 23) |
                           ((uint32_t)options->dst_alpha_factor << 20) |
                           ((options->premult_src_alpha ? 1u : 0) << 19) |
                           ((uint32_t)options->src_alpha_factor << 16) |
                           ((uint32_t)options->special_blend_mode << 12) |
                           ((uint32_t)options->blend_func << 8) |
                           ((uint32_t)options->dst_color_factor << 4) |
                           ((uint32_t)options->src_color_factor);

    _gfx2d_queue_words(cmd_group, cmd_idx);
    return GFX2D_SUCCESS;
}

gfx2d_error_t gfx2d_rop(gfx2d_surface_t* dest_surface, const gfx2d_rect_t* dest_rect,
                        gfx2d_surface_t* src_surface, const gfx2d_rect_t* src_rect,
                        gfx2d_surface_t* pattern_surface, const gfx2d_rect_t* pattern_rect,
                        gfx2d_surface_t* mask_surface,
                        const gfx2d_rop_options_t* options) {
    if (!g_is_initialized) return GFX2D_ERROR_NOT_INITIALIZED;
    if (dest_surface == NULL || dest_rect == NULL || dest_surface->address == NULL ||
        src_surface == NULL || src_rect == NULL || src_surface->address == NULL ||
        pattern_surface == NULL || pattern_rect == NULL || pattern_surface->address == NULL ||
        options == NULL) return GFX2D_ERROR_INVALID_ARG;
    if (options->mode == GFX2D_ROP_MODE_ROP4 && (mask_surface == NULL || mask_surface->address == NULL)) return GFX2D_ERROR_INVALID_ARG;
    if (dest_rect->width == 0 || dest_rect->height == 0 || dest_surface->stride == 0 || src_surface->stride == 0 ||
        pattern_surface->stride == 0) return GFX2D_ERROR_INVALID_ARG;

    uint32_t cmd_size = (options->mode == GFX2D_ROP_MODE_ROP4) ? 31 : 25;
    if (_gfx2d_get_free_space() < cmd_size) {
        return GFX2D_ERROR_BUFFER_FULL;
    }

    uint32_t cmd_group[31];
    uint32_t cmd_idx = 0;

    // Surface 0 (Destination)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)dest_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = dest_surface->stride;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG0 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(dest_surface->format) | GFX2D_CFG_IDXCX(dest_surface->clut_id);

    // Surface 1 (Source)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)src_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = src_surface->stride;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG1 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(src_surface->format) | GFX2D_CFG_IDXCX(src_surface->clut_id);

    // Surface 2 (Pattern)
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = (uint32_t)pattern_surface->address;
    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = pattern_surface->stride;

    // Track this instruction's header offset. It will be the last one before the ROP command.
    g_last_instr_offset = (g_ring_buffer_head + cmd_idx) % g_ring_buffer_size_words;

    cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG2 << GFX2D_INSTR_REG_Pos);
    cmd_group[cmd_idx++] = GFX2D_CFG_PF(pattern_surface->format) | GFX2D_CFG_IDXCX(pattern_surface->clut_id);

    // Surface 3 (Mask), only for ROP4
    if (options->mode == GFX2D_ROP_MODE_ROP4) {
        cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PA3 << GFX2D_INSTR_REG_Pos);
        cmd_group[cmd_idx++] = (uint32_t)mask_surface->address;
        cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_PITCH3 << GFX2D_INSTR_REG_Pos);
        cmd_group[cmd_idx++] = mask_surface->stride;

        // This is now the last LDR, so update the offset to point to its header.
        g_last_instr_offset = (g_ring_buffer_head + cmd_idx) % g_ring_buffer_size_words;

        cmd_group[cmd_idx++] = (OPCODE_LDR << GFX2D_INSTR_OPCODE_Pos) | (GFX2D_REG_CFG3 << GFX2D_INSTR_REG_Pos);
        cmd_group[cmd_idx++] = GFX2D_CFG_PF(mask_surface->format) | GFX2D_CFG_IDXCX(mask_surface->clut_id);
    }

    // ROP Instruction
    cmd_group[cmd_idx++] = (OPCODE_ROP << GFX2D_INSTR_OPCODE_Pos) | (5 << GFX2D_INSTR_ARGS_Pos);
    cmd_group[cmd_idx++] = ((uint32_t)(dest_rect->height - 1) << 16) | (uint32_t)(dest_rect->width - 1);
    cmd_group[cmd_idx++] = ((uint32_t)dest_rect->y << 16) | (uint32_t)dest_rect->x;
    cmd_group[cmd_idx++] = ((uint32_t)src_rect->y << 16) | (uint32_t)src_rect->x;
    cmd_group[cmd_idx++] = ((uint32_t)pattern_rect->y << 16) | (uint32_t)pattern_rect->x;
    cmd_group[cmd_idx++] = (options->mode == GFX2D_ROP_MODE_ROP4) ? (uint32_t)mask_surface->address : 0;
    cmd_group[cmd_idx++] = ((uint32_t)options->mode << 16) |
                           ((uint32_t)options->rop_hi << 8) |
                           ((uint32_t)options->rop_lo);

    _gfx2d_queue_words(cmd_group, cmd_idx);
    return GFX2D_SUCCESS;
}
