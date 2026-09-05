"""
Deterministic UI tests for Batch Actions.
"""

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
