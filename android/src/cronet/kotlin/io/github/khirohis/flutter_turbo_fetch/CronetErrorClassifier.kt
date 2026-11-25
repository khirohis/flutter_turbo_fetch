package io.github.khirohis.flutter_turbo_fetch

import org.chromium.net.NetworkException

internal enum class RequestPhase {
    CONNECTING,
    SENDING_BODY,
    RECEIVING
}

/**
 * Maps Cronet [NetworkException] and Chromium internal error codes to shared turbo-fetch error codes.
 */
internal object CronetErrorClassifier {
    // Chromium net_error_list.h SSL/TLS handshake error codes outside -200..-299.
    private val SSL_HANDSHAKE_INTERNAL_ERROR_CODES = setOf(
        -107, // ERR_SSL_PROTOCOL_ERROR
        -110, // ERR_SSL_CLIENT_AUTH_CERT_NEEDED
        -113, // ERR_SSL_VERSION_OR_CIPHER_MISMATCH
        -114, // ERR_SSL_RENEGOTIATION_REQUESTED
        -117, // ERR_BAD_SSL_CLIENT_AUTH_CERT
        -125, // ERR_SSL_UNRECOGNIZED_NAME_ALERT
        -126, // ERR_SSL_SERVER_CERT_BAD_FORMAT
        -148, // ERR_SSL_OBSOLETE_CIPHER
        -150, // ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN
        -156  // ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED
    )

    fun classify(e: NetworkException, phase: RequestPhase): String {
        val errorCode = e.errorCode
        if (errorCode == NetworkException.ERROR_CONNECTION_TIMED_OUT) {
            return "connectionTimeout"
        }
        if (errorCode == NetworkException.ERROR_TIMED_OUT) {
            return when (phase) {
                RequestPhase.CONNECTING -> "connectionTimeout"
                RequestPhase.SENDING_BODY -> "sendTimeout"
                RequestPhase.RECEIVING -> "receiveTimeout"
            }
        }

        val internalCode = e.cronetInternalErrorCode
        if (internalCode in -299..-200 || internalCode in SSL_HANDSHAKE_INTERNAL_ERROR_CODES) {
            return "badCertificate"
        }

        return when (errorCode) {
            NetworkException.ERROR_HOSTNAME_NOT_RESOLVED,
            NetworkException.ERROR_INTERNET_DISCONNECTED,
            NetworkException.ERROR_NETWORK_CHANGED,
            NetworkException.ERROR_CONNECTION_CLOSED,
            NetworkException.ERROR_CONNECTION_REFUSED,
            NetworkException.ERROR_CONNECTION_RESET,
            NetworkException.ERROR_ADDRESS_UNREACHABLE,
            NetworkException.ERROR_QUIC_PROTOCOL_FAILED,
            NetworkException.ERROR_OTHER -> "connectionError"
            else -> "unknown"
        }
    }
}
