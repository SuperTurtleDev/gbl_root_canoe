package com.gblbds.manager.core

import android.app.Activity
import android.hardware.biometrics.BiometricManager
import android.hardware.biometrics.BiometricPrompt
import android.os.CancellationSignal
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.security.keystore.StrongBoxUnavailableException
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.Signature
import java.security.cert.Certificate
import java.security.spec.RSAKeyGenParameterSpec
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicReference
import javax.security.auth.x500.X500Principal

/**
 * The secure-boot signing identity: an RSA-2048 key inside AndroidKeyStore.
 *
 * Mirrors the csigner HardwareKey design:
 *   - non-exportable private key, StrongBox first with TEE fallback
 *   - auth-per-use: every signature needs a BiometricPrompt CryptoObject
 *     bound to that exact keystore operation challenge
 *   - the self-signed keystore certificate is the BOOT_CERTS trust anchor
 */
object HardwareKey {
    // v2: NONEwithRSA over a pre-computed DigestInfo. v1 ("SHA256withRSA",
    // key digest SHA-256) streamed the whole multi-megabyte canonical image
    // into the StrongBox secure element and froze the app; the caller now
    // hashes locally and only the 51-byte DigestInfo enters the chip. New
    // alias so the stale v1 key can never be selected.
    const val ALIAS = "sfb_boot_cert_v2"
    private const val PROVIDER = "AndroidKeyStore"
    private const val SIG_ALGORITHM = "NONEwithRSA"

    /**
     * The keystore calls MUST NOT run on the main thread.
     *
     * Signature.update() streams the entire payload into the secure element;
     * with a StrongBox key and an EFI image that is several seconds of binder
     * time to keymint, which ANRs the activity. BiometricPrompt delivers its
     * callback on activity.mainExecutor, so the crypto is handed to this
     * single background thread instead.
     */
    private val crypto = Executors.newSingleThreadExecutor { r ->
        Thread(r, "keystore-sign").apply { isDaemon = true }
    }

    fun hasSecureLockScreen(activity: Activity): Boolean {
        return hasSecureLockScreen(activity as android.content.Context)
    }

    /**
     * TRUE when a PIN/pattern/password is configured. The auth-per-use key is
     * unusable without one, so secure boot must be off in that state.
     */
    fun hasSecureLockScreen(context: android.content.Context): Boolean {
        val km = context.getSystemService(android.app.KeyguardManager::class.java)
        return km != null && km.isDeviceSecure
    }

    fun exists(): Boolean = try {
        val ks = KeyStore.getInstance(PROVIDER).apply { load(null) }
        ks.getCertificate(ALIAS) != null
    } catch (e: Exception) {
        false
    }

    /** Remove the key (disabling secure boot). */
    fun delete() {
        try {
            val ks = KeyStore.getInstance(PROVIDER).apply { load(null) }
            if (ks.containsAlias(ALIAS)) ks.deleteEntry(ALIAS)
        } catch (e: Exception) {
            // Nothing usable to delete.
        }
    }

    /** Create the key; StrongBox first, TEE when unavailable. */
    fun generate() {
        if (exists()) return
        try {
            generate(strongBox = true)
        } catch (e: StrongBoxUnavailableException) {
            generate(strongBox = false)
        }
    }

    private fun generate(strongBox: Boolean) {
        val b = KeyGenParameterSpec.Builder(
            ALIAS,
            KeyProperties.PURPOSE_SIGN or KeyProperties.PURPOSE_VERIFY)
            .setAlgorithmParameterSpec(
                RSAKeyGenParameterSpec(2048, RSAKeyGenParameterSpec.F4))
            .setDigests(KeyProperties.DIGEST_NONE)
            .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
            .setCertificateSubject(X500Principal("CN=sfb boot cert"))
            .setCertificateSerialNumber(
                java.math.BigInteger.valueOf(System.currentTimeMillis()))
            .setCertificateNotBefore(java.util.Date())
            .setCertificateNotAfter(java.util.Date(
                System.currentTimeMillis() + 10000L * 24 * 3600 * 1000))
            .setUserAuthenticationRequired(true)
            // Zero-second validity = auth-per-use: every signature needs its
            // own fingerprint prompt, nothing stays usable afterwards.
            .setUserAuthenticationParameters(
                0, KeyProperties.AUTH_BIOMETRIC_STRONG)
        if (strongBox) {
            b.setIsStrongBoxBacked(true)
        }
        val kpg = KeyPairGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_RSA, PROVIDER)
        kpg.initialize(b.build())
        kpg.generateKeyPair()
    }

    /** Self-signed certificate in DER form, or null when the key is absent. */
    fun certDer(): ByteArray? = try {
        val ks = KeyStore.getInstance(PROVIDER).apply { load(null) }
        ks.getCertificate(ALIAS)?.encoded
    } catch (e: Exception) {
        null
    }

    @Suppress("unused")
    fun signBlocking(activity: Activity, data: ByteArray,
                     err: AtomicReference<String>): ByteArray? {
        val latch = CountDownLatch(1)
        val out = AtomicReference<ByteArray?>(null)
        activity.runOnUiThread {
            sign(activity, data, object : SignCallback {
                override fun onSigned(signature: ByteArray) {
                    out.set(signature); latch.countDown()
                }
                override fun onFailed(message: String) {
                    err.set(message); latch.countDown()
                }
            })
        }
        latch.await()
        return out.get()
    }

    interface SignCallback {
        fun onSigned(signature: ByteArray)
        fun onFailed(message: String)
    }

    /**
     * Prompt for biometrics and sign `digestInfo` (the DER DigestInfo built
     * by PeSigner.digestInfo; the raw image is hashed by the caller - only
     * these 51 bytes reach the secure element).
     */
    fun sign(activity: Activity, digestInfo: ByteArray, cb: SignCallback) {
        try {
            val ks = KeyStore.getInstance(PROVIDER).apply { load(null) }
            val key = ks.getKey(ALIAS, null) as? PrivateKey
            if (key == null) {
                cb.onFailed("no key"); return
            }

            val signature = Signature.getInstance(SIG_ALGORITHM)
            signature.initSign(key)   // mints the per-operation challenge

            val prompt = BiometricPrompt.Builder(activity)
                .setTitle(activity.getString(
                    com.gblbds.manager.R.string.sb_prompt_title))
                .setSubtitle(activity.getString(
                    com.gblbds.manager.R.string.sb_prompt_subtitle))
                .setAllowedAuthenticators(
                    BiometricManager.Authenticators.BIOMETRIC_STRONG)
                .build()

            prompt.authenticate(
                BiometricPrompt.CryptoObject(signature),
                CancellationSignal(),
                activity.mainExecutor,
                object : BiometricPrompt.AuthenticationCallback() {
                    override fun onAuthenticationSucceeded(
                        result: BiometricPrompt.AuthenticationResult) {
                        // Off the main thread: update()/sign() are slow binder
                        // calls into keymint (see `crypto` above). The auth
                        // token is bound to this operation, so starting the
                        // work immediately is enough; it need not run inline.
                        crypto.execute {
                            try {
                                signature.update(digestInfo)
                                cb.onSigned(signature.sign())
                            } catch (e: Exception) {
                                cb.onFailed(e.message ?: e.toString())
                            }
                        }
                    }

                    override fun onAuthenticationError(code: Int, msg: CharSequence) {
                        cb.onFailed(msg.toString())
                    }

                    override fun onAuthenticationFailed() {
                        // keep the prompt open for a retry
                    }
                })
        } catch (e: Exception) {
            cb.onFailed(e.message ?: e.toString())
        }
    }
}
