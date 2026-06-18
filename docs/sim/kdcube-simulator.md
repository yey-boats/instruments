# Modeled SignalK simulator (lab)

The lab drives SignalK from the **`yey-boats/simulator`** image — the productized
KDCube boat simulator ([repo](https://github.com/yey-boats/simulator), CLI
`yey-boats-sim`). It's a route-following Adriatic boat with polar/weather/navigator
physics that emits a full modeled dataset at 1 Hz: SOG, STW (SOW), heading/COG,
apparent + true wind (AWA/AWS/TWA/TWS), depth, swell, temperatures, electrical,
**autopilot** (`steering.autopilot.*` incl. `target.headingTrue`), **AIS** vessel
contexts, polar targets, and `navigation.closestApproach.*` (bearing/range to the
nearest AIS target, for compass-marker binding).

## Builds

GitHub Actions builds and publishes a multi-arch image on every green push to
`main`:

```
ghcr.io/yey-boats/simulator:latest        # newest main build
ghcr.io/yey-boats/simulator:main
ghcr.io/yey-boats/simulator:sha-<short>
```

Versioned releases (`v*` tags) additionally publish to PyPI + a tagged image via
`release.yml`. There is no source subtree or venv to maintain anymore.

## Deployment (mythra-nav)

Run the image alongside the SignalK container, sharing its network namespace so
the sim reaches the server at `localhost:3000`. The manager's deploy scripts do
this automatically — `deploy/scripts/run-remote.sh` (in
`yey-boats/Instruments-manager`) pulls `ghcr.io/yey-boats/simulator:latest` and
starts it as the `boat-sim` container on the remote. To run it standalone:

```
# on mythra-nav, next to the signalk-server container
docker pull ghcr.io/yey-boats/simulator:latest
docker run -d --name boat-sim \
  --network "container:signalk-server" \
  -e SIGNALK_HOST=localhost -e SIGNALK_PORT=3000 \
  -e SIGNALK_USERNAME=admin -e SIGNALK_PASSWORD=admin \
  -v sim-data:/data \
  ghcr.io/yey-boats/simulator:latest
```

As a systemd unit (replaces the old `kdcube-sim.service` / `fake-boat.service`):

```
[Service]
ExecStartPre=-/usr/bin/docker rm -f boat-sim
ExecStartPre=/usr/bin/docker pull ghcr.io/yey-boats/simulator:latest
ExecStart=/usr/bin/docker run --rm --name boat-sim \
  --network container:signalk-server \
  -e SIGNALK_HOST=localhost -e SIGNALK_PORT=3000 \
  -e SIGNALK_USERNAME=admin -e SIGNALK_PASSWORD=admin \
  -v sim-data:/data ghcr.io/yey-boats/simulator:latest
ExecStop=/usr/bin/docker rm -f boat-sim
Restart=always
```

`docker logs -f boat-sim` to watch it. The GEBCO depth cache is built lazily into
the `sim-data` volume on first run (mount it to persist across restarts).

> Pulling from GHCR for a private repo needs a one-time
> `docker login ghcr.io` with a PAT that has `read:packages` on the mythra-nav
> host.

## Known gap: tidal current

The productized simulator does **not** emit `environment.current.*` (the old
subtree carried a local `kdcube-current-emission.patch` for it). The device's
wind-screen tide vector renders only when that path is present, so it stays blank
under this deployment until modeled set/drift is added to the simulator itself
(set around the ~150° Adriatic drift; drift breathing 0..~0.8 kn, periodically
dipping to ~0 to exercise the calm/zero-ring state). Track this as a follow-up in
`yey-boats/simulator` — it should live in the engine, not as a deploy-time patch.

## Verified paths

SignalK self vessel reports modeled `environment.wind.*`,
`navigation.speedOverGround`/`speedThroughWater`, `steering.autopilot.state` +
`target.headingTrue`, polar targets, and AIS contexts. The device picks these up
over its existing subscription (wind/compass widgets, AP HUD). `environment.current.*`
is pending (see above).
