"""
Deterministic UI tests for the Timeline scrub bar.
"""

import re
from playwright.sync_api import Page, expect


def test_timeline_rendering_and_tick_bars(server, page: Page):
    """Verify that timeline renders tick bars for each month with photos and correct tooltips."""
    page.goto(server["url"])

    timeline = page.locator("#timelineContainer")
    expect(timeline).to_be_visible()

    # In our seeded catalog, photos exist in 3 distinct months: Dec 2025, Jan 2026, Feb 2026
    bars = timeline.locator(".timeline-bar-wrap")
    expect(bars).to_have_count(3)

    # Verify bar labels and tooltips
    # Chronological descending order from timeline API (Feb 2026, Jan 2026, Dec 2025)
    expect(bars.nth(0)).to_have_attribute("title", "Feb 2026: 2 photos")
    expect(bars.nth(0).locator(".timeline-tick-label")).to_contain_text("Feb '26")

    expect(bars.nth(1)).to_have_attribute("title", "Jan 2026: 2 photos")
    expect(bars.nth(1).locator(".timeline-tick-label")).to_contain_text("Jan '26")

    expect(bars.nth(2)).to_have_attribute("title", "Dec 2025: 2 photos")
    expect(bars.nth(2).locator(".timeline-tick-label")).to_contain_text("Dec '25")

    # Each bar has a proportional inner bar element
    for i in range(3):
        inner_bar = bars.nth(i).locator(".timeline-bar")
        expect(inner_bar).to_be_visible()


def test_timeline_click_to_filter_and_reset(server, page: Page):
    """Clicking a timeline month bar filters the grid to that month; clicking Reset clears the filter."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    timeline = page.locator("#timelineContainer")
    jan_bar = timeline.locator(".timeline-bar-wrap", has_text="Jan '26")
    expect(jan_bar).to_be_visible()

    # Click Jan '26 bar to filter
    jan_bar.click()
    expect(jan_bar).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("January 2026")

    # Only Jan 2026 photos shown (beach.bmp and birthday.bmp)
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "beach.bmp" in names
    assert "birthday.bmp" in names

    # Click Reset button
    page.locator("#resetTimelineBtn").click()
    expect(jan_bar).not_to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)
