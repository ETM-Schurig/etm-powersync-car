// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 ETM
//
// When to send the next request (SK-2xx). The quota comes from the
// RateLimit-* headers, never from a constant (SK-201).

#ifndef SKODAPOLLER_H
#define SKODAPOLLER_H

#include <QDateTime>
#include <QObject>
#include <QTimer>

#include <functional>

#include "skodaresponse.h"

class SkodaApiClient;

class SkodaPoller : public QObject
{
    Q_OBJECT
public:
    using Clock = std::function<QDateTime()>;

    // SK-200 (revised 2026-11-02, lots car-6 and car-7): the energy manager
    // computes the SoC between two readings, so the plugin reads rarely.
    // Plugged in, or plug state unknown: one reading every N min (thing
    // setting, default 30). Unplugged: no periodic reading at all; the energy
    // manager asks for one (refresh) when a vehicle is plugged in at a charger.
    static constexpr int pluggedIntervalSeconds = 30 * 60;
    static constexpr int pluggedIntervalMinMinutes = 5;
    static constexpr int pluggedIntervalMaxMinutes = 120;
    // Never less than this between two requests, whatever the cause
    // (start-up, reconfiguration, new key, refresh, 429...), across restarts too.
    static constexpr int minimumGapSeconds = 5 * 60;
    // SK-201: pause when fewer requests than this remain.
    static constexpr int quotaReserve = 4;
    // When RateLimit-Remaining is low but RateLimit-Reset is missing.
    static constexpr int quotaWindowFallbackSeconds = 60 * 60;
    // When a 429 carries neither Retry-After nor RateLimit-Reset.
    static constexpr int rateLimitFallbackSeconds = 15 * 60;

    explicit SkodaPoller(SkodaApiClient *client, QObject *parent = nullptr);

    void setClock(const Clock &clock);

    // Interval while plugged in or plug state unknown, in minutes (thing
    // setting, changed at run time). A planned periodic reading moves to the
    // last reading plus the new interval; no request is sent for the change.
    // Returns false, and keeps the current interval, when outside its bounds.
    bool setPluggedInterval(int minutes);
    int pluggedInterval() const { return m_pluggedInterval; }
    // French message when the interval is outside its bounds, else empty.
    static QString intervalError(int minutes);

    // Time of the last request sent for this vehicle, kept by the plugin
    // across restarts and reconfigurations (SK-200: no burst).
    void setLastRequestAt(const QDateTime &lastRequestAt);
    QDateTime lastRequestAt() const;

    // Polls now if allowed, then keeps polling on the timer.
    void start();
    void stop();

    // Sends the planned request if allowed: not halted, no request running, a
    // request planned and its time reached. Returns true when a request was sent.
    bool pollNow();

    // Action "refresh" (lot car-7): one reading now, whatever the plan, if the
    // minimum gap, a back-off, a 429 wait and the quota reserve allow it.
    // Sent: a request went out. Busy: one is already running, its answer
    // serves. Refused: no request, *refusal says why in French, with the
    // time of the next possible reading.
    enum class RefreshResult { Sent, Busy, Refused };
    RefreshResult refresh(QString *refusal);

    // An answer obtained outside the poller, used as a reading (lot car-7:
    // the pairing validation, so that a reconfiguration costs 1 request, not
    // 2). Emits responseReceived and plans the next request as for its own.
    void acceptResponse(const SkodaResponse &response);
    // Whether an answer can serve as the first reading: HTTP 200 with the
    // charging part.
    static bool usableAsFirstReading(const SkodaResponse &response);

    // After a new API key: lifts the halt and allows a request at once.
    void resume();

    bool halted() const;
    // Whether a request is planned (none while the vehicle is unplugged).
    bool hasPlannedRequest() const;
    // Whether the timer is running (diagnostics and tests).
    bool timerActive() const { return m_timer.isActive(); }
    // Time of the planned request: the plan, never less than minimumGapSeconds
    // after the last request, never before the end of a back-off, a 429 wait
    // or a quota pause. Invalid: at once if a request is planned.
    QDateTime notBefore() const;
    int backoffSeconds() const;
    QString scheduleReason() const;

    // The back-off sequence of SK-203: 5, 10, 20 then 30 minutes.
    static int backoffSecondsForFailure(int consecutiveFailures);

signals:
    // Emitted for every response, before the next request is scheduled.
    void responseReceived(const SkodaResponse &response);
    // Emitted when polling stops until a new key or a reconfiguration.
    void haltedChanged(bool halted);
    // Emitted when a request is sent, with its time (to be kept by the plugin).
    void requestSent(const QDateTime &at);

private:
    enum class Plug { Unknown, Connected, Disconnected };
    enum class Block { None, Backoff, RateLimit, Quota };

    void onVehicleFetched(const SkodaResponse &response);
    bool send(const QDateTime &now);
    // max(minimum gap, back-off / 429 / quota pause); invalid when free.
    QDateTime earliestAllowed() const;
    void logSchedule();
    void armTimer();
    void setHalted(bool halted, const QString &reason);

    SkodaApiClient *m_client = nullptr;
    Clock m_clock;
    QTimer m_timer;
    bool m_running = false;
    bool m_halted = false;
    // The plan: a request planned at m_plannedAt (invalid: at once), or none.
    bool m_planned = true;
    QDateTime m_plannedAt;
    // The plan is the periodic interval after a successful reading (not a
    // retry after a failure), started at m_lastReadAt.
    bool m_planIsInterval = false;
    QDateTime m_lastReadAt;
    // Hard limits from the API: back-off, 429, quota reserve.
    QDateTime m_blockedUntil;
    Block m_block = Block::None;
    QString m_reason;
    int m_consecutiveFailures = 0;
    int m_lastBackoffSeconds = 0;
    bool m_rateLimitEpisode = false;
    Plug m_plug = Plug::Unknown;
    int m_pluggedInterval = pluggedIntervalSeconds;
    QDateTime m_lastRequestAt;
};

#endif // SKODAPOLLER_H
