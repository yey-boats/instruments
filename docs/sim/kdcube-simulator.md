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

Use `--network host` so the sim reaches SignalK at `localhost:3000` regardless of
how the SK container is networked (bridge `-p` or host) **and** exposes its web
admin UI on the host. The web UI defaults to port 8080, but that's frequently
taken on lab hosts (mythra-nav runs `voicex` there), so pin it to **8088**:

```
# on mythra-nav, next to the signalk-server container
# 0) one-time: retire the legacy KDCube subtree sim (else two sims fight over
#    vessels.self — see "Two simulators" below)
sudo systemctl disable --now kdcube-sim.service

docker pull ghcr.io/yey-boats/simulator:latest
docker run -d --name boat-sim --restart unless-stopped \
  --network host \
  -e SIGNALK_HOST=localhost -e SIGNALK_PORT=3000 \
  -e SIGNALK_USERNAME=admin -e SIGNALK_PASSWORD=admin \
  -e SIM_WEB_HOST=0.0.0.0 -e SIM_WEB_PORT=8088 \
  -e DATA_DIR=/data -v sim-data:/data \
  ghcr.io/yey-boats/simulator:latest
```

The web admin UI is then at **`http://mythra-nav:8088/`** (API at `/api/status`).
`docker logs -f boat-sim` to watch it. The GEBCO depth cache is built lazily into
the `sim-data` volume on first run (mount it to persist across restarts).

> Pulling the private image needs a one-time `docker login ghcr.io` on the host
> with a token that has `read:packages` (GHCR doesn't offer org-scoped
> fine-grained PATs here — use a classic `read:packages` PAT, or make the package
> public). The login persists in the host's docker config for future pulls.

## Two simulators (gotcha)

Only **one** simulator may feed SignalK — both `boat-sim` and the legacy
`kdcube-sim.service` write `vessels.self` and will clobber each other (you'll see
the boat position/heading jitter between two states). The container deploy
**replaces** the subtree unit, so disable the old one once:

```
sudo systemctl disable --now kdcube-sim.service
```

The manager's `deploy/scripts/run-remote.sh` does this best-effort on each run.

## Tidal current

The simulator emits `environment.current.setTrue` (rad) + `environment.current.drift`
(m/s) from an engine model (`engine/current.py`): set ~150° ± 30° (Adriatic NW→SE),
drift 0–0.8 kn half-wave-rectified (touches 0 at the trough to exercise the device's
calm/zero-ring state), on a time-compressed ~8-minute cycle so it's observable within
a session. This replaces the old subtree's `kdcube-current-emission.patch` — it lives
in the engine, not as a deploy-time patch.

## Verified paths

SignalK self vessel reports modeled `environment.wind.*`,
`navigation.speedOverGround`/`speedThroughWater`, `steering.autopilot.state` +
`target.headingTrue`, polar targets, AIS contexts, and `environment.current.*`. The
device picks these up over its existing subscription (wind/compass widgets, AP HUD,
wind-screen tide vector).
