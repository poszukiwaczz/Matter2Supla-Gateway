package pl.matter2supla.finder

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.net.URL
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import javax.net.ssl.HostnameVerifier
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.X509TrustManager

data class ApiResult(
    val code: Int,
    val json: JSONObject?,
    val rawBody: String,
    val error: String? = null
) {
    val isOk: Boolean get() = code in 200..299 && error == null
}

class GatewayApiClient(
    val ip: String,
    val pinnedSha256: String,
    val credential: String
) {
    private val sslContext: SSLContext = SSLContext.getInstance("TLS").apply {
        val tm = object : X509TrustManager {
            override fun checkClientTrusted(chain: Array<out X509Certificate>?, authType: String?) {}
            override fun checkServerTrusted(chain: Array<out X509Certificate>?, authType: String?) {
                val cert = chain?.firstOrNull()
                    ?: throw CertificateException("Brak certyfikatu serwera")
                val actual = GatewayDiscovery.sha256Hex(cert.encoded)
                if (!actual.equals(pinnedSha256, ignoreCase = true)) {
                    throw CertificateException("Niezgodny odcisk certyfikatu SHA-256")
                }
            }
            override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
        }
        init(null, arrayOf(tm), SecureRandom())
    }

    private val hostVerifier = HostnameVerifier { hostname, _ ->
        hostname == ip
    }

    suspend fun get(path: String, timeoutMs: Int = 5000): ApiResult =
        request("GET", path, null, timeoutMs)

    suspend fun post(path: String, body: JSONObject, timeoutMs: Int = 8000): ApiResult =
        request("POST", path, body.toString(), timeoutMs)

    suspend fun getStatus(): ApiResult = get("/api/status")
    suspend fun getDevices(): ApiResult = get("/api/devices", timeoutMs = 7000)
    suspend fun getDiagnostics(): ApiResult = get("/api/gateway/diagnostics", timeoutMs = 6000)

    suspend fun controlRelay(node: Long, endpoint: Int, on: Boolean): ApiResult {
        val body = JSONObject().apply {
            put("node", node)
            put("endpoint", endpoint)
            put("on", on)
        }
        return post("/api/device/control", body, timeoutMs = 10000)
    }

    suspend fun pairMatter(
        code: String,
        network: String,
        ssid: String? = null,
        password: String? = null
    ): ApiResult {
        val body = JSONObject().apply {
            put("code", code.trim())
            put("network", network)
            if (network == "wifi") {
                put("ssid", ssid ?: "")
                put("password", password ?: "")
            }
        }
        return post("/api/matter/pair", body, timeoutMs = 10000)
    }

    suspend fun rebootGateway(): ApiResult = post("/api/system/reboot", JSONObject())

    private suspend fun request(
        method: String,
        path: String,
        body: String?,
        timeoutMs: Int
    ): ApiResult = withContext(Dispatchers.IO) {
        var conn: HttpsURLConnection? = null
        try {
            val url = URL("https://$ip$path")
            conn = (url.openConnection() as HttpsURLConnection).apply {
                sslSocketFactory = sslContext.socketFactory
                hostnameVerifier = hostVerifier
                requestMethod = method
                connectTimeout = timeoutMs
                readTimeout = timeoutMs
                useCaches = false
                setRequestProperty("Host", ip)
                setRequestProperty("Origin", "https://$ip")
                setRequestProperty("Authorization", "Bearer $credential")
                setRequestProperty("Accept", "application/json")
                setRequestProperty("Connection", "close")
                if (body != null) {
                    setRequestProperty("Content-Type", "application/json")
                    doOutput = true
                }
            }

            if (body != null) {
                conn.outputStream.use { os ->
                    os.write(body.toByteArray(Charsets.UTF_8))
                    os.flush()
                }
            }

            val statusCode = conn.responseCode
            val stream = if (statusCode in 200..299) conn.inputStream else conn.errorStream
            val raw = if (stream != null) {
                BufferedReader(InputStreamReader(stream, Charsets.UTF_8)).use { it.readText() }
            } else {
                ""
            }
            val parsed = try {
                if (raw.trim().startsWith("{")) JSONObject(raw.trim()) else null
            } catch (_: Exception) {
                null
            }
            ApiResult(code = statusCode, json = parsed, rawBody = raw)
        } catch (e: Exception) {
            ApiResult(
                code = -1,
                json = null,
                rawBody = "",
                error = e.message ?: e.javaClass.simpleName
            )
        } finally {
            conn?.disconnect()
        }
    }
}
