"""
Deterministic UI tests for Keyword Tags management, categories accordion, and filtering.
"""

import re
from playwright.sync_api import Page, expect


def test_tag_category_accordion_toggle(server, page: Page):
    """Clicking category header collapses and expands the tag items list."""
    page.goto(server["url"])

    people_group = page.locator('.tag-category-group[data-category="people"]')
    header = people_group.locator(".category-header")
    items = people_group.locator(".tag-items")
    arrow = people_group.locator(".arrow")

    # Initially items are visible, arrow is ▼
    expect(items).to_be_visible()
    expect(arrow).to_have_text("▼")

    # Click header to collapse
    header.click()
    expect(items).to_be_hidden()
    expect(arrow).to_have_text("▶")

    # Click header to expand again
    header.click()
    expect(items).to_be_visible()
    expect(arrow).to_have_text("▼")


def test_tags_sidebar_rendering_and_filtering(server, page: Page):
    """Verify pre-seeded tags are rendered in correct categories and clicking a tag filters the media grid."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # Verify pre-seeded tags in their respective categories
    expect(page.locator("#tagCategoryPeople")).to_contain_text("Alice")
    expect(page.locator("#tagCategoryPeople")).to_contain_text("Bob")
    expect(page.locator("#tagCategoryPlaces")).to_contain_text("Alps")
    expect(page.locator("#tagCategoryPlaces")).to_contain_text("Beach")
    expect(page.locator("#tagCategoryEvents")).to_contain_text("Birthday 2026")
    expect(page.locator("#tagCategoryKeyword")).to_contain_text("Sunset")

    # Click tag "Sunset" (in keyword category)
    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    expect(sunset_tag.locator(".count-badge")).to_have_text("2")
    sunset_tag.click()

    # Filtered to 2 photos with Sunset tag: mountain.bmp and sunset.bmp
    expect(page.locator("#filterLabel")).to_contain_text("Tag: Sunset")
    expect(cards).to_have_count(2)
    names = [cards.nth(0).locator(".card-filename").text_content(), cards.nth(1).locator(".card-filename").text_content()]
    assert "mountain.bmp" in names
    assert "sunset.bmp" in names

    # Click again to toggle filter off
    sunset_tag.click()
    expect(cards).to_have_count(6)


def test_create_tag_modal_validation_and_creation(server, page: Page):
    """Test Create Tag modal dialog, validation on empty name, and successful tag creation."""
    page.goto(server["url"])

    modal = page.locator("#newTagModal")
    expect(modal).to_be_hidden()

    # Open modal
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()

    # Validation: empty name triggers alert
    dialog_messages = []
    page.on("dialog", lambda dialog: (dialog_messages.append(dialog.message), dialog.accept()))

    page.locator("#createTagSubmitBtn").click()
    assert len(dialog_messages) == 1
    assert "Tag name is required" in dialog_messages[0]
    expect(modal).to_be_visible()

    # Fill valid tag details
    page.locator("#tagNameInput").fill("Skiing")
    page.locator("#tagCategorySelect").select_option("events")
    page.locator("#createTagSubmitBtn").click()

    # Modal closes
    expect(modal).to_be_hidden()

    # Verify tag appears in Events category
    new_tag = page.locator("#tagCategoryEvents .tag-item", has_text="Skiing")
    expect(new_tag).to_be_visible()
    expect(new_tag.locator(".count-badge")).to_have_text("0")


def test_create_tag_modal_cancel_and_backdrop(server, page: Page):
    """Test closing tag modal via cancel button, close button, and backdrop click."""
    page.goto(server["url"])

    modal = page.locator("#newTagModal")

    # 1. Cancel button
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#cancelTagBtn").click()
    expect(modal).to_be_hidden()

    # 2. Close icon button
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#closeNewTagModalBtn").click()
    expect(modal).to_be_hidden()

    # 3. Backdrop click
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()
    page.locator("#newTagBackdrop").click(position={"x": 10, "y": 10})
    expect(modal).to_be_hidden()


def test_delete_tag(server, page: Page):
    """Deleting a tag sends DELETE /api/tags/:id, removes it from the sidebar, and resets filter if active."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Create a temporary tag to delete
    page.locator("#newTagBtn").click()
    page.locator("#tagNameInput").fill("ToDelete Tag")
    page.locator("#tagCategorySelect").select_option("keyword")
    page.locator("#createTagSubmitBtn").click()

    tag_item = page.locator("#tagCategoryKeyword .tag-item", has_text="ToDelete Tag")
    expect(tag_item).to_be_visible()

    modal = page.locator("#deleteTagModal")
    expect(modal).to_be_hidden()

    # Dismissing / canceling deletion
    tag_item.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    expect(page.locator("#deleteTagPromptText")).to_contain_text('Are you sure you want to delete tag "ToDelete Tag"?')
    page.locator("#cancelDeleteTagBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator("#tagCategoryKeyword .tag-item", has_text="ToDelete Tag")).to_be_visible()

    # Accepting confirm deletes the tag
    tag_item.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.locator("#confirmDeleteTagBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator("#tagCategoryKeyword .tag-item", has_text="ToDelete Tag")).to_have_count(0)

    # 2. Delete an active tag and verify filter resets
    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    sunset_tag.click()
    expect(page.locator("#filterLabel")).to_contain_text("Tag: Sunset")
    expect(cards).to_have_count(2)

    # Delete the active "Sunset" tag with confirmation modal
    sunset_tag.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    expect(page.locator("#deleteTagPromptText")).to_contain_text('Are you sure you want to delete tag "Sunset"?')
    page.locator("#confirmDeleteTagBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")).to_have_count(0)
    # Filter resets back to all media
    expect(cards).to_have_count(6)


def test_delete_tag_modal_close_and_backdrop_and_escape(server, page: Page):
    """Test closing delete tag modal via close icon, backdrop, and Escape key."""
    page.goto(server["url"])

    modal = page.locator("#deleteTagModal")
    expect(modal).to_be_hidden()

    sunset_tag = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")

    # 1. Close icon button
    sunset_tag.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.locator("#closeDeleteTagModalBtn").click()
    expect(modal).to_be_hidden()
    expect(sunset_tag).to_be_visible()

    # 2. Backdrop click
    sunset_tag.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.locator("#deleteTagBackdrop").click(position={"x": 10, "y": 10})
    expect(modal).to_be_hidden()
    expect(sunset_tag).to_be_visible()

    # 3. Escape key
    sunset_tag.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.keyboard.press("Escape")
    expect(modal).to_be_hidden()
    expect(sunset_tag).to_be_visible()


def test_delete_tag_removes_from_sidebar_and_photos(server, page: Page):
    """Verify deleting a tag from the left panel removes it from sidebar and from photo metadata in inspector."""
    page.goto(server["url"])

    # 1. Select mountain.bmp which is tagged with 'Alps' and 'Sunset'
    mountain_card = page.locator(".photo-card", has_text="mountain.bmp")
    mountain_card.click()

    inspector_tags = page.locator("#inspectorTags")
    expect(inspector_tags.locator(".tag-badge", has_text="Sunset")).to_be_visible()
    expect(inspector_tags.locator(".tag-badge", has_text="Alps")).to_be_visible()

    # Verify Sunset is present in the left Keywords panel with count 2
    sunset_item = page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")
    expect(sunset_item).to_be_visible()
    expect(sunset_item.locator(".count-badge")).to_have_text("2")

    modal = page.locator("#deleteTagModal")

    # 2. Delete "Sunset" from the left Keywords tag panel
    sunset_item.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.locator("#confirmDeleteTagBtn").click()
    expect(modal).to_be_hidden()

    # Verify "Sunset" is completely gone from the left sidebar
    expect(page.locator("#tagCategoryKeyword .tag-item", has_text="Sunset")).to_have_count(0)

    # Verify "Sunset" was removed from the active photo in inspector (Alps remains)
    expect(inspector_tags.locator(".tag-badge", has_text="Sunset")).to_have_count(0)
    expect(inspector_tags.locator(".tag-badge", has_text="Alps")).to_be_visible()

    # 3. Select sunset.bmp which was previously tagged only with "Sunset"
    sunset_card = page.locator(".photo-card", has_text="sunset.bmp")
    sunset_card.click()

    # sunset.bmp should now have no tags at all since Sunset was deleted from catalog
    expect(inspector_tags.locator(".tag-badge", has_text="Sunset")).to_have_count(0)
    expect(inspector_tags).to_contain_text("No tags")

    # 4. Select beach.bmp which has "Beach" tag
    beach_card = page.locator(".photo-card", has_text="beach.bmp")
    beach_card.click()
    expect(inspector_tags.locator(".tag-badge", has_text="Beach")).to_be_visible()

    # Detach tag from photo via inspector (detach from photo, retain in sidebar)
    beach_badge = inspector_tags.locator(".tag-badge", has_text="Beach")
    beach_badge.locator(".remove-tag").click()

    # Removed from photo inspector
    expect(inspector_tags.locator(".tag-badge", has_text="Beach")).to_have_count(0)
    expect(inspector_tags).to_contain_text("No tags")

    # Still present in sidebar under Places, but count dropped from 1 to 0
    beach_item = page.locator("#tagCategoryPlaces .tag-item", has_text="Beach")
    expect(beach_item).to_be_visible()
    expect(beach_item.locator(".count-badge")).to_have_text("0")

    # 5. Delete "Beach" from sidebar as well
    beach_item.locator(".delete-tag-btn").click()
    expect(modal).to_be_visible()
    page.locator("#confirmDeleteTagBtn").click()
    expect(modal).to_be_hidden()
    expect(page.locator("#tagCategoryPlaces .tag-item", has_text="Beach")).to_have_count(0)


def test_add_tag_with_category_to_photo(server, page: Page):
    """Add a tag to a photo with a specific category (places) and verify inspector badge, sidebar group, filtering, and auto-category selection."""
    page.goto(server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)

    # 1. Select portrait.bmp
    portrait_card = page.locator(".photo-card", has_text="portrait.bmp")
    portrait_card.click()

    inspector_tags = page.locator("#inspectorTags")
    expect(inspector_tags.locator(".tag-badge", has_text="Bob")).to_be_visible()

    # 2. Add tag "Kyoto" with category "places"
    page.locator("#addTagInput").fill("Kyoto")
    page.locator("#addTagCategorySelect").select_option("places")
    page.locator("#addTagBtn").click()

    # 3. Verify Kyoto badge appears in photo's inspector tags with places category
    kyoto_badge = inspector_tags.locator(".tag-badge", has_text="Kyoto")
    expect(kyoto_badge).to_be_visible()
    expect(kyoto_badge).to_have_attribute("data-category", "places")

    # 4. Verify Kyoto is added to the Places category in the left sidebar
    kyoto_sidebar_item = page.locator("#tagCategoryPlaces .tag-item", has_text="Kyoto")
    expect(kyoto_sidebar_item).to_be_visible()
    expect(kyoto_sidebar_item.locator(".count-badge")).to_have_text("1")

    # 5. Click Kyoto in Places to filter the grid
    kyoto_sidebar_item.click()
    expect(page.locator("#filterLabel")).to_contain_text("Tag: Kyoto")
    expect(cards).to_have_count(1)
    expect(cards.nth(0).locator(".card-filename")).to_have_text("portrait.bmp")

    # 6. Click again to unfilter
    kyoto_sidebar_item.click()
    expect(cards).to_have_count(6)

    # 7. Select forest.bmp and type existing tag "Beach" (which is in Places)
    forest_card = page.locator(".photo-card", has_text="forest.bmp")
    forest_card.click()
    page.locator("#addTagInput").fill("Beach")
    page.locator("#addTagInput").dispatch_event("input")
    expect(page.locator("#addTagCategorySelect")).to_have_value("places")
    page.locator("#addTagBtn").click()

    # Verify "Beach" tag is now attached to forest.bmp
    expect(inspector_tags.locator(".tag-badge", has_text="Beach")).to_be_visible()
    # Verify count for Beach increased to 2 (beach.bmp + forest.bmp)
    expect(page.locator("#tagCategoryPlaces .tag-item", has_text="Beach").locator(".count-badge")).to_have_text("2")


def test_tag_modal_category_cards_and_search_help(server, page: Page):
    """Test category cards selection (People, Events, Places, Keyword) and search help chips in Add Tag modal."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    modal = page.locator("#newTagModal")
    page.locator("#newTagBtn").click()
    expect(modal).to_be_visible()

    # Default category is Keyword
    expect(page.locator("#catBtnKeyword")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("keyword")
    # Quick pick chips show Sunset in Keywords
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Sunset")

    # Click People category card
    page.locator("#catBtnPeople").click()
    expect(page.locator("#catBtnPeople")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("people")
    # Quick pick chips now show Alice and Bob
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Alice")
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Bob")

    # Click Events category card
    page.locator("#catBtnEvents").click()
    expect(page.locator("#catBtnEvents")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("events")
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Birthday 2026")

    # Click Places category card
    page.locator("#catBtnPlaces").click()
    expect(page.locator("#catBtnPlaces")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("places")
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Alps")
    expect(page.locator("#tagSearchHelpChips")).to_contain_text("Beach")

    # Click a search help chip (e.g. Alps)
    page.locator('#tagSearchHelpChips .tag-help-chip[data-tag-name="Alps"]').click()
    expect(page.locator("#tagNameInput")).to_have_value("Alps")

    # Clear button clears input
    expect(page.locator("#tagModalClearInputBtn")).to_be_visible()
    page.locator("#tagModalClearInputBtn").click()
    expect(page.locator("#tagNameInput")).to_have_value("")

    page.locator("#cancelTagBtn").click()
    expect(modal).to_be_hidden()


def test_tag_modal_photo_target_and_tagging_from_inspector(server, page: Page):
    """Opening Add Tag modal from inspector shows photo preview target and attaches tag to photo."""
    page.goto(server["url"])

    # 1. Select portrait.bmp
    portrait_card = page.locator(".photo-card", has_text="portrait.bmp")
    portrait_card.click()

    # 2. Click inspectorAddTagModalBtn (+) next to Tags header
    page.locator("#inspectorAddTagModalBtn").click()
    modal = page.locator("#newTagModal")
    expect(modal).to_be_visible()

    # 3. Photo target card is visible with photo details
    target_card = page.locator("#tagModalPhotoTarget")
    expect(target_card).to_be_visible()
    expect(page.locator("#tagModalPhotoName")).to_contain_text("portrait.bmp")
    expect(page.locator("#tagModalHeading")).to_have_text("Add Tag to Photo")
    expect(page.locator("#tagModalApplyToPhotoCheckbox")).to_be_checked()

    # 4. Fill tag and category
    page.locator("#tagNameInput").fill("BestFriend")
    page.locator("#catBtnPeople").click()
    page.locator("#createTagSubmitBtn").click()
    expect(modal).to_be_hidden()

    # 5. Verify tag attached to photo in inspector
    inspector_tags = page.locator("#inspectorTags")
    expect(inspector_tags.locator('.tag-badge[data-category="people"]', has_text="BestFriend")).to_be_visible()

    # 6. Verify tag added to People category in sidebar with count 1
    people_item = page.locator("#tagCategoryPeople .tag-item", has_text="BestFriend")
    expect(people_item).to_be_visible()
    expect(people_item.locator(".count-badge")).to_have_text("1")


def test_tag_modal_auto_detect_category_on_type(server, page: Page):
    """Typing an existing tag name in Add Tag modal automatically detects and selects its category."""
    page.goto(server["url"])
    expect(page.locator(".photo-card")).to_have_count(6)

    page.locator("#newTagBtn").click()
    modal = page.locator("#newTagModal")
    expect(modal).to_be_visible()

    # Type "Alice" (pre-seeded People tag)
    page.locator("#tagNameInput").fill("Alice")
    expect(page.locator("#catBtnPeople")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("people")
    expect(page.locator("#tagDetectedBadge")).to_contain_text("Existing in People")

    # Type "Birthday 2026" (pre-seeded Events tag)
    page.locator("#tagNameInput").fill("Birthday 2026")
    expect(page.locator("#catBtnEvents")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#tagCategorySelect")).to_have_value("events")
    expect(page.locator("#tagDetectedBadge")).to_contain_text("Existing in Events")

    page.locator("#cancelTagBtn").click()
    expect(modal).to_be_hidden()



