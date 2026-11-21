// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// Local HTTP server that replays queued responses (fixtures built on the
// structure of the contract, with values of our own).

#ifndef FAKESKODASERVER_H
#define FAKESKODASERVER_H

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QPair>
#include <QQueue>
#include <QTcpServer>
#include <QUrl>

struct FakeResponse
{
    int status = 200;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray body;
    bool dropConnection = false; // close the socket without an answer
};

struct RecordedRequest
{
    QByteArray method;
    QByteArray target; // path and query, as sent
    QMap<QByteArray, QByteArray> headers; // lower-case names
};

class FakeSkodaServer : public QObject
{
    Q_OBJECT
public:
    explicit FakeSkodaServer(QObject *parent = nullptr);

    bool start();
    QUrl baseUrl() const;

    void enqueue(const FakeResponse &response);
    int pendingResponses() const;
    QList<RecordedRequest> requests() const;
    int requestCount() const;

signals:
    void requestServed();

private:
    void onNewConnection();

    QTcpServer m_server;
    QQueue<FakeResponse> m_queue;
    QList<RecordedRequest> m_requests;
};

#endif // FAKESKODASERVER_H
