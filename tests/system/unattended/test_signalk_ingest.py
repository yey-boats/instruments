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
    sk_pump.send(SK_HOST, SK_PORT,
                 "navigation.speedOverGround", 3.21)
    f = device.wait_for_field("sog_mps", "signalk", timeout_s=8)
    assert abs(f["value"] - 3.21) < 0.05


def test_signalk_depth_publish(device):
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.depth.belowTransducer", 7.5)
    f = device.wait_for_field("depth_m", "signalk", timeout_s=8)
    assert abs(f["value"] - 7.5) < 0.1


def test_signalk_wind_publish(device):
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.wind.angleApparent", 0.785)  # ~45 deg
    sk_pump.send(SK_HOST, SK_PORT,
                 "environment.wind.speedApparent", 6.0)    # ~11.7 kn
    f_a = device.wait_for_field("awa_rad", "signalk", timeout_s=8)
    f_s = device.wait_for_field("aws_mps", "signalk", timeout_s=8)
    assert abs(f_a["value"] - 0.785) < 0.01
    assert abs(f_s["value"] - 6.0) < 0.05
    device.show_screen("wind")
    time.sleep(1.0)
    device.screenshot("signalk_wind")
