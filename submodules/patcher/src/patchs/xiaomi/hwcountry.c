/* HwCountry redirection adapted from StevenWin818/GBL_Root_HyperCanoe. */
#include "arm64_inst/utils.h"
#include "patchs/xiaomi/hwcountry.h"

static int32_t find_cstring(const char* buffer, int32_t size, const char* needle) {
    int32_t length = (int32_t)strlen(needle) + 1;
    for (int32_t i = 0; i <= size - length; ++i)
        if (memcmp(buffer + i, needle, length) == 0) return i;
    return -1;
}

static int32_t find_reference(const char* buffer, int32_t size, int64_t target) {
    int32_t result = -1;
    for (int32_t i = 0; i <= size - 8; i += 4) {
        if (calc_adrl_file_offset(buffer, i, 0) != target) continue;
        if (result >= 0) {
            printf("HwCountry: ambiguous ADRP+ADD references to 0x%llX\n",
                   (unsigned long long)target);
            return -1;
        }
        result = i;
    }
    return result;
}

static uint16_t read_u16(const char* buffer, int32_t off) {
    uint16_t value;
    memcpy(&value, buffer + off, sizeof(value));
    return value;
}

static bool in_range(int64_t target, int32_t start, int32_t length) {
    return target >= start && target < (int64_t)start + length;
}

static bool slack_is_referenced(const char* buffer, int32_t size,
                                int32_t start, int32_t length) {
    for (int32_t i = 0; i <= size - 8; i += 4) {
        if (in_range(calc_adrl_file_offset(buffer, i, 0), start, length)) return true;
        uint32_t raw = read_instr(buffer, i);
        if ((raw & 0x9F000000u) == 0x10000000u) { /* ADR */
            int32_t imm = ((raw >> 29) & 3) | (((raw >> 5) & 0x7FFFF) << 2);
            if (imm & 0x100000) imm -= 0x200000;
            if (in_range((int64_t)i + imm, start, length)) return true;
        }
        if ((raw & 0x3B000000u) == 0x18000000u) { /* LDR literal */
            if (in_range((int64_t)i + decode_imm19(raw), start, length)) return true;
        }
        if ((raw & 0x7E000000u) == 0x36000000u) { /* TBZ/TBNZ */
            int32_t imm = (raw >> 5) & 0x3FFF;
            if (imm & 0x2000) imm -= 0x4000;
            if (in_range((int64_t)i + imm * 4, start, length)) return true;
        }
        DecodedInst d = decode_at(buffer, i);
        int64_t target;
        if (get_JUMP_target(&d, i, &target) && in_range(target, start, length)) return true;
    }
    return false;
}

static int32_t find_slack(const char* buffer, int32_t size, int32_t need) {
    /* Only use mapped, readable .text tail padding in a flat ARM64 PE. */
    if (size < 64 || memcmp(buffer, "MZ", 2) != 0) return -1;
    uint32_t pe = read_instr(buffer, 0x3C);
    if (pe > (uint32_t)size - 24 || memcmp(buffer + pe, "PE\0\0", 4) != 0
        || read_u16(buffer, pe + 4) != 0xAA64) return -1;
    uint16_t count = read_u16(buffer, pe + 6);
    uint16_t optional_size = read_u16(buffer, pe + 20);
    int64_t sections = (int64_t)pe + 24 + optional_size;
    if (count == 0 || optional_size < 160 || sections + (int64_t)count * 40 > size
        || read_u16(buffer, pe + 24) != 0x20B
        || read_instr(buffer, pe + 48) != 0 || read_instr(buffer, pe + 52) != 0)
        return -1;

    int32_t slack = -1;
    for (int32_t i = 0; i < count; ++i) {
        int32_t section = (int32_t)sections + i * 40;
        uint32_t virtual_size = read_instr(buffer, section + 8);
        uint32_t rva = read_instr(buffer, section + 12);
        uint32_t raw_size = read_instr(buffer, section + 16);
        uint32_t raw = read_instr(buffer, section + 20);
        if (raw_size == 0) continue;
        if (raw != rva || (int64_t)raw + raw_size > size) return -1;
        if (memcmp(buffer + section, ".text\0\0\0", 8) != 0) continue;
        uint32_t mapped_size = virtual_size < raw_size ? virtual_size : raw_size;
        if (slack >= 0 || mapped_size < (uint32_t)need
            || !(read_instr(buffer, section + 36) & 0x40000000u)) return -1;
        slack = (int32_t)(raw + mapped_size - need);
    }
    if (slack < 0) return -1;
    for (int32_t i = 0; i < need; ++i)
        if (buffer[slack + i] != 0) return -1;
    if (slack_is_referenced(buffer, size, slack, need)) return -1;

    /* Reject padding which the PE loader would rewrite through relocations. */
    uint32_t directory_count = read_instr(buffer, pe + 24 + 108);
    if (directory_count > 5) {
        uint32_t reloc = read_instr(buffer, pe + 24 + 152);
        uint32_t reloc_size = read_instr(buffer, pe + 24 + 156);
        int64_t end = (int64_t)reloc + reloc_size;
        if (end > size) return -1;
        for (int64_t off = reloc; off < end;) {
            if (end - off < 8) return -1;
            uint32_t page = read_instr(buffer, (int32_t)off);
            uint32_t block_size = read_instr(buffer, (int32_t)off + 4);
            if (block_size < 8 || (block_size & 1) || off + block_size > end) return -1;
            for (int64_t entry = off + 8; entry < off + block_size; entry += 2) {
                uint16_t relocation = read_u16(buffer, (int32_t)entry);
                if ((relocation >> 12) == 0) continue;
                int64_t target = (int64_t)page + (relocation & 0xFFF);
                if (target < (int64_t)slack + need && target + 8 > slack) return -1;
            }
            off += block_size;
        }
    }
    return slack;
}

static void redirect_reference(char* buffer, int32_t off, int32_t target) {
    DecodedInst pair = decode_at(buffer, off);
    int64_t delta = ((int64_t)(target & ~0xFFF) - (off & ~0xFFF)) / 4096;
    uint32_t imm21 = (uint32_t)delta & 0x1FFFFF;
    uint32_t adrp = 0x90000000u | ((imm21 & 3) << 29)
                  | ((imm21 >> 2) << 5) | pair.rt;
    uint32_t add = 0x91000000u | ((uint32_t)(target & 0xFFF) << 10)
                 | ((uint32_t)pair.rt << 5) | pair.rt;
    printf("HwCountry: redirect ADRP+ADD at 0x%X -> file:0x%X\n", off, target);
    write_instr(buffer, off, adrp);
    write_instr(buffer, off + 4, add);
}

int32_t patch_hwcountry_global(char* buffer, int32_t size) {
    const char value[] = "GLOBAL";
    const char line[] = "HwCountry: GLOBAL";
    if (!buffer || size < 8) return -1;

    int32_t format = find_cstring(buffer, size, "HwCountry: %a");
    if (format < 0) {
        printf("HwCountry: format string not found\n");
        return -1;
    }
    int32_t display = find_reference(buffer, size, format);
    if (display < 0) {
        printf("HwCountry: format reference not found or ambiguous\n");
        return -1;
    }

    /* The format call receives the country buffer in X3. */
    int32_t base_reg = -1;
    uint32_t country_offset = 0;
    for (int32_t i = display; i <= size - 4 && i - display <= 32; i += 4) {
        DecodedInst d = decode_at(buffer, i);
        if (d.type == INST_ADD_X_IMM && d.rt == 3 && d.rn != 31) {
            base_reg = d.rn;
            country_offset = d.imm;
            break;
        }
    }
    if (base_reg < 0) {
        printf("HwCountry: country argument in X3 not found\n");
        return -1;
    }

    int64_t country = -1;
    for (int32_t i = display; i >= 0; i -= 4) {
        DecodedInst d = decode_at(buffer, i);
        if (d.type == INST_PACIASP || d.type == INST_RET) break;
        if (d.type != INST_ADRP || d.rt != base_reg) continue;
        int64_t base = calc_adrl_file_offset(buffer, i, 0);
        if (base >= 0) {
            country = base + country_offset;
            break;
        }
    }
    if (country < 0 || country >= size) {
        printf("HwCountry: country buffer not found in current function\n");
        return -1;
    }
    int32_t getvar = find_reference(buffer, size, country);
    if (getvar < 0 || getvar == display) {
        printf("HwCountry: getvar reference not found or ambiguous\n");
        return -1;
    }

    int32_t slack = find_slack(buffer, size, sizeof(value) + sizeof(line));
    if (slack < 0) {
        printf("HwCountry: no verified ARM64 PE .text tail padding for replacement strings\n");
        return -1;
    }

    /* All sites are validated before changing the buffer. */
    memcpy(buffer + slack, value, sizeof(value));
    memcpy(buffer + slack + sizeof(value), line, sizeof(line));
    redirect_reference(buffer, getvar, slack);
    redirect_reference(buffer, display, slack + sizeof(value));
    printf("HwCountry -> GLOBAL applied\n");
    return 0;
}
