package com.gblbds.manager.core

import com.gblbds.manager.R

/**
 * One EFI bundled with the app and kept in sync with the device. A single
 * "BDS update" covers the BDS core, these additional EFIs and the system
 * boot entries below.
 *
 * [asset] is the APK asset name; [destName] the file under persist/efisp
 * (null for the BDS, which lives in the raw efisp partition); [signable]
 * marks images secure boot signs - the BDS is the root of trust and is
 * never signed.
 */
enum class SystemEfi(
    val asset: String,
    val destName: String?,
    val signable: Boolean,
    val labelRes: Int,
) {
    BDS("BDS.efi", null, false, R.string.sys_efi_bds),
    FASTBOOT_WRAPPER("secure_fastboot.efi", Device.WRAPPER_NAME, true,
        R.string.sys_efi_wrapper),
}

/**
 * A hardcoded system boot entry template. `%s` is the persist volume's
 * (disk,partition) address; every system loader is chainloaded through
 * secure_fastboot.efi:
 *
 *   Android:(5,8)\efisp\secure_fastboot.efi \efisp\boot.efi
 */
data class SystemBootEntry(val template: String) {
    /** The image this entry chainloads: the template's argument. */
    val target: String get() = template.substringAfterLast(' ')

    /** Render the line for the given "disk,partition" address. */
    fun line(address: String): String = template.format(address)
}

val SYSTEM_BOOT_ENTRIES = listOf(
    SystemBootEntry(
        "Android:(%s)\\efisp\\secure_fastboot.efi \\efisp\\boot.efi"),
    SystemBootEntry(
        "Android Backup:(%s)\\efisp\\secure_fastboot.efi \\efisp\\boot_backup.efi"),
)
