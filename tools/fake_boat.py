#!/usr/bin/env python3
"""Push synthetic boat data into a local SignalK server so the
dashboard gauges have something to draw. Connects as a WebSocket
provider client and sends deltas every second.

Usage: python3 fake_boat.py [host] [port]
"""
import asyncio
import json
import math
import sys
import time
import urllib.request
import websockets

HOST = sys.argv[1] if len(sys.argv) > 1 else "localhost"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3000


async def main():
    # Login to get a token
    login = {"username": "admin", "password": "admin"}
    req = urllib.request.Request(
        f"http://{HOST}:{PORT}/signalk/v1/auth/login",
        data=json.dumps(login).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=5) as r:
        token = json.loads(r.read().decode())["token"]
    print(f"got token len={len(token)}")

    uri = f"ws://{HOST}:{PORT}/signalk/v1/stream?subscribe=none&token={token}"
    print(f"connecting (authenticated)")
    async with websockets.connect(uri) as ws:
        # Identify as a provider; ignore any incoming hello frame
        hello = await ws.recv()
        print(f"server hello: {hello[:120]}...")
        t0 = time.time()
        while True:
            t = time.time() - t0
            # Synthesize sinusoidal boat data
            sog = 4.0 + 1.5 * math.sin(t / 30)               # 2.5..5.5 m/s
            cog = math.radians((50 + 10 * math.sin(t / 60)) % 360)
            heading = cog + 0.05
            depth = 12.0 + 4 * math.sin(t / 45)              # 8..16 m
            water_temp = 273.15 + 19 + math.sin(t / 600)
            awa = math.radians(35 + 20 * math.sin(t / 12))   # apparent wind angle
            aws = 6.0 + 2 * math.cos(t / 18)                  # ~12 kn
            tws = aws + 1.5
            twa = awa + 0.1
            lat = 41.3851 + 0.001 * math.sin(t / 200)
            lon = 2.1734 + 0.001 * math.cos(t / 200)
            battv = 12.7 + 0.2 * math.sin(t / 90)

            delta = {
                "context": "vessels.self",
                "updates": [{
                    "$source": "fake_boat.py",
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                    "values": [
                        {"path": "navigation.position", "value": {"latitude": lat, "longitude": lon}},
                        {"path": "navigation.speedOverGround", "value": sog},
                        {"path": "navigation.courseOverGroundTrue", "value": cog},
                        {"path": "navigation.headingTrue", "value": heading},
                        {"path": "environment.wind.angleApparent", "value": awa},
                        {"path": "environment.wind.speedApparent", "value": aws},
                        {"path": "environment.wind.angleTrueWater", "value": twa},
                        {"path": "environment.wind.speedTrue", "value": tws},
                        {"path": "environment.depth.belowTransducer", "value": depth},
                        {"path": "environment.water.temperature", "value": water_temp},
                        {"path": "electrical.batteries.house.voltage", "value": battv},
                        {"path": "electrical.batteries.house.stateOfCharge", "value": 0.82},
                        {"path": "tanks.fuel.0.currentLevel", "value": 0.65},
                        {"path": "tanks.freshWater.0.currentLevel", "value": 0.40},
                    ]
                }]
            }
            await ws.send(json.dumps(delta))
            await asyncio.sleep(1.0)


asyncio.run(main())
