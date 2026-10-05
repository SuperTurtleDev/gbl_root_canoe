#ifndef PATCHS_XIAOMI_HWCOUNTRY_H
#define PATCHS_XIAOMI_HWCOUNTRY_H

#include <stdint.h>

/* Returns 0 on success, -1 if the supported HwCountry layout is absent. */
int32_t patch_hwcountry_global(char* buffer, int32_t size);

#endif
