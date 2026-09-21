"""
Deterministic UI tests for the Timeline scrub bar.
"""

from datetime import datetime, timezone
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

    # Verify tooltips and chronological descending order.
    # Chronological descending order from timeline API (Feb 2026, Jan 2026, Dec 2025)
    expect(bars.nth(0)).to_have_attribute("title", "Feb 2026: 2 photos")
    expect(bars.nth(1)).to_have_attribute("title", "Jan 2026: 2 photos")
    expect(bars.nth(2)).to_have_attribute("title", "Dec 2025: 2 photos")

    year_labels = timeline.locator(".timeline-year-label")
    expect(year_labels).to_have_count(2)
    expect(year_labels.nth(0)).to_have_text("2026")
    expect(year_labels.nth(1)).to_have_text("2025")

    year_select = page.locator("#timelineYearSelect")
    expect(year_select).to_have_value("all")
    expect(year_select.locator("option")).to_have_count(3)

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
    jan_bar = timeline.locator('.timeline-bar-wrap[data-year="2026"][data-month="1"]')
    expect(jan_bar).to_be_visible()

    # Click the January bar to filter.
    jan_bar.click()
    expect(jan_bar).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("January 2026")

    # Only Jan 2026 photos shown (beach.bmp and birthday.bmp)
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "beach.bmp" in names
    assert "birthday.bmp" in names

    # Clicking the active single month toggles the timeline filter off.
    jan_bar.click()
    expect(cards).to_have_count(6)
    expect(jan_bar).not_to_have_class(re.compile(r"\bactive\b"))

    # Select it again so Reset exercises the same complete clear path.
    jan_bar.click()
    expect(cards).to_have_count(2)

    # Click Reset button
    page.locator("#resetTimelineBtn").click()
    expect(jan_bar).not_to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(6)
    expect(page.locator("#timelineYearSelect")).to_have_value("all")
    state = page.evaluate("""() => ({
        activeTimelinePeriod: window._imagineApp.state.activeTimelinePeriod,
        timelineAnchor: window._imagineApp.state.timelineAnchor
    })""")
    assert state["activeTimelinePeriod"] is None
    assert state["timelineAnchor"] is None


def test_timeline_shift_click_filters_an_inclusive_range(server, page: Page):
    """Shift-click ranges from the last normally clicked month in either direction."""
    page.goto(server["url"])

    timeline = page.locator("#timelineContainer")
    jan_bar = timeline.locator('.timeline-bar-wrap[data-year="2026"][data-month="1"]')
    feb_bar = timeline.locator('.timeline-bar-wrap[data-year="2026"][data-month="2"]')
    dec_bar = timeline.locator('.timeline-bar-wrap[data-year="2025"][data-month="12"]')

    jan_bar.click()
    dec_bar.click(modifiers=["Shift"])

    expect(page.locator(".photo-card")).to_have_count(4)
    expect(timeline.locator(".timeline-bar-wrap.active")).to_have_count(2)
    expect(page.locator("#filterLabel")).to_contain_text("December 2025 – January 2026")
    expect(page.locator("#timelineYearSelect")).to_have_value("custom")

    result = page.evaluate("""() => ({
        period: window._imagineApp.state.activeTimelinePeriod,
        anchor: window._imagineApp.state.timelineAnchor,
        params: window._imagineApp.buildMediaParams()
    })""")
    assert result["period"] == {
        "startYear": 2025,
        "startMonth": 12,
        "endYear": 2026,
        "endMonth": 1,
    }
    assert result["anchor"] == {"year": 2026, "month": 1}
    assert result["params"]["date_from"] == int(datetime(2025, 12, 1, tzinfo=timezone.utc).timestamp())
    assert result["params"]["date_to"] == int(datetime(2026, 2, 1, tzinfo=timezone.utc).timestamp()) - 1

    # The anchor remains January, so another Shift-click replaces the far endpoint.
    feb_bar.click(modifiers=["Shift"])
    expect(page.locator(".photo-card")).to_have_count(4)
    expect(timeline.locator(".timeline-bar-wrap.active")).to_have_count(2)
    expect(jan_bar).to_have_class(re.compile(r"\bactive\b"))
    expect(feb_bar).to_have_class(re.compile(r"\bactive\b"))
    expect(dec_bar).not_to_have_class(re.compile(r"\bactive\b"))


def test_timeline_year_selector_filters_and_clears(server, page: Page):
    """The year selector applies a whole-year range and All years clears it."""
    page.goto(server["url"])

    year_select = page.locator("#timelineYearSelect")
    year_select.select_option("2026")

    expect(year_select).to_have_value("2026")
    expect(page.locator(".photo-card")).to_have_count(4)
    expect(page.locator("#timelineContainer .timeline-bar-wrap.active")).to_have_count(2)
    params = page.evaluate("() => window._imagineApp.buildMediaParams()")
    assert params["date_from"] == int(datetime(2026, 1, 1, tzinfo=timezone.utc).timestamp())
    assert params["date_to"] == int(datetime(2027, 1, 1, tzinfo=timezone.utc).timestamp()) - 1

    year_select.select_option("all")
    expect(year_select).to_have_value("all")
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(page.locator("#timelineContainer .timeline-bar-wrap.active")).to_have_count(0)


def test_timeline_controls_and_range_are_localized(server, page: Page):
    """Year controls and range summaries update in every supported language."""
    page.goto(server["url"])

    translations = {
        "en": ("All years", "Filter timeline by year"),
        "es": ("Todos los años", "Filtrar la línea de tiempo por año"),
        "de": ("Alle Jahre", "Zeitleiste nach Jahr filtern"),
        "ru": ("Все годы", "Фильтровать временную шкалу по году"),
        "zh": ("所有年份", "按年份筛选时间线"),
    }
    for language, (all_years, aria_label) in translations.items():
        page.locator("#langSelect").select_option(language)
        expect(page.locator('#timelineYearSelect option[value="all"]')).to_have_text(all_years)
        expect(page.locator("#timelineYearSelect")).to_have_attribute("aria-label", aria_label)

    page.locator("#langSelect").select_option("de")
    timeline = page.locator("#timelineContainer")
    timeline.locator('.timeline-bar-wrap[data-year="2026"][data-month="1"]').click()
    timeline.locator('.timeline-bar-wrap[data-year="2025"][data-month="12"]').click(modifiers=["Shift"])
    expect(page.locator("#filterLabel")).to_contain_text("Dezember 2025 – Januar 2026")
    expect(page.locator('#timelineYearSelect option[value="custom"]')).to_have_text("Benutzerdefinierter Bereich")


def test_timeline_is_compact_and_year_labels_do_not_overlap(server, page: Page):
    """Month bars have one-pixel spacing and only one non-overlapping label per year."""
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

    layout = page.evaluate(
        """() => {
        const labels = Array.from(document.querySelectorAll('#timelineContainer .timeline-year-label'));
        let hasOverlap = false;
        for (let i = 0; i < labels.length - 1; i++) {
            const r1 = labels[i].getBoundingClientRect();
            const r2 = labels[i + 1].getBoundingClientRect();
            if (r1.right > r2.left + 0.5) {
                hasOverlap = true;
                break;
            }
        }
        const gaps = [];
        document.querySelectorAll('#timelineContainer .timeline-year-bars').forEach(group => {
            const bars = Array.from(group.querySelectorAll('.timeline-bar-wrap'));
            for (let i = 0; i < bars.length - 1; i++) {
                const current = bars[i].getBoundingClientRect();
                const next = bars[i + 1].getBoundingClientRect();
                gaps.push(next.left - current.right);
            }
        });
        return { hasOverlap, gaps };
    }"""
    )
    expect(page.locator("#timelineContainer .timeline-year-label")).to_have_count(5)
    assert not layout["hasOverlap"], "Timeline year labels must not overlap"
    assert layout["gaps"]
    assert all(0.5 <= gap <= 1.5 for gap in layout["gaps"]), layout["gaps"]


def test_timeline_wheel_scrolls_horizontally(server, page: Page):
    """Wheel scrolling on timelineContainer translates vertical deltaY into horizontal scroll."""
    page.goto(server["url"])

    timeline = page.locator("#timelineContainer")
    expect(timeline).to_be_visible()
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(timeline.locator(".timeline-bar-wrap")).to_have_count(3)

    # Provide 60 months of data so timeline overflows and can scroll
    months_data = [{"year": 2026 - i // 12, "month": 12 - (i % 12), "count": 10} for i in range(60)]
    page.evaluate(
        """(data) => {
        window._imagineApp.state.timelineData = data;
        window._imagineApp.renderTimeline();
    }""",
        months_data,
    )
    expect(timeline.locator(".timeline-bar-wrap")).to_have_count(60)

    scroll_result = page.evaluate(
        """() => {
        const el = document.getElementById('timelineContainer');
        const start = el.scrollLeft;
        el.dispatchEvent(new WheelEvent('wheel', { deltaY: 150, cancelable: true, bubbles: true }));
        return { start, after: el.scrollLeft };
    }"""
    )
    assert scroll_result["after"] > scroll_result["start"]

