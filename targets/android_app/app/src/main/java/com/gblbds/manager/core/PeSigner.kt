package com.gblbds.manager.core

import java.io.ByteArrayOutputStream
import java.math.BigInteger

/**
 * SFB PE signing: the Kotlin port of tools/sfbsec/sign_efi.py.
 *
 * The signature is a detached, no-attribute PKCS#7 (SHA-256 + RSA) over the
 * canonical stream (the whole file except the COFF checksum field, the
 * security data-directory entry and the certificate table itself; bytes
 * after the certificate table stay in the hash), appended as a
 * WIN_CERTIFICATE entry with the security directory patched to point at it -
 * the exact format the BDS SfbPeVerify.c accepts.
 */
object PeSigner {
    private const val PE_DIR_SECURITY = 4

    // ---- DER helpers (hand-rolled, no third-party ASN.1 library) ----------

    private fun der(tag: Int, content: ByteArray): ByteArray {
        val out = ByteArrayOutputStream()
        out.write(tag)
        var len = content.size
        if (len < 0x80) {
            out.write(len)
        } else {
            var n = 0
            var v = len
            while (v > 0) { n++; v = v shr 8 }
            out.write(0x80 or n)
            for (shift in (n - 1) * 8 downTo 0 step 8) {
                out.write((len shr shift) and 0xFF)
            }
        }
        out.write(content)
        return out.toByteArray()
    }

    private fun seq(vararg parts: ByteArray) = der(0x30, parts.fold(ByteArray(0)) { a, b -> a + b })
    private fun set(vararg parts: ByteArray) = der(0x31, parts.fold(ByteArray(0)) { a, b -> a + b })
    private fun octet(b: ByteArray) = der(0x04, b)
    private fun int(v: BigInteger) = der(0x02, v.toByteArray())
    private fun int(v: Long) = int(BigInteger.valueOf(v))
    private fun nullParams() = byteArrayOf(0x05, 0x00)
    private fun ctx(n: Int, content: ByteArray) = der(0xA0 or n, content)

    private fun oid(oid: String): ByteArray {
        val parts = oid.split(".").map { it.toInt() }
        val body = ByteArrayOutputStream()
        body.write(parts[0] * 40 + parts[1])
        for (i in 2 until parts.size) {
            var v = parts[i]
            val stack = mutableListOf<Int>()
            stack.add(0, v and 0x7F)
            v = v shr 7
            while (v > 0) {
                stack.add(0, (v and 0x7F) or 0x80)
                v = v shr 7
            }
            for (b in stack) body.write(b)
        }
        return der(0x06, body.toByteArray())
    }

    // ---- certificate parsing (issuer Name + serialNumber) ------------------

    private class DerReader(private val buf: ByteArray, private var pos: Int = 0) {
        fun read(): Pair<Int, ByteArray> {
            val tag = buf[pos].toInt() and 0xFF
            var p = pos + 1
            var len = buf[p].toInt() and 0xFF
            p++
            if (len and 0x80 != 0) {
                val n = len and 0x7F
                len = 0
                repeat(n) { len = (len shl 8) or (buf[p].toInt() and 0xFF); p++ }
            }
            val content = buf.copyOfRange(p, p + len)
            pos = p + len
            return tag to content
        }

        fun atEnd(): Boolean = pos >= buf.size
    }

    /** Issuer Name as complete DER (with its SEQUENCE header) and serial. */
    fun certIssuerAndSerial(certDer: ByteArray): Pair<ByteArray, BigInteger> {
        val top = DerReader(certDer)
        val (t1, cert) = top.read()          // Certificate SEQUENCE
        require(t1 == 0x30)
        val r = DerReader(cert)
        val (tt, tbs) = r.read()             // tbsCertificate SEQUENCE
        require(tt == 0x30)
        val t = DerReader(tbs)
        var (tag, v) = t.read()
        if (tag == 0xA0) {                   // [0] EXPLICIT version
            val p = t.read(); tag = p.first; v = p.second
        }
        require(tag == 0x02)                 // serialNumber INTEGER
        val serial = BigInteger(1, v)
        t.read()                             // signature AlgorithmIdentifier
        // issuer Name: keep the complete DER (tag + length + content) -
        // PKCS7's IssuerAndSerialNumber embeds the whole Name SEQUENCE.
        val (iTag, iContent) = t.read()
        require(iTag == 0x30)
        return der(iTag, iContent) to serial
    }

    // ---- PKCS#7 detached, no attributes (SHA-256 + RSA) --------------------

    private val OID_SIGNED_DATA = "1.2.840.113549.1.7.2"
    private val OID_DATA = "1.2.840.113549.1.7.1"
    private val OID_SHA256 = "2.16.840.1.101.3.4.2.1"
    private val OID_RSA = "1.2.840.113549.1.1.1"

    /**
     * Build the PKCS#7 matching `openssl smime -sign -noattr -binary
     * -md sha256` output: ContentInfo{signedData, SignedData{v1,
     * digestAlgorithms{sha256}, contentInfo{data} (detached),
     * certificates[cert], signerInfos{SignerInfo}}}.
     */
    fun buildPkcs7(certDer: ByteArray, signature: ByteArray): ByteArray {
        val (issuer, serial) = certIssuerAndSerial(certDer)
        val signerInfo = seq(
            int(1),
            seq(issuer, int(serial)),                  // issuerAndSerialNumber
            seq(oid(OID_SHA256)),                      // digestAlgorithm
            seq(oid(OID_RSA), nullParams()),           // digestEncryptionAlgorithm
            octet(signature),
        )
        val signedData = seq(
            int(1),
            set(seq(oid(OID_SHA256))),
            seq(oid(OID_DATA)),                        // detached contentInfo
            ctx(0, certDer),                           // [0] certificates
            set(signerInfo),
        )
        return seq(oid(OID_SIGNED_DATA), ctx(0, signedData))
    }

    // ---- PE layout / canonical stream (port of sign_efi.py) ----------------

    class PeLayout(val checksumOff: Int, val secDirOff: Int, val sizeOfHeaders: Int)

    fun peLayout(blob: ByteArray): PeLayout {
        require(blob.size > 0x40) { "not a PE image" }
        val peOff = u32(blob, 0x3c)
        require(peOff >= 0 && peOff + 4 <= blob.size &&
                blob[peOff] == 'P'.code.toByte() &&
                blob[peOff + 1] == 'E'.code.toByte()) { "not a PE image" }
        val optOff = peOff + 4 + 20
        require(optOff + 64 <= blob.size) { "truncated optional header" }
        val magic = u16(blob, optOff)
        val dataDirOff = when (magic) {
            0x010b -> optOff + 96   // PE32
            0x020b -> optOff + 112  // PE32+
            else -> throw IllegalArgumentException("bad optional header magic")
        }
        val checksumOff = optOff + 64
        val secDirOff = dataDirOff + PE_DIR_SECURITY * 8
        require(secDirOff + 8 <= blob.size) { "truncated security directory" }
        val sizeOfHeaders = u32(blob, optOff + 60)
        require(sizeOfHeaders in 1..blob.size &&
                checksumOff + 4 <= secDirOff &&
                secDirOff + 8 <= sizeOfHeaders) { "bad PE headers" }
        return PeLayout(checksumOff, secDirOff, sizeOfHeaders)
    }

    private fun u16(b: ByteArray, o: Int) =
        ((b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8))

    private fun u32(b: ByteArray, o: Int): Int =
        (u16(b, o) or (u16(b, o + 2) shl 16))

    private fun put32(b: ByteArray, o: Int, v: Int) {
        b[o] = (v and 0xFF).toByte()
        b[o + 1] = ((v shr 8) and 0xFF).toByte()
        b[o + 2] = ((v shr 16) and 0xFF).toByte()
        b[o + 3] = ((v shr 24) and 0xFF).toByte()
    }

    /** TRUE when the image carries a certificate table already. */
    fun hasSignature(blob: ByteArray): Boolean {
        val l = peLayout(blob)
        val size = u32(blob, l.secDirOff + 4)
        return size != 0
    }

    /** Strip any existing certificate table, keeping data that follows it. */
    fun stripSignature(blob: ByteArray): ByteArray {
        val l = peLayout(blob)
        val off = u32(blob, l.secDirOff)
        val size = u32(blob, l.secDirOff + 4)
        if (size == 0 || off == 0) return blob
        require(size > 0 && off >= l.sizeOfHeaders &&
                off.toLong() + size <= blob.size) {
            "bad certificate table"
        }
        val out = ByteArray(blob.size - size)
        System.arraycopy(blob, 0, out, 0, off)
        System.arraycopy(blob, off + size, out, off, blob.size - off - size)
        put32(out, l.secDirOff, 0)
        put32(out, l.secDirOff + 4, 0)
        return out
    }

    /** PKCS#7 DER body of the WIN_CERTIFICATE, or null. */
    private fun certificateTable(blob: ByteArray): ByteArray? {
        val l = peLayout(blob)
        val off = u32(blob, l.secDirOff)
        val size = u32(blob, l.secDirOff + 4)
        if (size <= 8 || off <= 0 || off + size > blob.size) return null
        val wLength = u16(blob, off)
        if (wLength <= 8 || off + wLength > blob.size) return null
        return blob.copyOfRange(off + 8, off + wLength)
    }

    /** Walk ContentInfo -> SignedData and return its content reader. */
    private fun signedData(pkcs7: ByteArray): DerReader? {
        val top = DerReader(pkcs7)
        val (t0, contentInfo) = top.read()
        if (t0 != 0x30) return null
        val ci = DerReader(contentInfo)
        ci.read()                                  // contentType OID
        val (t1, explicit) = ci.read()             // [0] EXPLICIT SignedData
        if (t1 != 0xA0) return null
        val (t2, body) = DerReader(explicit).read()
        if (t2 != 0x30) return null
        return DerReader(body)
    }

    /** DER of the first certificate in the image's PKCS#7, or null. */
    fun signerCert(blob: ByteArray): ByteArray? {
        val pkcs7 = certificateTable(blob) ?: return null
        return try {
            val sd = signedData(pkcs7) ?: return null
            sd.read()                              // version
            sd.read()                              // digestAlgorithms
            sd.read()                              // encapContentInfo
            val (t, certs) = sd.read()             // [0] certificates
            if (t != 0xA0) return null
            val (ct, cert) = DerReader(certs).read()
            if (ct != 0x30) return null
            der(ct, cert)
        } catch (e: Exception) {
            null
        }
    }

    /** RSA signature value from the first SignerInfo, or null. */
    fun signerSignature(blob: ByteArray): ByteArray? {
        val pkcs7 = certificateTable(blob) ?: return null
        return try {
            val sd = signedData(pkcs7) ?: return null
            sd.read()                              // version
            sd.read()                              // digestAlgorithms
            sd.read()                              // encapContentInfo
            var next = sd.read()
            if (next.first == 0xA0) next = sd.read()   // certificates
            if (next.first != 0x31) return null        // signerInfos SET
            val (st, signerInfo) = DerReader(next.second).read()
            if (st != 0x30) return null
            val si = DerReader(signerInfo)
            var signature: ByteArray? = null
            while (!si.atEnd()) {
                val e = si.read()
                if (e.first == 0x04) signature = e.second
            }
            signature
        } catch (e: Exception) {
            null
        }
    }

    /**
     * TRUE when the image carries a signature made by trustedCert over its
     * own canonical stream: the signer certificate must BE the trust anchor
     * and the RSA signature must verify. A signature from another key (for
     * example a keystore reset) is not trusted.
     */
    fun isTrustedSignature(blob: ByteArray, trustedCert: ByteArray?): Boolean {
        if (trustedCert == null) return false
        val certDer = signerCert(blob) ?: return false
        if (!certDer.contentEquals(trustedCert)) return false
        val sig = signerSignature(blob) ?: return false
        return try {
            val cert = java.security.cert.CertificateFactory.getInstance("X.509")
                .generateCertificate(certDer.inputStream())
            val verifier = java.security.Signature.getInstance("SHA256withRSA")
            verifier.initVerify(cert)
            verifier.update(canonicalStream(blob))
            verifier.verify(sig)
        } catch (e: Exception) {
            false
        }
    }

    /**
     * The byte stream the BDS authentihash covers: everything except the
     * checksum, the security directory entry and the certificate table.
     * Data after the certificate table is included, matching SfbPeVerify.c.
     */
    fun canonicalStream(blob: ByteArray): ByteArray {
        val l = peLayout(blob)
        val sdOff = u32(blob, l.secDirOff)
        val sdSize = u32(blob, l.secDirOff + 4)
        val certStart: Int
        val certEnd: Int
        if (sdSize > 0) {
            require(sdOff >= l.sizeOfHeaders &&
                    sdOff.toLong() + sdSize <= blob.size) {
                "bad certificate table"
            }
            certStart = sdOff
            certEnd = sdOff + sdSize
        } else if (sdSize == 0) {
            certStart = blob.size
            certEnd = blob.size
        } else {
            throw IllegalArgumentException("bad certificate table size")
        }
        val out = ByteArrayOutputStream()
        out.write(blob, 0, l.checksumOff)
        out.write(blob, l.checksumOff + 4, l.secDirOff - l.checksumOff - 4)
        out.write(ByteArray(8))
        out.write(blob, l.secDirOff + 8, l.sizeOfHeaders - l.secDirOff - 8)
        out.write(blob, l.sizeOfHeaders, certStart - l.sizeOfHeaders)
        out.write(blob, certEnd, blob.size - certEnd)
        return out.toByteArray()
    }

    /**
     * Assemble the signed image: `signature` is the RSA signature over the
     * canonical stream of the (already stripped) image, produced by the
     * AndroidKeyStore after a biometric prompt.
     */
    fun assembleSigned(blob: ByteArray, certDer: ByteArray,
                       signature: ByteArray): ByteArray {
        require(!hasSignature(blob)) { "strip before assembling" }
        val pkcs7 = buildPkcs7(certDer, signature)
        val l = peLayout(blob)

        // WIN_CERTIFICATE: wLength, revision 0x0200, type PKCS_SIGNED_DATA,
        // reserved, then the DER body, padded to an 8-byte multiple.
        var entry = ByteArray(8) + pkcs7
        if (entry.size % 8 != 0) {
            entry += ByteArray(8 - entry.size % 8)
        }
        entry[0] = (8 + pkcs7.size and 0xFF).toByte()
        entry[1] = ((8 + pkcs7.size) shr 8 and 0xFF).toByte()
        entry[2] = 0x00; entry[3] = 0x02
        entry[4] = 0x02; entry[5] = 0x00
        entry[6] = 0x00; entry[7] = 0x00

        val out = blob + entry
        put32(out, l.secDirOff, blob.size)
        put32(out, l.secDirOff + 4, entry.size)
        return out
    }

    /**
     * DigestInfo for RSA PKCS#1 v1.5 over SHA-256: the DER the secure element
     * signs under NONEwithRSA. Only these 51 bytes reach the keystore; the
     * caller hashes the image locally.
     */
    fun digestInfo(hash: ByteArray): ByteArray =
        seq(seq(oid(OID_SHA256), nullParams()), octet(hash))

    /** SHA-256 of the canonical stream, computed off the secure element. */
    fun canonicalHash(canonical: ByteArray): ByteArray =
        java.security.MessageDigest.getInstance("SHA-256").digest(canonical)

    /** Full in-process flow except the signature itself. */
    fun prepare(blob: ByteArray): Pair<ByteArray, ByteArray> =
        // (stripped image, canonical stream to hash locally)
        stripSignature(blob).let { it to canonicalStream(it) }
}
