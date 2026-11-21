# etm-powersync-car

nymea integration plugins that read the state of a user's electric vehicle (state of charge,
plug state) from the vehicle maker's API, so that the PowerSync energy manager can use it.
One directory per maker; every plugin exposes the same vehicle thing. Read-only: no plugin
sends a command to the vehicle.

| Plugin | Maker API | Status |
|---|---|---|
| `skoda/` | MyŠkoda public API | in development |

Each user creates their own API key with the maker and enters it when adding the vehicle.
No key and no real VIN are stored in this repository.

## Build and tests

Built against nymea 1.15.2 (`libnymea-dev`, Qt 6):

```
mkdir build && cd build
qmake6 ../etm-powersync-car.pro            # plugins only
qmake6 CONFIG+=build_tests ../etm-powersync-car.pro   # plugins and tests
make -j$(nproc)
./tests/skoda/testskoda
```

The tests run against a local HTTP server that replays responses built on the structure of the
maker's API contract, with values of our own; they send no request to the real API. The contract
itself belongs to the maker and is not in this repository: `docs/skoda/README.md` says where to
download it for the two tests that check the fixtures against it (skipped without it).

License: GPL-3.0-only, version 3 of the GNU General Public License only (see `LICENSE`).
