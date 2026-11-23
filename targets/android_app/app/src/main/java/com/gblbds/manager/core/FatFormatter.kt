package com.gblbds.manager.core

import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Byte-for-byte port of the BDS embedded FAT formatter
 * (QcomModulePkg/Library/SfbConfigStoreLib/SuperFbConfigFormat.c).
 *
 * Produces a complete, freshly formatted FAT12/16/32 image for the config
 * container. The caller must write exactly containerBytes of it at the
 * 1 MiB offset of the efisp partition - never a byte beyond.
 */
object FatFormatter {
    const val SECTOR = 512
    private const val BYTES_PER_MIB = 1024 * 1024
    const val BDS_RESERVED_BYTES = BYTES_PER_MIB

    data class Layout(
        val type: Int,
        val totalSectors: Int,
        val spc: Int,
        val numFats: Int = 2,
        val reserved: Int,
        val rootEntries: Int,
        val rootDirSectors: Int,
        var fatSectors: Int = 1,
        var clusters: Int = 0,
        var dataStart: Int = 0,
    )

    private fun put16(b: ByteArray, off: Int, v: Int) {
        b[off] = (v and 0xFF).toByte()
        b[off + 1] = ((v shr 8) and 0xFF).toByte()
    }

    private fun put32(b: ByteArray, off: Int, v: Int) {
        put16(b, off, v)
        put16(b, off + 2, v shr 16)
    }

    private fun typeOfClusters(c: Int): Int = when {
        c <= 4084 -> 12
        c <= 65524 -> 16
        else -> 32
    }

    private fun preferredSpc(type: Int, total: Int): Int = when (type) {
        16 -> when {
            total <= 32680 -> 2
            total <= 262144 -> 4
            total <= 524288 -> 8
            else -> 16
        }
        32 -> when {
            total <= 16777216 -> 8
            total <= 33554432 -> 16
            total <= 67108864 -> 32
            else -> 64
        }
        else -> 1
    }

    private fun tryLayout(total: Int, type: Int, spc: Int): Layout? {
        val l = Layout(
            type = type,
            totalSectors = total,
            spc = spc,
            reserved = if (type == 32) 32 else 1,
            rootEntries = if (type == 32) 0 else 512,
            rootDirSectors = (if (type == 32) 0 else 512 * 32 + SECTOR - 1) / SECTOR,
        )
        repeat(64) {
            val overhead = l.reserved + l.numFats * l.fatSectors + l.rootDirSectors
            if (overhead >= total) return null
            l.clusters = (total - overhead) / spc
            val fatBytes = if (type == 12) {
                ((l.clusters.toLong() + 2) * 3 + 1) / 2
            } else {
                (l.clusters.toLong() + 2) * (type / 8)
            }
            val need = ((fatBytes + SECTOR - 1) / SECTOR).toInt()
            if (need <= l.fatSectors) return@repeat
            l.fatSectors = need
        }
        val overhead = l.reserved + l.numFats * l.fatSectors + l.rootDirSectors
        if (overhead >= total) return null
        l.clusters = (total - overhead) / spc
        l.dataStart = overhead
        if (l.clusters == 0 || l.clusters > 0x0FFFFFF4) return null
        return if (typeOfClusters(l.clusters) == type) l else null
    }

    fun plan(sectors: Int): Layout {
        require(sectors >= 65536 / SECTOR) { "container too small" }
        var type = if (sectors < 8400) 12 else if (sectors < 1048576) 16 else 32
        while (type >= 12) {
            tryLayout(sectors, type, preferredSpc(type, sectors))?.let { return it }
            var spc = 1
            while (spc <= 64) {
                tryLayout(sectors, type, spc)?.let { return it }
                spc *= 2
            }
            if (type == 12) break
            type = if (type == 32) 16 else type - 4
        }
        error("cannot lay out a FAT volume of $sectors sectors")
    }

    /** Boot sector identical to SfbFatBuildBootSector. */
    private fun bootSector(l: Layout): ByteArray {
        val s = ByteArray(SECTOR)
        s[0] = 0xEB.toByte()
        s[1] = (if (l.type == 32) 0x58 else 0x3C).toByte()
        s[2] = 0x90.toByte()
        "MSWIN4.1".toByteArray().copyInto(s, 3)
        put16(s, 11, SECTOR)
        s[13] = l.spc.toByte()
        put16(s, 14, l.reserved)
        s[16] = l.numFats.toByte()
        put16(s, 17, l.rootEntries)
        if (l.type != 32 && l.totalSectors < 0x10000) {
            put16(s, 19, l.totalSectors)
        } else {
            put32(s, 32, l.totalSectors)
        }
        s[21] = 0xF8.toByte()
        put16(s, 24, 63)
        put16(s, 26, 255)
        val ext: Int
        if (l.type == 32) {
            put32(s, 36, l.fatSectors)
            put32(s, 44, 2)
            put16(s, 48, 1)
            put16(s, 50, 6)
            ext = 64
        } else {
            put16(s, 22, l.fatSectors)
            ext = 36
        }
        s[ext] = 0x80.toByte()
        s[ext + 2] = 0x29.toByte()
        put32(s, ext + 3, 0x53464200 xor l.totalSectors)
        "SFBCONFIG  ".toByteArray().copyInto(s, ext + 7)
        (when (l.type) {
            12 -> "FAT12   "
            16 -> "FAT16   "
            else -> "FAT32   "
        }).toByteArray().copyInto(s, ext + 18)
        s[510] = 0x55.toByte()
        s[511] = 0xAA.toByte()
        return s
    }

    /** Format the whole container into a fresh image of exactly `sectors` 512-byte sectors. */
    fun format(sectors: Int): ByteArray {
        val l = plan(sectors)
        val img = ByteArray(sectors * SECTOR)

        // Clear reserved area, FATs and root directory / root cluster.
        val clearEnd = l.dataStart + (if (l.type == 32) l.spc else 0)
        java.util.Arrays.fill(img, 0, clearEnd * SECTOR, 0.toByte())

        // First sector of each FAT: media descriptor + EOC markers.
        val fat0 = ByteArray(SECTOR)
        when (l.type) {
            12 -> {
                fat0[0] = 0xF8.toByte(); fat0[1] = 0xFF.toByte(); fat0[2] = 0xFF.toByte()
            }
            16 -> {
                put16(fat0, 0, 0xFFF8); put16(fat0, 2, 0xFFFF)
            }
            else -> {
                put32(fat0, 0, 0x0FFFFFF8.toInt())
                put32(fat0, 4, 0x0FFFFFFF.toInt())
                put32(fat0, 8, 0x0FFFFFFF.toInt())
            }
        }
        repeat(l.numFats) { f ->
            fat0.copyInto(img, (l.reserved + f * l.fatSectors) * SECTOR)
        }

        // Volume label entry first in the root directory.
        val label = ByteArray(SECTOR)
        "SFBCONFIG  ".toByteArray().copyInto(label, 0)
        label[11] = 0x08
        val rootSector = if (l.type == 32) l.dataStart
        else l.reserved + l.numFats * l.fatSectors
        label.copyInto(img, rootSector * SECTOR)

        if (l.type == 32) {
            val fsi = ByteArray(SECTOR)
            put32(fsi, 0, 0x41615252)
            put32(fsi, 484, 0x61417272)
            put32(fsi, 488, l.clusters - 1)
            put32(fsi, 492, 3)
            put32(fsi, 508, 0xAA550000.toInt())
            fsi.copyInto(img, 1 * SECTOR)
            fsi.copyInto(img, 7 * SECTOR)
            bootSector(l).copyInto(img, 6 * SECTOR)
        }

        // Boot sector last: an interrupted format is retried next boot.
        bootSector(l).copyInto(img, 0)
        return img
    }
}
