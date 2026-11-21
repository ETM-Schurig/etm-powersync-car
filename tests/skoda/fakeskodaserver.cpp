// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "fakeskodaserver.h"

#include <QHostAddress>
#include <QTcpSocket>

namespace {

QByteArray reasonPhrase(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Status";
    }
}

}

FakeSkodaServer::FakeSkodaServer(QObject *parent)
    : QObject(parent)
{
    connect(&m_server, &QTcpServer::newConnection, this, &FakeSkodaServer::onNewConnection);
}

bool FakeSkodaServer::start()
{
    return m_server.listen(QHostAddress::LocalHost, 0);
}

QUrl FakeSkodaServer::baseUrl() const
{
    return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
}

void FakeSkodaServer::enqueue(const FakeResponse &response)
{
    m_queue.enqueue(response);
}

int FakeSkodaServer::pendingResponses() const
{
    return m_queue.size();
}

QList<RecordedRequest> FakeSkodaServer::requests() const
{
    return m_requests;
}

int FakeSkodaServer::requestCount() const
{
    return m_requests.size();
}

void FakeSkodaServer::onNewConnection()
{
    while (QTcpSocket *socket = m_server.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;

            RecordedRequest request;
            const QList<QByteArray> lines = buffer->left(end).split('\n');
            const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
            request.method = requestLine.value(0);
            request.target = requestLine.value(1);
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray line = lines.at(i).trimmed();
                const int colon = line.indexOf(':');
                if (colon > 0)
                    request.headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
            }
            buffer->clear();
            m_requests.append(request);

            if (m_queue.isEmpty()) {
                // Not the target: it holds the VIN, and vinNeverInLogs checks every captured line.
                qWarning() << "FakeSkodaServer: no response queued for request" << m_requests.size() << request.method;
                socket->disconnectFromHost();
                emit requestServed();
                return;
            }
            const FakeResponse response = m_queue.dequeue();
            if (response.dropConnection) {
                socket->abort();
                emit requestServed();
                return;
            }
            QByteArray data = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' + reasonPhrase(response.status) + "\r\n";
            bool hasContentType = false;
            for (const auto &header : response.headers) {
                if (header.first.toLower() == "content-type")
                    hasContentType = true;
                data += header.first + ": " + header.second + "\r\n";
            }
            if (!hasContentType)
                data += response.status == 200 ? "Content-Type: application/json\r\n" : "Content-Type: application/problem+json\r\n";
            data += "Content-Length: " + QByteArray::number(response.body.size()) + "\r\n";
            data += "Connection: close\r\n\r\n";
            data += response.body;
            socket->write(data);
            socket->flush();
            socket->disconnectFromHost();
            emit requestServed();
        });
    }
}
