#include "patchs/core.h"
#include "patchs/xiaomi/hwcountry.h"
//FILE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
/* ==================== main ==================== */
int32_t read_file(const char* filename, char** data, int32_t* size) {
    FILE* file = fopen(filename, "rb");
    if (!file) return -1;
    fseek(file, 0, SEEK_END);
    *size = ftell(file);
    fseek(file, 0, SEEK_SET);
    *data = (char*)malloc(*size);
    if (!*data) { fclose(file); return -1; }
    if ((int32_t)fread(*data, 1, *size, file) != *size) {
        free(*data); fclose(file); return -1;
    }
    fclose(file);
    return 0;
}
int32_t main(int32_t argc, char* argv[]) {
    bool hwcountry_global = argc == 4 && strcmp(argv[3], "--hwcountry-global") == 0;
    if (argc != 3 && !hwcountry_global) {
        printf("Usage: %s <input_file> <output_file> [--hwcountry-global]\n", argv[0]);
        return EXIT_FAILURE;
    }
    char* data = NULL;
    int32_t size = 0;
    if (read_file(argv[1], &data, &size) != 0) {
        printf("Failed to read file: %s\n", argv[1]);
        return EXIT_FAILURE;
    }
    if (hwcountry_global && patch_hwcountry_global(data, size) != 0) {
        printf("Error: Failed to patch HwCountry -> GLOBAL\n");
        free(data);
        return EXIT_FAILURE;
    }
    if (!PatchBuffer(data,size))
    {
        printf("Patching failed\n");
        free(data);
        return EXIT_FAILURE;
    }
    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        printf("Failed to open output: %s\n", argv[2]);
        free(data);
        return EXIT_FAILURE;
    }
    if (fwrite(data, 1, size, out) != size) {
        printf("Failed to write output\n");
        fclose(out);
        free(data);
        return EXIT_FAILURE;
    }
    fclose(out);
    free(data);
    printf("Saved to %s\n", argv[2]);
    return EXIT_SUCCESS;
}
