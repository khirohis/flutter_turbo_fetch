package io.github.khirohis.flutter_turbo_fetch

import java.net.URI
import java.net.URISyntaxException
import java.util.Locale

internal data class ValidatedCronetRequest(
    val url: String,
    val method: String,
    val headers: List<Pair<String, String>>,
    val body: ByteArray?
)

/**
 * Pre-flight validator for Cronet HTTP requests (method, http/https URL, RFC header characters,
 * and mandatory Content-Type for non-empty bodies).
 */
internal object CronetRequestValidator {
    fun validateAndNormalize(request: TurboHttpRequest): ValidatedCronetRequest {
        val method = request.method.trim().uppercase(Locale.ROOT)
        if (method.isEmpty()) {
            throw IllegalArgumentException("HTTP method must not be blank.")
        }

        val uri = try {
            URI(request.url)
        } catch (e: URISyntaxException) {
            throw IllegalArgumentException("Malformed request URL: ${e.message}", e)
        }
        val scheme = uri.scheme?.lowercase(Locale.ROOT)
        if (scheme != "http" && scheme != "https") {
            throw IllegalArgumentException("Unsupported URL scheme: '${uri.scheme}'. Only http and https are supported.")
        }
        if (uri.host.isNullOrBlank()) {
            throw IllegalArgumentException("Request URL must specify a non-empty host.")
        }

        val validatedHeaders = ArrayList<Pair<String, String>>(request.headers.size)
        var hasNonBlankContentType = false
        for ((rawKey, rawValue) in request.headers) {
            if (rawKey == null || rawValue == null) {
                continue
            }
            validateHeaderName(rawKey)
            validateHeaderValue(rawKey, rawValue)
            if (rawKey.equals("content-type", ignoreCase = true) && rawValue.isNotBlank()) {
                hasNonBlankContentType = true
            }
            validatedHeaders.add(rawKey to rawValue)
        }

        val rawBody = request.body
        val nonEmptyBody = if (rawBody != null && rawBody.isNotEmpty()) {
            if (!hasNonBlankContentType) {
                throw IllegalArgumentException(
                    "Non-empty request body requires a Content-Type header when using the Android Cronet engine."
                )
            }
            rawBody
        } else {
            null
        }

        return ValidatedCronetRequest(
            url = request.url,
            method = method,
            headers = validatedHeaders,
            body = nonEmptyBody
        )
    }

    private fun validateHeaderName(name: String) {
        if (name.isEmpty()) {
            throw IllegalArgumentException("Header name must not be empty.")
        }
        for (ch in name) {
            if (ch !in '\u0021'..'\u007e' || ch == ':') {
                throw IllegalArgumentException("Invalid character in HTTP header name: '$name'.")
            }
        }
    }

    private fun validateHeaderValue(name: String, value: String) {
        for (ch in value) {
            if (ch != '\t' && ch !in '\u0020'..'\u007e') {
                throw IllegalArgumentException("Invalid character in HTTP header value for '$name'.")
            }
        }
    }
}
