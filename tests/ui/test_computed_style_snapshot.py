"""
Deterministic UI test for computed styles neutrality snapshot.
Verifies getComputedStyle over a fixed selector x element matrix
to ensure CSS refactoring/splitting remains visually and stylistically neutral.
"""

import json
import os
from playwright.sync_api import Page, expect

TARGET_SELECTORS = [
    "#app",
    ".top-bar",
    ".logo",
    ".view-tabs",
    ".tab-btn.active",
    ".search-box",
    "#searchInput",
    ".zoom-control",
    ".lang-select",
    ".btn-primary",
    ".btn-secondary",
    ".main-container",
    ".left-sidebar",
    ".sidebar-section-header",
    ".sidebar-menu",
    ".menu-item.active",
    ".count-badge",
    ".content-area",
    ".filter-bar",
    ".media-grid",
    ".photo-card",
    ".inspector-panel",
    ".timeline-container",
    ".status-bar",
]

TARGET_PROPERTIES = [
    "display",
    "position",
    "box-sizing",
    "color",
    "background-color",
    "font-size",
    "font-weight",
    "padding-top",
    "padding-right",
    "padding-bottom",
    "padding-left",
    "border-top-width",
    "flex-direction",
]

SNAPSHOT_FILE = os.path.join(os.path.dirname(__file__), "fixtures", "computed_style_snapshot.json")


def _collect_computed_styles(page: Page):
    return page.evaluate(
        """({ selectors, properties }) => {
            const results = {};
            for (const sel of selectors) {
                const el = document.querySelector(sel);
                if (!el) {
                    results[sel] = null;
                    continue;
                }
                const cs = window.getComputedStyle(el);
                const props = {};
                for (const prop of properties) {
                    props[prop] = cs.getPropertyValue(prop);
                }
                results[sel] = props;
            }
            return results;
        }""",
        {"selectors": TARGET_SELECTORS, "properties": TARGET_PROPERTIES},
    )


def test_computed_style_snapshot(server, page: Page):
    """Verify computed styles match the baseline snapshot."""
    page.goto(server["url"])
    expect(page.locator(".photo-card").first).to_be_visible()

    current_styles = _collect_computed_styles(page)

    # If snapshot does not exist, generate it
    if not os.path.exists(SNAPSHOT_FILE):
        os.makedirs(os.path.dirname(SNAPSHOT_FILE), exist_ok=True)
        with open(SNAPSHOT_FILE, "w", encoding="utf-8") as f:
            json.dump(current_styles, f, indent=2, sort_keys=True)

    with open(SNAPSHOT_FILE, "r", encoding="utf-8") as f:
        baseline_styles = json.load(f)

    # Compare
    for selector, props in baseline_styles.items():
        assert selector in current_styles, f"Selector {selector} missing in current styles"
        current_props = current_styles[selector]
        if props is None:
            assert current_props is None, f"Expected {selector} to not exist"
            continue
        assert current_props is not None, f"Element {selector} not found"
        for prop, val in props.items():
            assert (
                current_props.get(prop) == val
            ), f"Computed style mismatch on {selector} [{prop}]: got '{current_props.get(prop)}', expected '{val}'"
