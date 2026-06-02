# SignalK Test Server Agent Notes

This folder owns the local SignalK server fixture used by `make demo-up`.

## Scope

- Keep this setup based on official SignalK server features and official
  plugins.
- Do not add custom bridge services here unless the user explicitly asks for a
  custom implementation.
- Runtime dependency installs belong in `config/package.json`.
- Plugin enablement belongs in `config/plugin-config-data/`.
- Server settings belong in `config/settings.json`.
- Before calling SignalK plugin work complete, check `git status --short`.
  Commit the implementation and tests when requested, or explicitly report the
  uncommitted files.

## Container

The test container is named `signalk-server`.

Published ports:

- `3000/tcp` - SignalK HTTP and WebSocket
- `10110/tcp` - SignalK NMEA 0183 TCP output

The container mounts this repo folder:

```text
signalk/config -> /home/node/.signalk
signalk/plugins -> /home/node/plugins
```

Use `./signalk/scripts/run.sh` to start or recreate the container. It installs
the plugin packages declared in `config/package.json` before launching the
server, then starts `tools/fake_boat.py` from the repo.

Use `./signalk/scripts/stop.sh` to stop the container and fake data producer.

## Enabled Plugins

`@signalk/signalk-to-nmea0183`

- Config: `config/plugin-config-data/sk-to-nmea0183.json`
- Purpose: convert SignalK deltas to NMEA 0183 sentences.
- Output: built-in SignalK `nmea-tcp` interface on TCP `10110`.

`@signalk/signalk-autopilot`

- Config: `config/plugin-config-data/autopilot.json`
- Backend: `emulator`
- Purpose: queryable autopilot command target for tests.

## Test Credentials

The demo data producer expects the local development credentials:

```text
username: admin
password: admin
```

Do not use this fixture as a production SignalK home.
