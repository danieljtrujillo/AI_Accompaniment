r"""Smoke test: open the UI in a visible browser, run one window end-to-end.

Prereqs — start these two processes in separate terminals before running:

  PS>  .\musical-accompaniment-ldm\.venv\Scripts\python.exe musical-accompaniment-ldm\server_CD.py
  PS>  .\musical-accompaniment-ldm\.venv\Scripts\python.exe clients\web_ui\bridge.py

Then:

  PS>  .\musical-accompaniment-ldm\.venv\Scripts\python.exe -m pytest tests/playwright -s

The test is HEADFUL by default per user requirement. Set PW_HEADLESS=1 to
override (CI only).
"""

from __future__ import annotations

import os
import re
import pytest
from playwright.sync_api import Page, expect, sync_playwright


UI_URL = os.environ.get("UI_URL", "http://127.0.0.1:5173/")
HEADLESS = os.environ.get("PW_HEADLESS", "0") == "1"


@pytest.fixture(scope="module")
def page():
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=HEADLESS, slow_mo=150)
        ctx = browser.new_context(viewport={"width": 1280, "height": 900})
        pg = ctx.new_page()
        pg.on("console", lambda m: print(f"  [console.{m.type}] {m.text}"))
        yield pg
        ctx.close()
        browser.close()


def _expect_log(page: Page, pattern: str, timeout: float = 60_000) -> None:
    log_loc = page.get_by_test_id("log")
    expect(log_loc).to_contain_text(re.compile(pattern), timeout=timeout)


def test_ui_smoke(page: Page):
    page.goto(UI_URL)
    expect(page).to_have_title(re.compile("multi_track"))

    # Connect to bridge
    page.get_by_test_id("btn-ws-connect").click()
    expect(page.get_by_test_id("ws-status")).to_contain_text("open", timeout=5_000)

    # Connect OSC client → server
    page.get_by_test_id("btn-osc-connect").click()
    expect(page.get_by_test_id("osc-status")).to_contain_text("open")

    # Configure (defaults are fine — bass + drums, r=0.25)
    page.get_by_test_id("btn-configure").click()
    _expect_log(page, r"configure: r=0.25", timeout=5_000)

    # Load model (server may already be warm, but still arm+send)
    page.get_by_test_id("btn-load").click()
    expect(page.get_by_test_id("model-status")).to_contain_text("ready", timeout=120_000)

    # Probe
    page.get_by_test_id("btn-probe").click()
    _expect_log(page, r"probe RTT [\d.]+ ms", timeout=5_000)

    # Reset so a fresh batch_id counter doesn't collide
    page.get_by_test_id("btn-reset").click()
    _expect_log(page, r"reset: sent", timeout=3_000)

    # Load bundled test audio
    page.get_by_test_id("btn-load-test").click()
    _expect_log(page, r"loaded test_tone_8s\.wav", timeout=5_000)

    # Run offline (one window)
    page.get_by_test_id("btn-offline").click()
    _expect_log(page, r"offline run complete", timeout=90_000)

    # Verify both stems produced downloadable WAVs
    expect(page.get_by_test_id("dl-bass")).to_be_visible()
    expect(page.get_by_test_id("dl-drums")).to_be_visible()

    # Take a screenshot for the record
    out_dir = os.path.dirname(os.path.abspath(__file__))
    page.screenshot(path=os.path.join(out_dir, "smoke_result.png"), full_page=True)
