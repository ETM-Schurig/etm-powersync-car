// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodaapiclient.h"
#include "skodalogging.h"

#include <QHostAddress>
#include <QMetaEnum>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrlQuery>

const QUrl SkodaApiClient::productionBaseUrl()
{
    return QUrl(QStringLiteral("https://public.api.connect.skoda-auto.cz"));
}

const QString SkodaApiClient::includeParts()
{
    return QStringLiteral("charging");
}

SkodaApiClient::SkodaApiClient(const GetFunction &get, const QUrl &baseUrl, const QString &vin,
                               const QString &apiKey, QObject *parent)
    : QObject(parent),
      m_get(get),
      m_baseUrl(baseUrl),
      m_vin(vin),
      m_apiKey(apiKey),
      m_clock([]() { return QDateTime::currentDateTimeUtc(); })
{
}

void SkodaApiClient::setClock(const Clock &clock)
{
    m_clock = clock;
}

void SkodaApiClient::setApiKey(const QString &apiKey)
{
    m_apiKey = apiKey;
}

QString SkodaApiClient::vin() const
{
    return m_vin;
}

QString SkodaApiClient::maskedApiKey() const
{
    return maskApiKey(m_apiKey);
}

bool SkodaApiClient::busy() const
{
    return m_reply != nullptr;
}

QString SkodaApiClient::maskApiKey(const QString &apiKey)
{
    // Short keys are not shown at all: 4 characters of an 8-character key is half of it.
    if (apiKey.size() < 12)
        return QStringLiteral("…");
    return QStringLiteral("…") + apiKey.right(4);
}

bool SkodaApiClient::isAllowedBaseUrl(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return false;
    if (url.scheme() == QLatin1String("https"))
        return true;
    if (url.scheme() == QLatin1String("http")) {
        const QHostAddress address(url.host());
        return url.host() == QLatin1String("localhost") || (!address.isNull() && address.isLoopback());
    }
    return false;
}

QString SkodaApiClient::networkErrorName(int error)
{
    // Code name only: QNetworkReply::errorString() quotes the URL, which holds the VIN.
    const char *name = QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(error);
    return QStringLiteral("%1 (%2)").arg(name ? QString::fromLatin1(name) : QStringLiteral("NetworkError")).arg(error);
}

bool SkodaApiClient::isValidVin(const QString &vin)
{
    // ISO 3779: 17 characters, digits and capital letters except I, O and Q.
    static const QRegularExpression pattern(QStringLiteral("^[A-HJ-NPR-Z0-9]{17}$"));
    return pattern.match(vin).hasMatch();
}

QUrl SkodaApiClient::vehicleUrl() const
{
    QUrl url = m_baseUrl;
    url.setPath(QStringLiteral("/api/v1/vehicles/") + m_vin);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("include"), includeParts());
    url.setQuery(query);
    return url;
}

bool SkodaApiClient::fetchVehicle()
{
    if (m_reply)
        return false;

    if (!isAllowedBaseUrl(m_baseUrl)) {
        qCCritical(dcSkodaCore()) << "Refusing to contact" << m_baseUrl.toString() << "- only https is allowed";
        return false;
    }

    QNetworkRequest request(vehicleUrl());
    request.setRawHeader("X-API-Key", m_apiKey.toUtf8());
    request.setRawHeader("Accept", "application/json, application/problem+json");
    // A redirect could carry the key to another host (SK-604): never follow one.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    // TLS: Qt's default configuration verifies the peer; it is never changed here (SK-602).

    QNetworkReply *reply = m_get(request);
    if (!reply) {
        qCWarning(dcSkodaCore()) << "The network manager returned no reply";
        return false;
    }
    m_reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() { onFinished(reply); });
    return true;
}

void SkodaApiClient::onFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;

    SkodaResponse response;
    response.receivedAt = m_clock();

    const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const QByteArray body = reply->readAll();

    if (!statusAttribute.isValid()) {
        response.kind = SkodaResponse::KindNetworkError;
        response.networkError = reply->error();
        response.errorString = networkErrorName(reply->error());
        emit vehicleFetched(response);
        return;
    }

    response.httpStatus = statusAttribute.toInt();
    SkodaParser::parseHeaders(response, reply->rawHeaderPairs());

    if (response.httpStatus == 200) {
        if (SkodaParser::parseVehicleBody(response, body)) {
            response.kind = SkodaResponse::KindOk;
        } else {
            response.kind = SkodaResponse::KindHttpError;
        }
    } else {
        response.kind = SkodaResponse::KindHttpError;
        SkodaParser::parseProblemBody(response, body);
    }
    emit vehicleFetched(response);
}
