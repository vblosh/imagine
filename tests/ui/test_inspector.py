"""
Deterministic UI tests for the Inspector panel.
"""

import re
from playwright.sync_api import Page, expect


def test_inspector_toggle_and_collapse(server, page: Page):
    """Test collapsing and expanding the right inspector panel."""
    page.goto(server["url"])

    inspector = page.locator("#rightInspector")
    expect(inspector).to_be_visible()
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    # Click toggle inspector button to collapse
    page.locator("#toggleInspectorBtn").click()
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))

    # Click toggle inspector button again to expand
    page.locator("#toggleInspectorBtn").click()
    expect(inspector).not_to_have_class(re.compile(r"\bcollapsed\b"))

    # Click close button inside inspector to collapse
    page.locator("#closeInspectorBtn").click()
    expect(inspector).to_have_class(re.compile(r"\bcollapsed\b"))


def test_inspector_metadata_display(server, page: Page):
    """Selecting a photo populates File Information and Camera/Exposure EXIF metadata."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    expect(page.locator("#inspectorNoSelection")).to_be_hidden()
    expect(page.locator("#inspectorSelection")).to_be_visible()

    # File information
    expect(page.locator("#infoFileName")).to_have_text("mountain.bmp")
    expect(page.locator("#infoDimensions")).to_have_text("1920 × 1080 px")
    expect(page.locator("#infoFileSize")).to_contain_text("B")
    expect(page.locator("#infoDateTaken")).to_contain_text("2026")
    expect(page.locator("#infoFilePath")).to_contain_text("nature/mountain.bmp")

    # Camera & Exposure (EXIF)
    expect(page.locator("#infoCamera")).to_have_text("Sony ILCE-7M4")
    expect(page.locator("#infoLens")).to_have_text("FE 24-70mm F2.8 GM II")
    expect(page.locator("#infoExposure")).to_have_text("1/500s")
    expect(page.locator("#infoAperture")).to_have_text("f/2.8")
    expect(page.locator("#infoIso")).to_have_text("ISO 100")
    expect(page.locator("#infoFocal")).to_have_text("35.0 mm")
    expect(page.locator("#infoGps")).to_contain_text("46.50000, 11.35000")


def test_inspector_rating_and_flag_updates(server, page: Page):
    """Clicking star rating or flag buttons in inspector updates the photo and reflected on the grid card."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()

    # sunset.bmp initial rating is 4, flag is 0
    expect(card.locator(".card-stars span.active")).to_have_count(4)
    expect(page.locator("#inspectorRating span.active")).to_have_count(4)

    # Click 2nd star in inspector to change rating to 2
    page.locator('#inspectorRating span[data-star="2"]').click()
    expect(page.locator("#inspectorRating span.active")).to_have_count(2)
    expect(card.locator(".card-stars span.active")).to_have_count(2)

    # Click Reject button in inspector
    expect(card.locator(".flag-badge")).to_have_count(0)
    page.locator("#inspectorFlag .flag-reject").click()
    expect(card.locator(".flag-badge.reject")).to_be_visible()
    expect(page.locator("#inspectorFlag .flag-reject")).to_have_class(re.compile(r"\bactive\b"))

    # Click Reject again to toggle off (unflag)
    page.locator("#inspectorFlag .flag-reject").click()
    expect(card.locator(".flag-badge")).to_have_count(0)
    expect(page.locator("#inspectorFlag .flag-reject")).not_to_have_class(re.compile(r"\bactive\b"))


def test_inspector_tags_inline_add_and_remove(server, page: Page):
    """Adding a new tag inline in inspector and removing a tag badge."""
    page.goto(server["url"])

    card = page.locator(".photo-card", has_text="mountain.bmp")
    card.click()

    tags_container = page.locator("#inspectorTags")
    # Pre-seeded tags: Alps, Sunset
    expect(tags_container).to_contain_text("Alps")
    expect(tags_container).to_contain_text("Sunset")

    # Add tag "Dolomites" inline
    page.locator("#addTagInput").fill("Dolomites")
    page.locator("#addTagBtn").click()

    # Verify "Dolomites" tag badge is created
    dolomites_badge = tags_container.locator(".tag-badge", has_text="Dolomites")
    expect(dolomites_badge).to_be_visible()

    # Remove "Dolomites" tag badge
    dolomites_badge.locator(".remove-tag").click()
    expect(tags_container.locator(".tag-badge", has_text="Dolomites")).to_have_count(0)
