# MyŠkoda Public API — contract 1.1.0

## Provenance

The contract belongs to Škoda Auto. **It is not redistributed in this repository.** The plugin uses
only interface facts from it: field names, enumeration values, header names and problem types. The
test fixtures follow its structure with values of our own.

- Reference page: `https://public.api.connect.skoda-auto.cz/docs/swagger-ui.html`; its
  `https://public.api.connect.skoda-auto.cz/v3/api-docs/swagger-config` gives the contract URL
  `https://public.api.connect.skoda-auto.cz/docs/myskoda-public-api.yaml`.
- Exported on 2026-11-02 at 06:16 UTC, without an API key (the documentation is public).
- `info.version` = `1.1.0`, `openapi` = `3.0.3`.
- SHA-256 of the YAML: `fb0514bf3e289e4bfb634f13dd5b7dfd24eb06cbab829ccf6a6fbb831ecf4cfc`.
- SHA-256 of the JSON made by the command below: `965d55386bbb5a1f8ce20242f3c98dcb731313fea96217f18063b9de22d04b37`.

The server also answers `/v3/api-docs` with a contract generated from the code (`info.version` = `v0`,
`openapi` = `3.1.0`). It is not the published reference: the field names of the charging part are the
same, but it carries generation artefacts. The plugin follows the published YAML.

### Getting the contract (for the two contract tests)

From the repository root (needs `curl` and `python3-yaml`):

```
curl -fsSL -o docs/skoda/openapi-1.1.0.yaml \
    https://public.api.connect.skoda-auto.cz/docs/myskoda-public-api.yaml
echo "fb0514bf3e289e4bfb634f13dd5b7dfd24eb06cbab829ccf6a6fbb831ecf4cfc  docs/skoda/openapi-1.1.0.yaml" | sha256sum -c -
python3 -I -c 'import json, sys, yaml
data = yaml.safe_load(open(sys.argv[1], encoding="utf-8"))
with open(sys.argv[2], "w", encoding="utf-8") as out:
    json.dump(data, out, indent=2, ensure_ascii=False)
    out.write("\n")' docs/skoda/openapi-1.1.0.yaml docs/skoda/openapi-1.1.0.json
```

If `sha256sum -c` fails, Škoda has published another version: the field table below may no longer
hold. The tests `contractDescribesEveryFieldRead` and `fixturesFollowTheContract` read
`docs/skoda/openapi-1.1.0.json`, or the file named by `SKODA_OPENAPI`. Without it they are skipped;
the other tests do not need the contract. Both files are listed in `.gitignore`: never commit them.

## Fields read by the plugin

The plugin sends one request per cycle:

```
GET /api/v1/vehicles/{vin}?include=charging
X-API-Key: <key>
```

`include` is a comma-separated list of `VehiclePart` values (`info`, `status`, `fuelStatus`, `odometer`,
`parkingPosition`, `airConditioning`, `auxiliaryHeating`, `activeVentilation`, `charging`,
`chargingProfiles`, `operations`). When `include` is omitted, every supported part is returned,
including `parkingPosition`. With `include=charging` the response holds `vehicle.vin`,
`vehicle.charging` and `errors` only: no parking position, no vehicle name, no licence plate.
The plug state is part of `charging.status`, so no other part is needed.

| API field (`vehicle.charging.…`) | Type in the contract | nymea state | Rule |
|---|---|---|---|
| `status.battery.stateOfChargeInPercent` | integer | `batteryLevel` (%) | Absent: unchanged, `socTimestamp` does not move. Outside 0–100: treated as absent, logged. |
| `carCapturedTimestamp` | date-time, time the vehicle captured and sent the data | `socTimestamp`, `socTimestampSource` = `vehicle` | When absent while the SoC is present: reception time, `socTimestampSource` = `reception`. |
| `status.plugConnectionState` | string: `CONNECTED`, `DISCONNECTED`, new values possible (1.1.0) | `plugState`: `connected`, `disconnected`, `unknown` | Absent: unchanged. Unknown value: `unknown`, logged once. |
| `vehicle.charging` absent, `errors` lists `CHARGING_UNAVAILABLE` / `CHARGING_DISABLED` / `CHARGING_UNSUPPORTED` | — | none | SoC and plug state unchanged, timestamps frozen; the `errors` list is logged at info level when it changes. |

The plugin reads only these three fields (SK-603, data minimisation; decision of 2026-11-02: the
SoC is enough, charge limits are a setting of the energy manager, not a value read from the car).
Every other member of the answer is ignored, including members the contract does not list yet:
`isVehicleInSavedLocation`, the whole of `settings`, and in `status` everything but
`plugConnectionState` and `battery.stateOfChargeInPercent`.

**Phases.** The contract (1.1.0) has no field for the number of AC phases nor for the vehicle's
own AC current limits. The nearest members, not read, are `settings.maxChargeCurrentAc` (`REDUCED`
or `MAXIMUM`, the current used when a charge starts), `settings.maxChargeCurrentAcAmpere`
(integer 5 to 32, "can acquire values 5, 10, 13 or 32": a limit chosen in the car, not the on-board
charger's capability) and `status.chargePowerInKw` (power of a charge in progress). None of them
gives the phase count; the plugin takes it from its `acPhaseCount` setting (see `skoda/README.md`).

## Headers and errors

| Contract | nymea state / behaviour |
|---|---|
| `X-API-Key-Expires-At` (date-time, on every successful response and on 401) | `apiKeyExpiresAt` (Unix time), `apiKeyExpiringSoon` when less than 14 days remain |
| `RateLimit-Limit`, `RateLimit-Remaining`, `RateLimit-Reset` (seconds) | `rateLimitRemaining`; pause until `RateLimit-Reset` when `RateLimit-Remaining` < 4 |
| `Retry-After` (seconds, on 429 and 503) | No request before it expires |
| `401` `…/problems/api-key-expired` | `apiStatus` = `keyExpired`, polling stops until a new key |
| `401` `about:blank` (missing or invalid key) | `apiStatus` = `keyInvalid`, polling stops until a new key |
| `403` `…/problems/api-key-not-authorized` | `apiStatus` = `keyNotAuthorized`, polling stops until a new key |
| `404` (no vehicle for this VIN) | `apiStatus` = `vehicleNotFound`, polling stops until reconfiguration |
| `429` `…/problems/rate-limit-exceeded` | `apiStatus` = `rateLimited` |
| `429` `…/problems/vehicle-not-accepting-requests` | `apiStatus` = `vehicleNotAccepting` |
| `500`, `503` | `apiStatus` = `serviceUnavailable`, back-off 5 → 10 → 20 → 30 min |
| `504` (vehicle did not answer in time) | `apiStatus` = `vehicleUnavailable`, same back-off |

Problem types are matched on the last segment of the `type` URI of the `application/problem+json` body.
