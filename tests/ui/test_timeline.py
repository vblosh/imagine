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


def test_timeline_labels_do_not_overlap_with_many_months(server, page: Page):
    """Ensure timeline labels do not overlap when catalog contains dozens of months."""
    page.goto(server["url"])
    expect(page.locator("#timelineContainer .timeline-bar-wrap")).to_have_count(3)

    # Provide 60 months of data across 5 years
    months_data = []
    for y in range(2026, 2021, -1):
        for m in range(12, 0, -1):
            months_data.append({"year": y, "month": m, "count": (y * 12 + m) % 50 + 1})

    page.evaluate(
        """(data) => {
        window._imagineApp.state.timelineData = data;
        window._imagineApp.renderTimeline();
    }""",
        months_data,
    )

    bars = page.locator("#timelineContainer .timeline-bar-wrap")
    expect(bars).to_have_count(60)

    # Check bounding rects of first 10 visible bars to guarantee no overlapping
    overlap_check = page.evaluate(
        """() => {
        const labels = Array.from(document.querySelectorAll('#timelineContainer .timeline-bar-wrap .timeline-tick-label'));
        let hasOverlap = false;
        for (let i = 0; i < labels.length - 1; i++) {
            const r1 = labels[i].getBoundingClientRect();
            const r2 = labels[i + 1].getBoundingClientRect();
            // Since timeline is rendered left-to-right (descending order):
            // r1 is to the left of r2, so r1.right should not be greater than r2.left
            if (r1.right > r2.left + 0.5) {
                hasOverlap = true;
                break;
            }
        }
        return { hasOverlap };
    }"""
    )
    assert not overlap_check["hasOverlap"], "Timeline month labels must not overlap"


def test_timeline_wheel_scrolls_horizontally(server, page: Page):
    """Wheel scrolling on timelineContainer translates vertical deltaY into horizontal scroll."""
    page.goto(server["url"])

    # Provide 60 months of data so timeline overflows and can scroll
    months_data = [{"year": 2026 - i // 12, "month": 12 - (i % 12), "count": 10} for i in range(60)]
    page.evaluate(
        """(data) => {
        window._imagineApp.state.timelineData = data;
        window._imagineApp.renderTimeline();
    }""",
        months_data,
    )

    timeline = page.locator("#timelineContainer")
    expect(timeline).to_be_visible()

    scroll_result = page.evaluate(
        """() => {
        const el = document.getElementById('timelineContainer');
        const start = el.scrollLeft;
        el.dispatchEvent(new WheelEvent('wheel', { deltaY: 150, cancelable: true, bubbles: true }));
        return { start, after: el.scrollLeft };
    }"""
    )
    assert scroll_result["after"] > scroll_result["start"]

