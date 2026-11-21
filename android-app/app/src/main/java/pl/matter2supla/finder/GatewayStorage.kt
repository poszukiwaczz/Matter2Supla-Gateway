package pl.matter2supla.finder

import android.content.Context

class GatewayStorage(context: Context) {
    private val prefs = context.getSharedPreferences("matter2supla_prefs", Context.MODE_PRIVATE)

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
        return prefs.getString("fp_$ip", null)
    }

    fun saveTrustedFingerprint(ip: String, sha256Hex: String) {
        prefs.edit().putString("fp_$ip", sha256Hex).apply()
    }

    fun getSavedCredential(ip: String): String? {
        return prefs.getString("cred_$ip", null)
    }

    fun saveCredential(ip: String, credential: String) {
        prefs.edit().putString("cred_$ip", credential).apply()
    }

    fun clearCredential(ip: String) {
        prefs.edit().remove("cred_$ip").apply()
    }
}
