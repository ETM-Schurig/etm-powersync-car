// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodaresponse.h"
#include "skodalogging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <cmath>
#include <limits>

namespace {

std::optional<int> headerInt(const QByteArray &value)
{
    bool ok = false;
    const qlonglong number = value.trimmed().toLongLong(&ok);
    if (!ok || number < 0 || number > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(number);
}

// An integer member. Absent, null, not a number or not integral: unknown.
std::optional<int> jsonInt(const QJsonObject &object, const QString &name, const char *path)
{
    if (!object.contains(name) || object.value(name).isNull())
        return std::nullopt;
    const QJsonValue value = object.value(name);
    if (!value.isDouble()) {
        qCWarning(dcSkodaCore()) << "Field" << path << "is not a number, treated as unknown";
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (std::floor(number) != number || std::abs(number) > std::numeric_limits<int>::max()) {
        qCWarning(dcSkodaCore()) << "Field" << path << "is not an integer, treated as unknown";
        return std::nullopt;
    }
    return static_cast<int>(number);
}

std::optional<QString> jsonString(const QJsonObject &object, const QString &name, const char *path)
{
    if (!object.contains(name) || object.value(name).isNull())
        return std::nullopt;
    const QJsonValue value = object.value(name);
    if (!value.isString()) {
        qCWarning(dcSkodaCore()) << "Field" << path << "is not a string, treated as unknown";
        return std::nullopt;
    }
    return value.toString();
}

std::optional<int> percent(std::optional<int> value, const char *path)
{
    if (value && (*value < 0 || *value > 100)) {
        qCWarning(dcSkodaCore()) << "Field" << path << "is outside 0-100:" << *value << "- treated as unknown";
        return std::nullopt;
    }
    return value;
}

}

std::optional<QDateTime> SkodaParser::parseDateTime(const QString &value)
{
    QDateTime dateTime = QDateTime::fromString(value.trimmed(), Qt::ISODateWithMs);
    if (!dateTime.isValid())
        dateTime = QDateTime::fromString(value.trimmed(), Qt::ISODate);
    if (!dateTime.isValid())
        return std::nullopt;
    return dateTime.toUTC();
}

void SkodaParser::parseHeaders(SkodaResponse &response, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    for (const auto &header : headers) {
        const QByteArray name = header.first.toLower();
        if (name == "x-api-key-expires-at") {
            response.apiKeyExpiresAt = parseDateTime(QString::fromLatin1(header.second));
        } else if (name == "ratelimit-limit") {
            response.rateLimitLimit = headerInt(header.second);
        } else if (name == "ratelimit-remaining") {
            response.rateLimitRemaining = headerInt(header.second);
        } else if (name == "ratelimit-reset") {
            response.rateLimitReset = headerInt(header.second);
        } else if (name == "retry-after") {
            // The contract defines Retry-After as a number of seconds.
            response.retryAfter = headerInt(header.second);
        }
    }
}

bool SkodaParser::parseVehicleBody(SkodaResponse &response, const QByteArray &body)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        response.errorString = QStringLiteral("unreadable JSON body: %1").arg(error.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("vehicle")).isObject()) {
        response.errorString = QStringLiteral("response without a \"vehicle\" object");
        return false;
    }
    const QJsonObject vehicle = root.value(QStringLiteral("vehicle")).toObject();
    response.vin = vehicle.value(QStringLiteral("vin")).toString();

    // errors: informative list (VehicleError), may be absent.
    const QJsonArray errors = root.value(QStringLiteral("errors")).toArray();
    for (const QJsonValue &value : errors) {
        const QJsonObject object = value.toObject();
        SkodaVehicleError vehicleError;
        vehicleError.type = object.value(QStringLiteral("type")).toString();
        vehicleError.description = object.value(QStringLiteral("description")).toString();
        response.errors.append(vehicleError);
    }

    SkodaChargingReading &reading = response.charging;
    if (!vehicle.value(QStringLiteral("charging")).isObject())
        return true;

    const QJsonObject charging = vehicle.value(QStringLiteral("charging")).toObject();
    reading.present = true;

    if (auto timestamp = jsonString(charging, QStringLiteral("carCapturedTimestamp"), "charging.carCapturedTimestamp")) {
        reading.carCapturedTimestamp = parseDateTime(*timestamp);
        if (!reading.carCapturedTimestamp)
            qCWarning(dcSkodaCore()) << "Field charging.carCapturedTimestamp is not a date-time, treated as unknown";
    }

    const QJsonObject status = charging.value(QStringLiteral("status")).toObject();
    const QJsonObject battery = status.value(QStringLiteral("battery")).toObject();

    reading.stateOfChargeInPercent = percent(
        jsonInt(battery, QStringLiteral("stateOfChargeInPercent"), "charging.status.battery.stateOfChargeInPercent"),
        "charging.status.battery.stateOfChargeInPercent");
    reading.plugConnectionState = jsonString(status, QStringLiteral("plugConnectionState"), "charging.status.plugConnectionState");
    return true;
}

void SkodaParser::parseProblemBody(SkodaResponse &response, const QByteArray &body)
{
    const QJsonObject problem = QJsonDocument::fromJson(body).object();
    QString type = problem.value(QStringLiteral("type")).toString();
    if (type.isEmpty() && !problem.isEmpty())
        type = QStringLiteral("about:blank"); // RFC 9457 default
    // https://public.api.connect.skoda-auto.cz/problems/api-key-expired -> api-key-expired
    const int slash = type.lastIndexOf(QLatin1Char('/'));
    response.problemType = slash >= 0 ? type.mid(slash + 1) : type;
    response.problemDetail = problem.value(QStringLiteral("detail")).toString();
}
