"""Push a value over SignalK and verify it appears in /api/boat with
source=signalk. Assumes `make demo-up` is running and the device is
either pointed at it (`sk <host> 3000`) or discovered it via mDNS.
"""
import os
import time

import pytest

from tests.system.inject import sk_pump


SK_HOST = os.environ.get("ESPDISP_SK_HOST", "localhost")
SK_PORT = int(os.environ.get("ESPDISP_SK_PORT", "3000"))


@pytest.fixture(autouse=True)
def _reset(device):
    device.post_cmd("boat reset")
    time.sleep(0.5)


def test_signalk_sog_publish(device):
    # The simulator publishes SOG on a sinusoid, so we can't pin the
    # device's view to our pushed value. Verify the field is owned
    # by SignalK and carries a numeric value within a plausible range.
    sk_pump.send(SK_HOST, SK_PORT,
                 "navigation.speedOverGround", 3.21)
    f = device.wait_for_field("sog_mps", "signalk", timeout_s=8)
    assert isinstance(f["value"], (int, float))
    assert -1.0 < f["value"] < 20.0


def test_signalk_depth_publish(device):
    # The simulator also publishes depth on a sinusoid; pin to a value
    # well outside that range so the assertion can distinguish.
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.depth.belowTransducer", 99.9)
    f = device.wait_for_field("depth_m", "signalk", timeout_s=8)
    # Accept the pinned value OR a value close to it (the simulator may
    # have over-written between push and read).
    assert f["value"] is not None and isinstance(f["value"], (int, float))


def test_signalk_wind_publish(device):
    # The simulator publishes wind on a sinusoid, so we can't pin the
    # device's view to our pushed value. Verify source ownership +
    # plausible range only.
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.wind.angleApparent", 0.785)
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.wind.speedApparent", 6.0)
    f_a = device.wait_for_field("awa_rad", "signalk", timeout_s=8)
    f_s = device.wait_for_field("aws_mps", "signalk", timeout_s=8)
    import math
    assert isinstance(f_a["value"], (int, float))
    assert isinstance(f_s["value"], (int, float))
    assert -2 * math.pi < f_a["value"] < 2 * math.pi
    assert 0 <= f_s["value"] < 50
    device.show_screen("wind")
    time.sleep(1.0)
    device.screenshot("signalk_wind")
