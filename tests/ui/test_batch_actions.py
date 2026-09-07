"""
Deterministic UI tests for Batch Actions.
"""

import os
import re
from playwright.sync_api import Page, expect


def test_batch_action_bar_visibility_and_selection_count(server, page: Page):
    """Batch action bar appears when >= 2 cards are selected and updates count."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    bar = page.locator("#batchActionBar")
    expect(bar).to_be_hidden()

    # Select 1 card -> batch bar still hidden
    cards.nth(0).click()
    expect(bar).to_be_hidden()

    # Select 2nd card with Ctrl -> batch bar appears
    cards.nth(1).click(modifiers=["Control"])
    expect(bar).to_be_visible()
    expect(page.locator("#batchSelectedCount")).to_have_text("2 selected")

    # Select 3rd card with Ctrl -> count updates to 3
    cards.nth(2).click(modifiers=["Control"])
    expect(page.locator("#batchSelectedCount")).to_have_text("3 selected")

    # Click batch deselect button -> all cards deselected, bar hidden
    page.locator("#batchClearBtn").click()
    expect(bar).to_be_hidden()
    expect(page.locator(".photo-card.selected")).to_have_count(0)


def test_batch_pick_and_reject(server, page: Page):
    """Batch Pick and Reject updates flag on all selected cards."""
    page.goto(server["url"])

    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="birthday.bmp")

    # Select both cards
    card1.click()
    card2.click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # Click Batch Pick
    page.locator("#batchPickBtn").click()
    expect(card1.locator(".flag-badge.pick")).to_be_visible()
    expect(card2.locator(".flag-badge.pick")).to_be_visible()

    # Click Batch Reject
    page.locator("#batchRejectBtn").click()
    expect(card1.locator(".flag-badge.reject")).to_be_visible()
    expect(card2.locator(".flag-badge.reject")).to_be_visible()


def test_batch_rating(server, page: Page):
    """Batch Rating sets the star rating for all selected cards."""
    page.goto(server["url"])

    card1 = page.locator(".photo-card", has_text="birthday.bmp")
    card2 = page.locator(".photo-card", has_text="forest.bmp")

    # Select both cards (both initially rating 0)
    card1.click()
    card2.click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # Click 4th star in batch rating widget
    page.locator('#batchRating span[data-star="4"]').click()
    expect(card1.locator(".card-stars span.active")).to_have_count(4)
    expect(card2.locator(".card-stars span.active")).to_have_count(4)


def test_batch_add_tag(server, page: Page):
    """Batch Add Tag attaches keyword tag to all selected photos via Add Tag dialog."""
    page.goto(server["url"])

    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="beach.bmp")

    card1.click()
    card2.click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # Click Batch Add Tag -> opens modal
    page.locator("#batchAddTagBtn").click()
    modal = page.locator("#newTagModal")
    expect(modal).to_be_visible()
    expect(page.locator("#tagModalHeading")).to_have_text("Add Tag to 2 Photos")

    page.locator("#tagNameInput").fill("WeekendTrip")
    page.locator("#createTagSubmitBtn").click()
    expect(modal).to_be_hidden()

    # Inspect card 1: verify tag is present
    card1.click()
    expect(page.locator("#inspectorTags")).to_contain_text("WeekendTrip")

    # Inspect card 2: verify tag is present
    card2.click()
    expect(page.locator("#inspectorTags")).to_contain_text("WeekendTrip")


def test_batch_operations_single_http_call(server, page: Page):
    """Batch rating, flag, and tag actions send a single batch HTTP request rather than N individual calls."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Select 3 photos
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])
    cards.nth(2).click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    api_calls = []
    page.on("request", lambda r: api_calls.append(r.url) if "/api/" in r.url else None)

    # 1. Batch Flag
    api_calls.clear()
    page.locator("#batchPickBtn").click()
    page.wait_for_timeout(300)
    flag_calls = [url for url in api_calls if "flag" in url]
    assert len(flag_calls) == 1
    assert "/api/media/batch-flag" in flag_calls[0]

    # 2. Batch Rating
    api_calls.clear()
    page.locator('#batchRating span[data-star="5"]').click()
    page.wait_for_timeout(300)
    rating_calls = [url for url in api_calls if "rating" in url]
    assert len(rating_calls) == 1
    assert "/api/media/batch-rating" in rating_calls[0]

    # 3. Batch Tag
    api_calls.clear()
    page.locator("#batchAddTagBtn").click()
    page.locator("#tagNameInput").fill("SingleBatchTag")
    page.locator("#createTagSubmitBtn").click()
    expect(page.locator("#newTagModal")).to_be_hidden()
    page.wait_for_timeout(300)
    tag_calls = [url for url in api_calls if "batch-tags" in url]
    assert len(tag_calls) == 1
    assert "/api/media/batch-tags" in tag_calls[0]


def test_batch_move_dialog_open_and_cancel(server, page: Page):
    """Batch Move button opens dialog asking user for path and can be closed/cancelled."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Select 2 cards
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # Move button is in the batch toolbar
    move_btn = page.locator("#batchMoveBtn")
    expect(move_btn).to_be_visible()

    # Click Move -> opens dialog
    move_btn.click()
    modal = page.locator("#batchMoveModal")
    expect(modal).to_be_visible()
    expect(page.locator("#batchMoveTargetCount")).to_contain_text("2 selected photos")

    # Cancel button closes dialog
    page.locator("#cancelBatchMoveBtn").click()
    expect(modal).to_be_hidden()

    # Reopen and test Escape key closes dialog
    move_btn.click()
    expect(modal).to_be_visible()
    page.keyboard.press("Escape")
    expect(modal).to_be_hidden()


def test_batch_move_photos_to_new_path(server, page: Page):
    """Batch move asks user for path and moves media to new path, updating catalog."""
    page.goto(server["url"])

    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="mountain.bmp")

    # Select both cards
    card1.click()
    card2.click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    # Click Move button
    page.locator("#batchMoveBtn").click()
    modal = page.locator("#batchMoveModal")
    expect(modal).to_be_visible()

    # Enter new destination path
    page.locator("#batchMovePathInput").fill("nature/vacation")
    with page.expect_response(lambda r: "/api/media/batch-move" in r.url) as response_info:
        page.locator("#confirmBatchMoveBtn").click()
    response = response_info.value
    expect(modal).to_be_hidden()

    # Wait for completion and verify toast or update
    expect(page.locator(".toast-success")).to_be_visible()
    expect(page.locator(".toast-success")).to_contain_text("Moved 2 photos to nature/vacation")

    # Folders sidebar should now list the new folder
    page.wait_for_timeout(500)
    expect(page.locator("#foldersTree")).to_contain_text("vacation")

    # Verify files moved on disk within test environment photos directory
    photos_dir = server["env"]["photos_dir"]
    assert os.path.isfile(os.path.join(photos_dir, "nature", "vacation", "mountain.bmp"))
    assert os.path.isfile(os.path.join(photos_dir, "nature", "vacation", "sunset.bmp"))
    assert not os.path.exists(os.path.join(photos_dir, "nature", "mountain.bmp"))
    assert not os.path.exists(os.path.join(photos_dir, "nature", "sunset.bmp"))


def test_batch_move_outside_photos_dir_is_rejected(server, page: Page):
    """Batch move rejects paths outside the photos directory."""
    page.goto(server["url"])

    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="mountain.bmp")

    card1.click()
    card2.click(modifiers=["Control"])
    expect(page.locator("#batchActionBar")).to_be_visible()

    page.locator("#batchMoveBtn").click()
    modal = page.locator("#batchMoveModal")
    expect(modal).to_be_visible()

    # Enter path that navigates outside photos directory
    page.locator("#batchMovePathInput").fill("../../outside_dir")
    page.locator("#confirmBatchMoveBtn").click()

    expect(page.locator(".toast-error")).to_be_visible()
    expect(page.locator(".toast-error")).to_contain_text("inside the photos directory")

    # Modal stays open so user can fix the input without losing their work
    expect(modal).to_be_visible()

    # Photos do not disappear from media grid
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(card1).to_have_class(re.compile(r"\bselected\b"))
    expect(card2).to_have_class(re.compile(r"\bselected\b"))
    expect(page.locator("#batchSelectedCount")).to_have_text("2 selected")

    # User can cancel the modal and the selection is fully preserved
    page.locator("#cancelBatchMoveBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator(".photo-card")).to_have_count(6)
    expect(card1).to_have_class(re.compile(r"\bselected\b"))
    expect(card2).to_have_class(re.compile(r"\bselected\b"))




