// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Parsed answer of GET /api/v1/vehicles/{vin}?include=charging.
// Every field name comes from the MyŠkoda contract 1.1.0 (SK-101, docs/skoda/README.md).
// A field that is absent from the response stays empty (std::optional):
// it is unknown, never 0 (SK-307).

#ifndef SKODARESPONSE_H
#define SKODARESPONSE_H

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMetaType>
#include <QString>

#include <optional>

struct SkodaVehicleError
{
    QString type;         // VehicleError.type, e.g. CHARGING_UNAVAILABLE
    QString description;  // VehicleError.description

    bool operator==(const SkodaVehicleError &other) const {
        return type == other.type && description == other.description;
    }
};

// vehicle.charging, read field by field. Only what the plugin publishes is
// read (SK-603, data minimisation): the SoC, its timestamp and the plug state.
// Every other member of the answer is ignored.
struct SkodaChargingReading
{
    bool present = false;                          // vehicle.charging is in the response
    std::optional<QDateTime> carCapturedTimestamp; // charging.carCapturedTimestamp
    std::optional<int> stateOfChargeInPercent;     // charging.status.battery.stateOfChargeInPercent
    std::optional<QString> plugConnectionState;    // charging.status.plugConnectionState
};

struct SkodaResponse
{
    enum Kind {
        KindOk,            // HTTP 200 with a readable body
        KindHttpError,     // HTTP status other than 200, or 200 with an unreadable body
        KindNetworkError,  // no HTTP status (DNS, TLS, timeout, connection refused, ...)
        KindRefused        // the client refused to send the request (bad URL)
    };

    Kind kind = KindNetworkError;
    int httpStatus = 0;
    QDateTime receivedAt;

    // Headers (absent header: empty optional).
    std::optional<QDateTime> apiKeyExpiresAt; // X-API-Key-Expires-At
    std::optional<int> rateLimitLimit;        // RateLimit-Limit
    std::optional<int> rateLimitRemaining;    // RateLimit-Remaining
    std::optional<int> rateLimitReset;        // RateLimit-Reset (seconds)
    std::optional<int> retryAfter;            // Retry-After (seconds)

    // application/problem+json (RFC 9457).
    QString problemType;   // last segment of the type URI ("api-key-expired") or "about:blank"
    QString problemDetail;

    // 200 body.
    QString vin;
    SkodaChargingReading charging;
    QList<SkodaVehicleError> errors;

    // For the log. Network error: name of the QNetworkReply::NetworkError code,
    // never QNetworkReply::errorString(), which quotes the URL and so the VIN.
    QString errorString;
    int networkError = 0; // QNetworkReply::NetworkError, KindNetworkError only

    bool isOk() const { return kind == KindOk; }
    bool isServerError() const { return kind == KindHttpError && httpStatus >= 500; }
};

Q_DECLARE_METATYPE(SkodaResponse)

namespace SkodaParser {

// Fills the fields of the response that come from the headers.
void parseHeaders(SkodaResponse &response, const QList<QPair<QByteArray, QByteArray>> &headers);

// Parses a 200 body. Returns false when the body is not a JSON object with a
// "vehicle" object (the only required member of VehicleResponse).
bool parseVehicleBody(SkodaResponse &response, const QByteArray &body);

// Parses a problem+json body. Never fails: an unreadable body leaves the
// problem fields empty.
void parseProblemBody(SkodaResponse &response, const QByteArray &body);

// Reads an RFC 3339 date-time. Invalid input gives an empty optional.
std::optional<QDateTime> parseDateTime(const QString &value);

}

#endif // SKODARESPONSE_H
