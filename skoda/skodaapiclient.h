// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// HTTP client of the MyŠkoda Public API, read-only: it only sends
// GET /api/v1/vehicles/{vin}?include=charging (SK-204, SK-603).

#ifndef SKODAAPICLIENT_H
#define SKODAAPICLIENT_H

#include <QObject>
#include <QUrl>

#include <functional>

#include "skodaresponse.h"

class QNetworkReply;
class QNetworkRequest;

class SkodaApiClient : public QObject
{
    Q_OBJECT
public:
    // The only host the plugin contacts (SK-604).
    static const QUrl productionBaseUrl();

    // The only part of the vehicle data the plugin asks for (SK-204, SK-603):
    // the charging part holds the SoC, the target, the range and the plug state.
    static const QString includeParts();

    // Sends a GET request and returns the reply. In the plugin this is
    // nymea's NetworkAccessManager; in the tests a QNetworkAccessManager.
    using GetFunction = std::function<QNetworkReply *(const QNetworkRequest &)>;
    using Clock = std::function<QDateTime()>;

    // baseUrl must be https (certificate verification stays on, SK-602).
    // Plain http is accepted only towards a loopback address, for the tests.
    SkodaApiClient(const GetFunction &get, const QUrl &baseUrl, const QString &vin,
                   const QString &apiKey, QObject *parent = nullptr);

    void setClock(const Clock &clock);
    void setApiKey(const QString &apiKey);

    QString vin() const;
    QString maskedApiKey() const;
    bool busy() const;

    // Sends one request. Returns false when a request is already running or
    // when the base URL was refused; vehicleFetched() follows otherwise.
    bool fetchVehicle();

    QUrl vehicleUrl() const;

    // "…ABCD": at most the last 4 characters of the key (SK-401).
    static QString maskApiKey(const QString &apiKey);
    static bool isAllowedBaseUrl(const QUrl &url);
    // "RemoteHostClosedError (2)": the name of a QNetworkReply::NetworkError
    // code, safe for the log (no URL, no VIN).
    static QString networkErrorName(int error);
    static bool isValidVin(const QString &vin);

signals:
    void vehicleFetched(const SkodaResponse &response);

private:
    void onFinished(QNetworkReply *reply);

    GetFunction m_get;
    QUrl m_baseUrl;
    QString m_vin;
    QString m_apiKey;
    Clock m_clock;
    QNetworkReply *m_reply = nullptr;
};

#endif // SKODAAPICLIENT_H
