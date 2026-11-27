package pl.matter2supla.finder

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.io.IOException
import java.security.GeneralSecurityException
import java.security.KeyStore
import java.security.ProviderException
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class GatewayStorage(context: Context) {
    private val prefs = context.getSharedPreferences("matter2supla_prefs", Context.MODE_PRIVATE)

    init {
        val legacyKeys = prefs.all.keys.filter { it.startsWith("cred_") || it.startsWith("fp_") }
        if (legacyKeys.isNotEmpty()) {
            val editor = prefs.edit()
            legacyKeys.forEach { editor.remove(it) }
            editor.apply()
        }
    }

    fun getSavedIps(): List<String> {
        val raw = prefs.getString("saved_ips", "") ?: ""
        return raw.split(",").map { it.trim() }.filter { it.isNotEmpty() }
    }

    fun saveIp(ip: String) {
        val current = getSavedIps().toMutableList()
        current.remove(ip)
        current.add(0, ip)
        prefs.edit().putString("saved_ips", current.take(5).joinToString(",")).apply()
    }

    fun getTrustedFingerprint(ip: String): String? {
        return prefs.getString("trusted_v2_$ip", null)
    }

    fun saveTrustedFingerprint(ip: String, sha256Hex: String) {
        prefs.edit().remove("encrypted_$ip").putString("trusted_v2_$ip", sha256Hex).apply()
    }

    fun getSavedCredential(ip: String, fingerprint: String): String? {
        if (fingerprint != getTrustedFingerprint(ip)) return null
        val envelope = prefs.getString("encrypted_$ip", null) ?: return null
        return try {
            val parts = envelope.split(':', limit = 2)
            if (parts.size != 2) return null
            val iv = Base64.decode(parts[0], Base64.NO_WRAP)
            val ciphertext = Base64.decode(parts[1], Base64.NO_WRAP)
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, credentialKey(create = false), GCMParameterSpec(128, iv))
            cipher.updateAAD("$ip:$fingerprint".toByteArray(Charsets.UTF_8))
            cipher.doFinal(ciphertext).toString(Charsets.UTF_8)
        } catch (_: GeneralSecurityException) {
            clearCredential(ip)
            null
        } catch (_: IOException) {
            null
        } catch (_: ProviderException) {
            null
        } catch (_: IllegalArgumentException) {
            clearCredential(ip)
            null
        }
    }

    fun saveCredential(ip: String, fingerprint: String, credential: String): Boolean {
        if (fingerprint != getTrustedFingerprint(ip)) return false
        return try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE, credentialKey(create = true))
            cipher.updateAAD("$ip:$fingerprint".toByteArray(Charsets.UTF_8))
            val iv = Base64.encodeToString(cipher.iv, Base64.NO_WRAP)
            val ciphertext = Base64.encodeToString(cipher.doFinal(credential.toByteArray(Charsets.UTF_8)), Base64.NO_WRAP)
            prefs.edit().putString("encrypted_$ip", "$iv:$ciphertext").apply()
            true
        } catch (_: GeneralSecurityException) {
            false
        } catch (_: IOException) {
            false
        } catch (_: ProviderException) {
            false
        }
    }

    fun clearCredential(ip: String) {
        prefs.edit().remove("encrypted_$ip").apply()
    }

    private fun credentialKey(create: Boolean): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val alias = "matter2supla-finder-credentials-v2"
        (store.getKey(alias, null) as? SecretKey)?.let { return it }
        if (!create) throw GeneralSecurityException("Brak klucza danych logowania")
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(256)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build()
        )
        return generator.generateKey()
    }
}
