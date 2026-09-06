"""
Deterministic UI tests for video and audio media types, card badges, sidebar navigation, inspector, and loupe player.
"""

import re
from playwright.sync_api import Page, expect


def test_media_sidebar_and_card_badges(media_server, page: Page):
    """Test sidebar filters for Photos, Videos, and Audio, along with card badges and duration pills."""
    page.goto(media_server["url"])

    cards = page.locator(".photo-card")
    expect(cards).to_have_count(8)

    # 1. Verify count badges in sidebar
    expect(page.locator("#totalMediaCount")).to_have_text("8")
    expect(page.locator("#totalPhotosCount")).to_have_text("6")
    expect(page.locator("#totalVideosCount")).to_have_text("1")
    expect(page.locator("#totalAudioCount")).to_have_text("1")

    # 2. Verify video card badge and duration
    video_card = page.locator(".photo-card", has_text="drone_flight.mp4")
    expect(video_card).to_be_visible()
    expect(video_card.locator(".media-type-badge.video-badge")).to_be_visible()
    expect(video_card.locator(".video-badge")).to_contain_text("0:15")

    # 3. Verify audio card badge and duration
    audio_card = page.locator(".photo-card", has_text="ambient_track.wav")
    expect(audio_card).to_be_visible()
    expect(audio_card.locator(".media-type-badge.audio-badge")).to_be_visible()
    expect(audio_card.locator(".audio-badge")).to_contain_text("0:45")

    # 4. Filter by Photos
    page.locator("#navPhotos").click()
    expect(page.locator("#navPhotos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Photos")
    expect(cards).to_have_count(6)
    for i in range(6):
        expect(cards.nth(i)).to_have_attribute("data-media-type", "photo")

    # 5. Filter by Videos
    page.locator("#navVideos").click()
    expect(page.locator("#navVideos")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Videos")
    expect(cards).to_have_count(1)
    expect(cards.first).to_have_attribute("data-media-type", "video")
    expect(cards.first.locator(".card-filename")).to_have_text("drone_flight.mp4")

    # 6. Filter by Audio
    page.locator("#navAudio").click()
    expect(page.locator("#navAudio")).to_have_class(re.compile(r"\bactive\b"))
    expect(page.locator("#filterLabel")).to_contain_text("Audio")
    expect(cards).to_have_count(1)
    expect(cards.first).to_have_attribute("data-media-type", "audio")
    expect(cards.first.locator(".card-filename")).to_have_text("ambient_track.wav")

    # 7. Return to All Media
    page.locator("#navAllMedia").click()
    expect(page.locator("#navAllMedia")).to_have_class(re.compile(r"\bactive\b"))
    expect(cards).to_have_count(8)


def test_media_inspector_panel(media_server, page: Page):
    """Test metadata inspector for video and audio media files."""
    page.goto(media_server["url"])

    # Select video card
    video_card = page.locator(".photo-card", has_text="drone_flight.mp4")
    video_card.click()

    expect(page.locator("#rightInspector")).to_be_visible()
    expect(page.locator("#inspectorSelection")).to_be_visible()
    expect(page.locator("#infoFileName")).to_have_text("drone_flight.mp4")
    expect(page.locator("#infoMediaType")).to_have_text("Video")
    expect(page.locator("#avSection")).to_be_visible()
    expect(page.locator("#infoDuration")).to_have_text("0:15")
    expect(page.locator("#infoCodec")).to_have_text("h264")
    expect(page.locator("#infoDimensions")).to_have_text("1920 × 1080 px")

    # Select audio card
    audio_card = page.locator(".photo-card", has_text="ambient_track.wav")
    audio_card.click()

    expect(page.locator("#infoFileName")).to_have_text("ambient_track.wav")
    expect(page.locator("#infoMediaType")).to_have_text("Audio")
    expect(page.locator("#avSection")).to_be_visible()
    expect(page.locator("#infoDuration")).to_have_text("0:45")
    expect(page.locator("#infoArtist")).to_have_text("SynthWave Artist")
    expect(page.locator("#infoTitle")).to_have_text("Night Drive")
    expect(page.locator("#infoAlbum")).to_have_text("Neon City")
    expect(page.locator("#infoAudioDetails")).to_contain_text("Mono")
    expect(page.locator("#infoAudioDetails")).to_contain_text("44.1 kHz")
    expect(page.locator("#exifSection")).to_be_hidden()


def test_media_loupe_player(media_server, page: Page):
    """Test loupe video/audio players and keyboard spacebar play/pause toggling."""
    page.goto(media_server["url"])

    loupe = page.locator("#loupeModal")
    expect(loupe).to_be_hidden()

    # 1. Open video card in loupe
    video_card = page.locator(".photo-card", has_text="drone_flight.mp4")
    video_card.dblclick()

    expect(loupe).to_be_visible()
    expect(page.locator("#loupeVideo")).to_be_visible()
    expect(page.locator("#loupeImg")).to_be_hidden()

    # Toggle spacebar in loupe
    page.keyboard.press("Space")
    # Close loupe with Escape
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()

    # 2. Open audio card in loupe
    audio_card = page.locator(".photo-card", has_text="ambient_track.wav")
    audio_card.dblclick()

    expect(loupe).to_be_visible()
    expect(page.locator("#loupeAudioContainer")).to_be_visible()
    expect(page.locator("#loupeAudio")).to_be_visible()
    expect(page.locator("#loupeAudioTitle")).to_have_text("Night Drive")
    expect(page.locator("#loupeAudioArtist")).to_contain_text("SynthWave Artist")
    expect(page.locator("#loupeImg")).to_be_hidden()

    # Spacebar toggle
    page.keyboard.press("Space")
    # Close loupe
    page.keyboard.press("Escape")
    expect(loupe).to_be_hidden()
