#include "arm64_inst/utils.h"
#include "patchs/xiaomi/hwcountry.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdlib.h>

enum { SIZE = 0x2000, SECTION = 0x188, DISPLAY = 0x310, GETVAR = 0x1300,
       FORMAT = 0x900, COUNTRY = 0xB00, SLACK = 0x1000 - 25 };

static void put16(char* data, int32_t off, uint16_t value) {
    memcpy(data + off, &value, sizeof(value));
}

/* Only the PE header and the instructions used by the patch are populated. */
static void fixture(char* data) {
    memset(data, 0, SIZE);
    memcpy(data, "MZ", 2);
    write_instr(data, 0x3C, 0x80);
    memcpy(data + 0x80, "PE\0\0", 4);
    put16(data, 0x84, 0xAA64);
    put16(data, 0x86, 2);
    put16(data, 0x94, 0xF0);
    put16(data, 0x98, 0x20B);
    memcpy(data + SECTION, ".text", 5);
    write_instr(data, SECTION + 8, 0xE00);
    write_instr(data, SECTION + 12, 0x200);
    write_instr(data, SECTION + 16, 0xE00);
    write_instr(data, SECTION + 20, 0x200);
    write_instr(data, SECTION + 36, 0x60000020);
    memcpy(data + SECTION + 40, ".data", 5);
    write_instr(data, SECTION + 48, 0x1000);
    write_instr(data, SECTION + 52, 0x1000);
    write_instr(data, SECTION + 56, 0x1000);
    write_instr(data, SECTION + 60, 0x1000);
    write_instr(data, SECTION + 76, 0xC0000040);
    memcpy(data + FORMAT, "HwCountry: %a", 14);
    write_instr(data, 0x300, 0xD503233F); /* PACIASP */
    write_instr(data, 0x304, 0x90000014); /* ADRP X20, page 0 */
    write_instr(data, 0x308, 0x91280294); /* ADD X20, X20, #0xA00 */
    write_instr(data, DISPLAY, 0x90000002);
    write_instr(data, DISPLAY + 4, 0x91240042); /* X2 -> format */
    write_instr(data, DISPLAY + 8, 0x91040283); /* ADD X3, X20, #0x100 */
    write_instr(data, GETVAR, 0xF0FFFFE0); /* ADRP X0, previous page */
    write_instr(data, GETVAR + 4, 0x912C0000); /* ADD X0, X0, #0xB00 */
    assert(calc_adrl_file_offset(data, GETVAR, 0) == COUNTRY);
}

static void expect_failure_unchanged(char* data) {
    char before[SIZE];
    memcpy(before, data, SIZE);
    assert(patch_hwcountry_global(data, SIZE) == -1);
    assert(memcmp(before, data, SIZE) == 0);
}

int main(void) {
    char data[SIZE];
    fixture(data);
    char before[SIZE];
    memcpy(before, data, SIZE);
    assert(patch_hwcountry_global(data, SIZE) == 0);
    assert(calc_adrl_file_offset(data, GETVAR, 0) == SLACK);
    assert(calc_adrl_file_offset(data, DISPLAY, 0) == SLACK + 7);
    assert(strcmp(data + SLACK, "GLOBAL") == 0);
    assert(strcmp(data + SLACK + 7, "HwCountry: GLOBAL") == 0);
    for (int32_t i = 0; i < SIZE; ++i) {
        if ((i >= GETVAR && i < GETVAR + 8) || (i >= DISPLAY && i < DISPLAY + 8)
            || (i >= SLACK && i < SLACK + 25)) continue;
        assert(data[i] == before[i]);
    }

    fixture(data);
    data[FORMAT] = 'X';
    expect_failure_unchanged(data);

    fixture(data);
    data[0xFFF] = 1; /* Occupied tail must not be overwritten. */
    expect_failure_unchanged(data);

    fixture(data);
    memcpy(data + GETVAR + 16, data + GETVAR, 8); /* Ambiguous getvar. */
    expect_failure_unchanged(data);

    fixture(data);
    write_instr(data, SECTION + 12, 0x400); /* Non-flat PE mapping. */
    expect_failure_unchanged(data);

    fixture(data);
    write_instr(data, 0x500, 0x90000005);
    write_instr(data, 0x504, 0x913F9CA5); /* Existing reference to tail padding. */
    assert(calc_adrl_file_offset(data, 0x500, 0) == SLACK);
    expect_failure_unchanged(data);

    fixture(data);
    write_instr(data, 0x104, 6); /* NumberOfRvaAndSizes. */
    write_instr(data, 0x130, 0x1800); /* Base relocation directory. */
    write_instr(data, 0x134, 10);
    write_instr(data, 0x1800, 0);
    write_instr(data, 0x1804, 10);
    put16(data, 0x1808, 0xA000 | SLACK); /* DIR64 relocation into padding. */
    expect_failure_unchanged(data);

    puts("HwCountry tests passed (success and six rejected layouts)");
    return EXIT_SUCCESS;
}
