package com.gblbds.manager.core

/**
 * One CONFIG\ENTRIES.CFG line: "<name>[:(d,p)]<path> [args]".
 * Parsing is hand-written - no regular expressions in this codebase.
 */
data class BootEntry(
    val id: Long,
    val name: String,
    val address: String?,           // "(d,p)" or null
    val path: String,               // e.g. \efisp\MyApp.efi
    val args: String = "",
    val stagedFile: String? = null, // local file copied into efisp on save
) {
    val fileName: String get() = path.substringAfterLast('\\')

    /**
     * TRUE when the entry boots TargetPath, either directly or through the
     * secure-fastboot wrapper (`\efisp\secure_fastboot.efi <target> ...`).
     */
    fun targets(targetPath: String): Boolean {
        if (path.equals(targetPath, true)) return true
        if (fileName.equals("secure_fastboot.efi", true)) {
            return args.substringBefore(' ').equals(targetPath, true)
        }
        return false
    }

    /**
     * The Android loader and its backup, direct or wrapper-routed: never
     * deletable or renamable from the editor.
     */
    val isSystem: Boolean
        get() = targets("\\efisp\\boot.efi") ||
                targets("\\efisp\\boot_backup.efi")

    fun toLine(): String = buildString {
        append(name)
        append(':')
        address?.let { append(it) }
        append(path)
        if (args.isNotBlank()) append(' ').append(args)
    }

    companion object {
        private var nextId = 1L

        fun newId(): Long = nextId++

        fun parse(line: String): BootEntry? {
            val text = line.trim()
            val colon = text.indexOf(':')
            if (colon <= 0 || colon == text.length - 1) return null

            var rest = colon + 1
            var address: String? = null
            if (text[rest] == '(') {
                val close = text.indexOf(')', rest + 1)
                if (close < 0) return null
                address = text.substring(rest, close + 1)
                rest = close + 1
            }
            if (rest >= text.length || text[rest] != '\\') return null

            val blank = text.indexOf(' ', rest)
            val path = if (blank < 0) text.substring(rest)
                       else text.substring(rest, blank)
            val args = if (blank < 0) "" else text.substring(blank + 1).trim()
            return BootEntry(newId(), text.substring(0, colon), address, path, args)
        }
    }
}
