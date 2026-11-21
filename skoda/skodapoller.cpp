// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM

#include "skodapoller.h"
#include "skodaapiclient.h"
#include "skodalogging.h"

#include <QTimeZone>

#include <algorithm>

namespace {

QDateTime later(const QDateTime &a, const QDateTime &b)
{
    if (!a.isValid())
        return b;
    if (!b.isValid())
        return a;
    return a > b ? a : b;
}

}

SkodaPoller::SkodaPoller(SkodaApiClient *client, QObject *parent)
    : QObject(parent),
      m_client(client),
      m_clock([]() { return QDateTime::currentDateTimeUtc(); })
{
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (!pollNow())
            armTimer();
    });
    connect(m_client, &SkodaApiClient::vehicleFetched, this, &SkodaPoller::onVehicleFetched);
}

void SkodaPoller::setClock(const Clock &clock)
{
    m_clock = clock;
}

QString SkodaPoller::intervalError(int minutes)
{
    if (minutes < pluggedIntervalMinMinutes || minutes > pluggedIntervalMaxMinutes)
        return QStringLiteral("L'intervalle de lecture voiture branchée doit être compris entre %1 et %2 min.")
            .arg(pluggedIntervalMinMinutes).arg(pluggedIntervalMaxMinutes);
    return QString();
}

bool SkodaPoller::setPluggedInterval(int minutes)
{
    if (!intervalError(minutes).isEmpty())
        return false;
    if (m_pluggedInterval == minutes * 60)
        return true;
    m_pluggedInterval = minutes * 60;
    if (m_planned && m_planIsInterval && m_lastReadAt.isValid()) {
        m_plannedAt = m_lastReadAt.addSecs(m_pluggedInterval);
        m_reason = QStringLiteral("%1, %2 min interval (setting changed)")
                       .arg(m_plug == Plug::Connected ? QStringLiteral("vehicle plugged in") : QStringLiteral("plug state unknown"))
                       .arg(m_pluggedInterval / 60);
        logSchedule();
        armTimer();
    }
    return true;
}

void SkodaPoller::setLastRequestAt(const QDateTime &lastRequestAt)
{
    // A time in the future (clock set back since) counts as now.
    const QDateTime now = m_clock();
    m_lastRequestAt = lastRequestAt.isValid() && lastRequestAt > now ? now : lastRequestAt;
}

QDateTime SkodaPoller::lastRequestAt() const
{
    return m_lastRequestAt;
}

void SkodaPoller::start()
{
    m_running = true;
    if (!pollNow())
        armTimer();
}

void SkodaPoller::stop()
{
    m_running = false;
    m_timer.stop();
}

QDateTime SkodaPoller::earliestAllowed() const
{
    const QDateTime gap = m_lastRequestAt.isValid() ? m_lastRequestAt.addSecs(minimumGapSeconds) : QDateTime();
    return later(gap, m_blockedUntil);
}

bool SkodaPoller::pollNow()
{
    if (m_halted) {
        qCDebug(dcSkodaCore()) << "No request: polling is halted -" << m_reason;
        return false;
    }
    if (m_client->busy()) {
        qCDebug(dcSkodaCore()) << "No request: a request is already running";
        return false;
    }
    if (!m_planned) {
        qCDebug(dcSkodaCore()) << "No request: none planned -" << m_reason;
        return false;
    }
    const QDateTime now = m_clock();
    const QDateTime earliest = notBefore();
    if (earliest.isValid() && now < earliest) {
        const QDateTime gap = m_lastRequestAt.isValid() ? m_lastRequestAt.addSecs(minimumGapSeconds) : QDateTime();
        const bool byGap = gap.isValid() && gap == earliest && gap > later(m_plannedAt, m_blockedUntil);
        qCDebug(dcSkodaCore()) << "No request before" << earliest.toString(Qt::ISODate) << "-"
                               << (byGap ? QStringLiteral("minimum gap of %1 s since the last request").arg(minimumGapSeconds) : m_reason);
        return false;
    }
    return send(now);
}

bool SkodaPoller::send(const QDateTime &now)
{
    if (!m_client->fetchVehicle()) {
        setHalted(true, QStringLiteral("the request could not be sent"));
        return false;
    }
    m_lastRequestAt = now;
    emit requestSent(now);
    m_timer.stop();
    return true;
}

SkodaPoller::RefreshResult SkodaPoller::refresh(QString *refusal)
{
    if (m_halted) {
        if (refusal)
            *refusal = QStringLiteral("Lecture impossible : les lectures sont arrêtées jusqu'à une nouvelle clé API ou une reconfiguration du véhicule.");
        qCInfo(dcSkodaCore()) << "Refresh refused: polling is halted -" << m_reason;
        return RefreshResult::Refused;
    }
    if (m_client->busy()) {
        qCInfo(dcSkodaCore()) << "Refresh: a request is already running, its answer serves";
        return RefreshResult::Busy;
    }
    const QDateTime now = m_clock();
    const QDateTime earliest = earliestAllowed();
    if (earliest.isValid() && now < earliest) {
        const QDateTime gap = m_lastRequestAt.isValid() ? m_lastRequestAt.addSecs(minimumGapSeconds) : QDateTime();
        QString cause;
        if (gap.isValid() && gap >= earliest) {
            cause = QStringLiteral("moins de %1 min depuis la dernière requête").arg(minimumGapSeconds / 60);
        } else {
            switch (m_block) {
            case Block::Backoff:
                cause = QStringLiteral("le service MyŠkoda vient d'échouer, nouvelle tentative différée");
                break;
            case Block::RateLimit:
                cause = QStringLiteral("le service MyŠkoda a demandé d'attendre (HTTP 429)");
                break;
            case Block::Quota:
                cause = QStringLiteral("réserve du quota de requêtes MyŠkoda atteinte");
                break;
            case Block::None:
                cause = QStringLiteral("lecture différée");
                break;
            }
        }
        // Rounded up to the minute, in the local time of the box.
        const qint64 seconds = (earliest.toSecsSinceEpoch() + 59) / 60 * 60;
        const QString at = QDateTime::fromSecsSinceEpoch(seconds, QTimeZone::UTC).toLocalTime().toString(QStringLiteral("HH:mm"));
        if (refusal)
            *refusal = QStringLiteral("Lecture refusée : %1. Prochaine lecture possible à %2.").arg(cause, at);
        qCInfo(dcSkodaCore()) << "Refresh refused until" << earliest.toString(Qt::ISODate) << "-" << cause;
        return RefreshResult::Refused;
    }
    qCInfo(dcSkodaCore()) << "Refresh: reading now";
    if (!send(now)) {
        if (refusal)
            *refusal = QStringLiteral("Lecture impossible : la requête n'a pas pu être envoyée.");
        return RefreshResult::Refused;
    }
    return RefreshResult::Sent;
}

void SkodaPoller::acceptResponse(const SkodaResponse &response)
{
    qCInfo(dcSkodaCore()) << "Using the pairing validation answer as the first reading";
    onVehicleFetched(response);
}

bool SkodaPoller::usableAsFirstReading(const SkodaResponse &response)
{
    return response.isOk() && response.charging.present;
}

void SkodaPoller::resume()
{
    m_consecutiveFailures = 0;
    m_lastBackoffSeconds = 0;
    m_rateLimitEpisode = false;
    m_blockedUntil = QDateTime();
    m_block = Block::None;
    m_planned = true;
    m_plannedAt = QDateTime();
    m_planIsInterval = false;
    setHalted(false, QStringLiteral("new API key"));
    if (m_running && !pollNow())
        armTimer();
}

bool SkodaPoller::halted() const
{
    return m_halted;
}

bool SkodaPoller::hasPlannedRequest() const
{
    return m_planned && !m_halted;
}

QDateTime SkodaPoller::notBefore() const
{
    if (!m_planned)
        return QDateTime();
    return later(m_plannedAt, earliestAllowed());
}

int SkodaPoller::backoffSeconds() const
{
    return m_lastBackoffSeconds;
}

QString SkodaPoller::scheduleReason() const
{
    return m_reason;
}

int SkodaPoller::backoffSecondsForFailure(int consecutiveFailures)
{
    static const int minutes[] = {5, 10, 20, 30};
    const int index = std::clamp(consecutiveFailures - 1, 0, 3);
    return minutes[index] * 60;
}

void SkodaPoller::onVehicleFetched(const SkodaResponse &response)
{
    emit responseReceived(response);

    const QDateTime now = m_clock();

    if (response.isOk()) {
        // An absent plug state keeps the last known one (SK-307); a value the
        // contract does not list is unknown, polled like a plugged-in vehicle.
        if (response.charging.present && response.charging.plugConnectionState) {
            const QString &plug = *response.charging.plugConnectionState;
            m_plug = plug == QLatin1String("CONNECTED") ? Plug::Connected
                   : plug == QLatin1String("DISCONNECTED") ? Plug::Disconnected
                   : Plug::Unknown;
        }
        m_consecutiveFailures = 0;
        m_lastBackoffSeconds = 0;
        if (m_rateLimitEpisode)
            qCInfo(dcSkodaCore()) << "Rate limit episode over";
        m_rateLimitEpisode = false;
        m_blockedUntil = QDateTime();
        m_block = Block::None;
        m_lastReadAt = now;
        m_planIsInterval = m_plug != Plug::Disconnected;
        if (m_plug == Plug::Disconnected) {
            // SK-200: no periodic reading of an unplugged vehicle.
            m_planned = false;
            m_plannedAt = QDateTime();
            m_reason = QStringLiteral("vehicle unplugged, no periodic reading (refresh only)");
        } else {
            m_planned = true;
            m_plannedAt = now.addSecs(m_pluggedInterval);
            m_reason = QStringLiteral("%1, %2 min interval")
                           .arg(m_plug == Plug::Connected ? QStringLiteral("vehicle plugged in") : QStringLiteral("plug state unknown"))
                           .arg(m_pluggedInterval / 60);
        }
    } else if (response.kind == SkodaResponse::KindHttpError
               && (response.httpStatus == 401 || response.httpStatus == 403 || response.httpStatus == 404)) {
        // SK-103, SK-402: no further request until a new key or a reconfiguration.
        m_planned = false;
        setHalted(true, QStringLiteral("HTTP %1 %2").arg(response.httpStatus).arg(response.problemType));
        return;
    } else if (response.kind == SkodaResponse::KindHttpError && response.httpStatus == 429) {
        // SK-202: nothing before Retry-After, then the read is retried.
        int wait = rateLimitFallbackSeconds;
        if (response.retryAfter)
            wait = *response.retryAfter;
        else if (response.rateLimitReset)
            wait = *response.rateLimitReset;
        wait = std::max(wait, minimumGapSeconds);
        m_blockedUntil = now.addSecs(wait);
        m_block = Block::RateLimit;
        m_planned = true;
        m_planIsInterval = false;
        m_plannedAt = m_blockedUntil;
        m_reason = QStringLiteral("HTTP 429 %1, waiting %2 s").arg(response.problemType).arg(wait);
        if (!m_rateLimitEpisode) {
            qCWarning(dcSkodaCore()) << "Rate limit episode started: HTTP 429" << response.problemType << "- no request for" << wait << "s";
            m_rateLimitEpisode = true;
        }
    } else {
        // SK-203: 5xx, network failure, unreadable body, unexpected status.
        // The failed read is retried after the back-off, plugged in or not.
        m_consecutiveFailures++;
        int wait = backoffSecondsForFailure(m_consecutiveFailures);
        m_lastBackoffSeconds = wait;
        if (response.retryAfter)
            wait = std::max(wait, *response.retryAfter);
        m_blockedUntil = now.addSecs(wait);
        m_block = Block::Backoff;
        m_planned = true;
        m_planIsInterval = false;
        m_plannedAt = m_blockedUntil;
        m_reason = QStringLiteral("failure %1 in a row, back-off %2 s").arg(m_consecutiveFailures).arg(wait);
    }

    // SK-201: keep a reserve. The headers come with 200, 429 and 5xx answers.
    // The pause applies to every request, refresh included; it plans none.
    if (response.rateLimitRemaining && *response.rateLimitRemaining < quotaReserve) {
        const int reset = response.rateLimitReset ? *response.rateLimitReset : quotaWindowFallbackSeconds;
        const QDateTime quotaNext = now.addSecs(reset);
        if (!m_blockedUntil.isValid() || quotaNext > m_blockedUntil) {
            m_blockedUntil = quotaNext;
            m_block = Block::Quota;
            if (!m_planned || quotaNext > m_plannedAt)
                m_reason = QStringLiteral("RateLimit-Remaining %1 below the reserve of %2, pause until RateLimit-Reset (%3 s)")
                               .arg(*response.rateLimitRemaining).arg(quotaReserve).arg(reset);
        }
    }

    logSchedule();
    armTimer();
}

void SkodaPoller::logSchedule()
{
    if (m_planned)
        qCInfo(dcSkodaCore()) << "Next request at" << notBefore().toString(Qt::ISODate) << "-" << m_reason;
    else
        qCInfo(dcSkodaCore()) << "No request planned -" << m_reason;
}

void SkodaPoller::armTimer()
{
    if (!m_running || m_halted || !m_planned) {
        m_timer.stop();
        return;
    }
    qint64 ms = 0;
    const QDateTime earliest = notBefore();
    if (earliest.isValid())
        ms = std::max<qint64>(0, m_clock().msecsTo(earliest));
    // Guard against a clock that moved backwards: the timer never waits longer
    // than an hour; a longer interval re-arms it (pollNow refuses until then).
    ms = std::min<qint64>(ms, 60LL * 60 * 1000) + 1000;
    m_timer.start(static_cast<int>(ms));
}

void SkodaPoller::setHalted(bool halted, const QString &reason)
{
    m_reason = reason;
    if (m_halted == halted)
        return;
    m_halted = halted;
    if (halted) {
        m_timer.stop();
        qCWarning(dcSkodaCore()) << "Polling halted until a new API key or a reconfiguration:" << reason;
    } else {
        qCInfo(dcSkodaCore()) << "Polling resumed:" << reason;
    }
    emit haltedChanged(halted);
}
