package pl.matter2supla.finder

internal class LivePollRetry {
    private var failures = 0
    private val delays = longArrayOf(5_000, 10_000, 30_000, 60_000)

    fun nextDelay(ok: Boolean, error: ApiError, httpCode: Int, retryAfterMs: Long): Long {
        if (ok) failures = 0
        if (httpCode == 429) return retryAfterMs.takeIf { it > 0 } ?: delays.last()
        if (error == ApiError.NETWORK || error == ApiError.TIMEOUT || httpCode in 500..599) {
            val wait = delays[failures]
            failures = (failures + 1).coerceAtMost(delays.lastIndex)
            return wait
        }
        return delays.first()
    }
}
