# Škoda (MyŠkoda public API)

nymea integration plugin that reads the state of charge and the plug state of a Škoda electric vehicle from the
MyŠkoda public API (`public.api.connect.skoda-auto.cz`, contract 1.1.0). Read-only: it sends no
command to the vehicle. Built against nymea 1.15.2.

## Setup

Add a "Škoda (API MyŠkoda)" thing with its **VIN** (17 characters). nymea then asks for the API key
created in the MyŠkoda app for that vehicle. The key is checked with one request, then kept in the
plugin storage (readable by the nymead user only), never in a thing param. The answer to that check
is the first reading of the vehicle.

**Settings** (`Integrations.SetThingSettings`, bounds checked by nymea), changed at once, without
reconfiguring the vehicle and without a request:

| Setting | Values | Default |
|---|---|---|
| Reading interval, plugged in or plug state unknown (`pluggedIntervalMinutes`) | 5 to 120 min | 30 |
| Battery capacity (`capacity`) | 0 to 200 kWh (0 = unknown) | 0 |
| AC phases the vehicle charges on (`acPhaseCount`) | 1, 2 or 3 | 3 |

nymea checks the bounds and refuses a value outside them; for `acPhaseCount` the plugin also
ignores a value outside 1-3 or with a fractional part (logged). There is no interval for an
unplugged vehicle: it is not read periodically.

**AC phases.** The API does not give the number of phases the vehicle charges on, so it is a
setting. The default, 3, is the on-board charger of the current electric range (11 kW,
three-phase); set 1 or 2 for a vehicle that charges on fewer phases (some versions with a 7.2 kW
on-board charger, or a single-phase installation). The energy manager uses it to turn a charging
current into a power: a three-phase vehicle counted as single-phase would draw three times the
power it plans for.

nymea 1.15.2 puts every setting of a thing back to its default when the thing is reconfigured
through the pairing dialog. The plugin keeps the last value of each setting in its storage and sets
it again right after: the settings survive a reconfiguration. The defaults nymea puts back are not
taken for a change: a setting change that arrives while the thing's setup is in progress comes from
nymea, not from the user, and is not stored (fixed in 0.1.7; in 0.1.5 and 0.1.6 the defaults
overwrote the stored values). A stored value outside the bounds gives the default (logged). The
stored values go with the thing when it is removed.

**Update from 0.1.4 or earlier.** The capacity used to be a thing param (`batteryCapacity`). On the
first start of 0.1.5, the plugin takes its value over into the setting (nymea still loads a param
that left the plugin description), once: a vehicle set up with 52 kWh keeps 52 kWh. A former value
above 200 kWh (the param allowed up to 250) gives 0, logged.

**Action.** *Relire maintenant* (`refresh`, no param): one reading now, see "Polling".

## States

The plugin publishes the state of charge (`batteryLevel`, `socKnown`, `socTimestamp`,
`socTimestampSource`, `batteryCritical`, `capacity`), the plug state (`plugState`, used to pair the
vehicle with a charger) and the state of the API link (`connected`, `apiStatus`, `apiStatusMessage`,
`apiKeyExpiresAt`, `apiKeyExpiringSoon`, `rateLimitRemaining`). It also publishes the AC phase
count from the `acPhaseCount` setting, at setup and on each change, under two names with the same
value (1 to 3): `phaseCount`, the name of the `electricvehicle` interface in nymea 1.15.2, read by
the energy manager, and `acPhaseCount`, its name from nymea 1.16. It reads nothing else from the API:
range, charge limit, battery care mode and charging state are not read. The charge limits used by
the energy manager are its own settings, the same for every vehicle make.

## Unknown values

A field the API does not give is never published as a made-up value. Each state has an explicit
"unknown" value, and a consumer must test for it before using the state:

| State | Unknown when | Test before use |
|---|---|---|
| `batteryLevel` | no SoC read yet (published as 0) | `socKnown == true` |
| `socTimestamp` | no SoC read yet (0) | `socKnown == true` |
| `socTimestampSource` | no SoC read yet | `!= "unknown"` |
| `capacity` | setting not entered (0) | `capacity > 0` |
| `plugState` | not given, or a value the contract does not list | `!= "unknown"` |
| `apiKeyExpiresAt` | header absent | `> 0` |
| `rateLimitRemaining` | header absent | `>= 0` (-1 = unknown) |

**`batteryLevel` and `socKnown`.** nymea 1.15.2 cannot publish a state without a value, so
`batteryLevel` is 0 until a first reading. `socKnown` is false until then and true afterwards; it
is a cached state, kept across restarts with `batteryLevel`. A later answer without a SoC keeps the
last value; check `socTimestamp` for its age (`socTimestampSource` says whether the time comes from
the vehicle or is the reception time).

**`capacity`.** The API does not give the battery capacity. It follows the `capacity` setting, and
0 means "not entered": a consumer must test `capacity > 0` before computing energy from a SoC.

**`phaseCount` and `acPhaseCount`.** Not read from the API: they follow the `acPhaseCount` setting
and always hold 1, 2 or 3. They say what the user entered, not what the vehicle reported.

`batteryCritical` is true below 30 % of a SoC actually read, false while the SoC is unknown.

## Polling

The energy manager computes the SoC between two readings (energy counted by the charger divided by
the capacity) and resets it at each reading, so the plugin reads rarely. One request per reading,
within the API quota (20 requests per hour and per VIN, `RateLimit-Limit` 20, `RateLimit-Reset` up to
3600 s):

| Rule | Behaviour |
|---|---|
| SK-200 plugged in | Plug `CONNECTED`, or plug state unknown (never read since start-up, or a value the contract does not list): one reading every N min, N being the setting (default 30). An absent plug state keeps the last known one. |
| SK-200 unplugged | Plug `DISCONNECTED`: no periodic reading at all. The energy manager asks for one (`refresh`) when a vehicle is plugged in at a charger. A failed reading is still retried after its back-off or its 429 wait (see below). |
| SK-200 minimum gap | Never less than 5 min between two requests, whatever the cause: start-up, restart of nymead, reconfiguration, new key, refresh, 429. The time of the last request (the pairing validation included) is kept in the plugin storage. A first reading at once is allowed when the last request is at least 5 min old. |
| Action `refresh` | One reading now, whatever the plan (unplugged included), if the minimum gap, a back-off, a 429 wait and the quota reserve allow it. Otherwise the action fails at once, without a request, with the cause and "Prochaine lecture possible à HH:MM" (local time). A request already running serves the action. The action ends once every state is set. |
| SK-201 quota reserve | When `RateLimit-Remaining` < 4, no request (refresh included) before `RateLimit-Reset` (60 min if absent). The pause plans no request by itself. |
| SK-202 HTTP 429 | No request before `Retry-After` (else `RateLimit-Reset`, else 15 min), never less than 5 min; then the reading is retried. |
| SK-203 back-off | 5xx, 504, network failure, unreadable answer: 5, 10, 20 then 30 min, or `Retry-After` if longer; then the reading is retried. A success resets it. |
| SK-103, SK-402 halt | 401, 403, 404: no request, refresh included, until a new key or a reconfiguration. |

**Why a failed reading is retried, even unplugged.** The plan comes from the last reading that
succeeded. If the API fails at the moment the vehicle is unplugged, the last good reading still says
`connected`; without the retry nothing would be read again until a refresh, and `plugState` would
stay `connected` indefinitely. The retry (back-off 5, 10, 20, 30 min, or the 429 wait) ends with the
first reading that succeeds; if it says `DISCONNECTED`, the plugin then stops reading.

The reserve, the 429 wait and the back-off can only lengthen the wait, never shorten it. Changing
the interval setting moves a planned periodic reading to the last reading plus the new interval.
`apiStatus` and `apiStatusMessage` explain why the plugin is not reading (expired key, vehicle not
found, quota…).

**Cost of a pairing or a reconfiguration.** Through the pairing dialog (PairThing, then
ConfirmPairing with the key), `confirmPairing` checks the key with one request; when the answer is
a 200 with the charging part, it is the first reading: 1 request in all (otherwise the first
reading follows 5 min later: 2). Through `Integrations.ReconfigureThing` (JSON-RPC, every param
sent, no key asked), the plugin reuses the stored key: 1 request, the first reading, at once if the
last request is at least 5 min old. Changing a setting (interval, capacity, AC phases) costs no
request.

## Tests

`tests/skoda/` runs the plugin core against a local HTTP server that replays responses built on
the structure of the contract (values of our own); no request goes to the real API. The contract is
not in this repository: `docs/skoda/README.md` says how to get it. See the build instructions in the top-level README.
