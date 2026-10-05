#include "arm64_inst/utils.h"
#include "patchs/core.h"
#include <stdlib.h>

int32_t patch_abl_gbl(char* buffer, int32_t size) {
    char target[]      = { 'e',0, 'f',0, 'i',0, 's',0, 'p',0 };
    char replacement[] = { 'n',0, 'u',0, 'l',0, 'l',0, 's',0 };
    int32_t target_len = sizeof(target);
    for (int32_t i = 0; i < size - target_len; ++i) {
        if (memcmp(buffer + i, target, target_len) == 0) {
            memcpy(buffer + i, replacement, target_len);
            return 0;
        }
    }
    return -1;
}

int16_t Original[] = {
    -1, 0x00, 0x00, 0x34, 0x28, 0x00, 0x80, 0x52,
    0x06, 0x00, 0x00, 0x14, 0xE8, -1, 0x40, 0xF9,
    0x08, 0x01, 0x40, 0x39, 0x1F, 0x01, 0x00, 0x71,
    0xE8, 0x07, 0x9F, 0x1A, 0x08, 0x79, 0x1F, 0x53
};
int16_t Patched[] = {
    -1, -1, -1, -1, 0x08, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

int32_t patch_abl_bootstate(char* buffer, int32_t size,
                          int8_t* lock_register_num, int32_t* offset) {
    int32_t pattern_len = sizeof(Original) / sizeof(int16_t);
    int32_t patched_count = 0;
    if (size < pattern_len) return 0;
    for (int32_t i = 0; i <= size - pattern_len; ++i) {
        bool match = true;
        for (int32_t j = 0; j < pattern_len; ++j) {
            if (Original[j] != -1 && (uint8_t)buffer[i + j] != (uint8_t)Original[j]) {
                match = false; break;
            }
        }
        if (match) {
            *lock_register_num = (int8_t)((uint8_t)buffer[i] & 0x1F);
            *offset = i;
            #ifndef DISABLE_PATCH_3
            for (int32_t j = 0; j < pattern_len; ++j)
                if (Patched[j] != -1) buffer[i + j] = (char)Patched[j];
            #endif
            patched_count++;
            i += pattern_len - 1;
        }
    }
    return patched_count;
}
// track_forward_patch_strb callback example: patch STRB to STR, so we can use the same register for 64-bit value instead of 8-bit, which is more likely to be used in real code and easier to track back to source.
int32_t patch_strb_to_str_forward_callback(char* buffer, int32_t size, int32_t off, DecodedInst d, int32_t anchor_offset) {
    if (d.type == INST_STRB_IMM || d.type == INST_STRB_POST || d.type == INST_STRB_PRE) {
        if (off > anchor_offset) {
            StrbInfo si = decode_any_strb(d.raw);
            if (si.rn == 31) {
                printf("  0x%X: STRB W%d,[SP,#0x%X] ** SINK (after anchor0x%X) **\n",
                    off, si.rt, si.imm, anchor_offset);
            } else {
                printf("  0x%X: STRB W%d,[X%d,#0x%X] ** SINK (after anchor0x%X) **\n",
                    off, si.rt, si.rn, si.imm, anchor_offset);
            }
            printf("  Before: %02X %02X %02X %02X\n",
                       (uint8_t)buffer[off], (uint8_t)buffer[off+1],
                       (uint8_t)buffer[off+2], (uint8_t)buffer[off+3]);
            write_instr(buffer, off, strb_with_reg(d.raw, 31));

            printf("  After : %02X %02X %02X %02X (Rt -> WZR)\n",
                       (uint8_t)buffer[off], (uint8_t)buffer[off+1],
                       (uint8_t)buffer[off+2], (uint8_t)buffer[off+3]);
            return SUCCESS;
        }
    }
    return NEED_MORE;
}
static int32_t track_forward_patch_strb(char* buffer, int32_t size, int32_t ldrb_off,
                                      int8_t src_reg, int32_t anchor_off) {
    return track_forward(buffer, size, ldrb_off, src_reg, anchor_off, patch_strb_to_str_forward_callback);
}
//
int32_t source_callback(char* buffer, int32_t size, int32_t now_offset, int8_t current_target, int32_t anchor_offset) {
    printf("  Before: %02X %02X %02X %02X\n",
        (uint8_t)buffer[now_offset], (uint8_t)buffer[now_offset+1],
        (uint8_t)buffer[now_offset+2], (uint8_t)buffer[now_offset+3]);
    #ifndef DISABLE_PATCH_4
    write_instr(buffer, now_offset, encode_movz_w((uint8_t)current_target, 1));
    printf("  After : %02X %02X %02X %02X (MOV W%d, #1)\n",
        (uint8_t)buffer[now_offset], (uint8_t)buffer[now_offset+1],
        (uint8_t)buffer[now_offset+2], (uint8_t)buffer[now_offset+3],
        (int)current_target);
    #endif
    #ifndef DISABLE_PATCH_5
    int32_t fwd = track_forward_patch_strb(buffer, size, now_offset, current_target, anchor_offset);
    if (fwd != SUCCESS) {
        printf("Warning: sink STRB not found after anchor 0x%X\n", anchor_offset);
        return -1;
    }
    printf("Sink patched successfully.\n");
    #endif
    return 0;
}

int32_t patch_adrl_unlocked_to_locked(char* buffer, int32_t size, uint64_t load_base) {
    if (size < 24) return 0;
    int32_t patched = 0;

    for (int32_t i = 0; i <= size - 24; i += 4) {
        DecodedInst a0 = decode_at(buffer, i);
        DecodedInst a1 = decode_at(buffer, i + 4);
        DecodedInst b0 = decode_at(buffer, i + 8);
        DecodedInst b1 = decode_at(buffer, i + 12);

        if (a0.type != INST_ADRP || a1.type != INST_ADD_X_IMM) continue;
        if (a1.rt != a0.rt || a1.rn != a0.rt) continue;

        if (b0.type != INST_ADRP || b1.type != INST_ADD_X_IMM) continue;
        if (b1.rt != b0.rt || b1.rn != b0.rt) continue;


        uint8_t xa = a0.rt, xb = b0.rt;
        if (xa == xb) continue;

        int64_t off0 = calc_adrl_file_offset(buffer, i,      load_base);
        int64_t off1 = calc_adrl_file_offset(buffer, i + 8,  load_base);

        if (!str_at(buffer, size, off0, "unlocked")) continue;
        if (!str_at(buffer, size, off1, "locked"))   continue;
        bool match = false;
        for(int j=i+16; j<=i+40;j+=4){
            if (j + 7 >= size) break;
            DecodedInst c0 = decode_at(buffer, j);
            DecodedInst c1 = decode_at(buffer, j + 4);
            if(c0.type == INST_ADRP && c1.type == INST_ADD_X_IMM){
                int64_t offc = calc_adrl_file_offset(buffer, j, load_base);
                if(str_at(buffer, size, offc, "androidboot.vbmeta.device_state")){
                    match = true;
                    break;
                }
            }
        }
        if (!match) continue;
        printf("Found ADRL triple at 0x%X:\n", i);
        printf("  [0x%X] ADRP+ADD X%d -> file:0x%llX \"unlocked\"\n",
               i, xa, (unsigned long long)off0);
        printf("  [0x%X] ADRP+ADD X%d -> file:0x%llX \"locked\"\n",
               i+8, xb, (unsigned long long)off1);

        uint32_t new_adrp = adrp_with_rd(b0.raw, xa);
        uint32_t new_add  = add_with_reg(b1.raw, xa);

        printf("  Patch pair-0: ADRP %08X->%08X, ADD %08X->%08X\n",
               a0.raw, new_adrp, a1.raw, new_add);

        write_instr(buffer, i,     new_adrp);
        write_instr(buffer, i + 4, new_add);

        patched++;
        i += 20;
    }

    if (patched == 0)
        printf("ADRL triple not found\n");
    else
        printf("ADRL patch applied: %d location(s)\n", patched);

    return patched;
}

#include "patchs/oplus/warning.h"
#include "patchs/oplus/forceenablefastboot.h"
bool PatchBuffer(char* data, int32_t size) {
    if (patch_abl_gbl(data, size) != 0)
        printf("Warning: Failed to patch ABL GBL\n");

    int32_t patched_adrl = patch_adrl_unlocked_to_locked(data, size, 0);
    if (patched_adrl == 0){
        printf("Warning: ADRL triple not found, skipping\n");
        // not critical, continue with other patches
    }

    if(patched_adrl > 1){
        printf("Warning: Multiple ADRL triples patched (%d), verify if all are correct\n", patched_adrl);
        return false; //cr
    }
    int32_t offset = -1;
    int8_t lock_register_num = -1;
    int32_t num_patches = patch_abl_bootstate(data, size, &lock_register_num, &offset);
    if (num_patches == 0) {
        printf("Error: Failed to find/patch ABL Boot State\n");
        return 0;
    }
    printf("Anchor offset : 0x%X\n", offset);
    printf("Lock register : W%d\n", (int)lock_register_num);
    printf("Boot patches: %d\n", num_patches);

    int32_t global_var_offset = -1;
    if (find_ldrB_instructio_reverse(data, size, offset, lock_register_num, &global_var_offset, source_callback) != 0) {
        printf("Warning: Failed to patch LDRB->STRB chain for W%d\n",
               (int)lock_register_num);
    }
    printf("Global variable offset (for warning patch): 0x%X\n", global_var_offset);
    // ===================== 启用去黄字补丁 =====================


    //oplus
    if (!patch_warning(data, size, global_var_offset)) {
        printf("OPlus Warning: patch_warning failed\n");
    }

    //force enable fastboot for unofficially unlock
    if (!patch_fastboot(data, size, global_var_offset)) {
        printf("OPlus Warning: patch_fastboot failed\n");
    }
    // ==========================================================

    return 1;
}
