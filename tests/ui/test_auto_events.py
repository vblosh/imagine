"""Deterministic UI tests for local Auto Event suggestions."""

import sqlite3

from playwright.sync_api import Page, expect


def select_two_photos(page: Page):
    cards = page.locator(".photo-card")
    expect(cards).to_have_count(6)
    cards.nth(0).click()
    cards.nth(1).click(modifiers=["Control"])
    expect(page.locator("#batchAutoEventBtn")).to_be_visible()


def test_auto_event_preview_is_reviewable_and_cancel_is_read_only(server, page: Page):
    with sqlite3.connect(server["env"]["db_path"]) as conn:
        initial_event_count = conn.execute("SELECT COUNT(*) FROM media_tags mt JOIN tags t ON t.id = mt.tag_id WHERE t.category = 'events'").fetchone()[0]
    page.goto(server["url"])
    select_two_photos(page)

    with page.expect_response(lambda response: response.url.endswith("/api/media/event-suggestions") and response.request.method == "POST") as response_info:
        page.locator("#batchAutoEventBtn").click()
    assert response_info.value.ok

    modal = page.locator("#autoEventModal")
    expect(modal).to_be_visible()
    expect(page.locator("#autoEventLoading")).to_be_hidden()
    expect(page.locator("#cancelAutoEventsBtn")).to_be_visible()
    dialog_style = page.locator(".auto-event-dialog").evaluate("el => ({ resize: getComputedStyle(el).resize, width: el.getBoundingClientRect().width })")
    assert dialog_style["resize"] == "both"
    assert dialog_style["width"] >= 480
    page.locator("#cancelAutoEventsBtn").click()
    expect(modal).to_be_hidden()

    with sqlite3.connect(server["env"]["db_path"]) as conn:
        event_count = conn.execute("SELECT COUNT(*) FROM media_tags mt JOIN tags t ON t.id = mt.tag_id WHERE t.category = 'events'").fetchone()[0]
    assert event_count == initial_event_count


def test_auto_event_group_and_photo_controls(server, page: Page):
    page.goto(server["url"])
    select_two_photos(page)
    with page.expect_response(lambda response: response.url.endswith("/api/media/event-suggestions")):
        page.locator("#batchAutoEventBtn").click()
    expect(page.locator("#autoEventModal")).to_be_visible()
    expect(page.locator("#autoEventLoading")).to_be_hidden()

    groups = page.locator(".auto-event-group")
    if groups.count() == 0:
        expect(page.locator("#autoEventEmpty")).to_be_visible()
        return
    first = groups.first
    name = first.locator(".auto-event-name")
    expect(name).to_be_visible()
    original = name.input_value()
    name.fill(original + " reviewed")
    expect(name).to_have_value(original + " reviewed")
    first.locator(".auto-event-toggle-photos").click()
    expect(first.locator(".auto-event-photo").first).to_be_visible()
    first.locator(".auto-event-photo-checkbox").first.uncheck()
    expect(first.locator(".auto-event-photo-checkbox").first).not_to_be_checked()
    expect(first.locator(".auto-event-photo-checkbox").first).to_be_focused()
    first.locator(".auto-event-photo-checkbox").first.check()
    first.locator(".auto-event-group-checkbox").uncheck()
    expect(first.locator(".auto-event-group-checkbox")).not_to_be_checked()
    expect(first.locator(".auto-event-group-checkbox")).to_be_focused()
    page.locator("#cancelAutoEventsBtn").click()


def test_auto_event_available_for_single_selection(server, page: Page):
    page.goto(server["url"])
    card = page.locator(".photo-card").first
    card.click()
    expect(page.locator("#inspectorAutoEventBtn")).to_be_visible()
    with page.expect_response(lambda response: response.url.endswith("/api/media/event-suggestions")):
        page.locator("#inspectorAutoEventBtn").click()
    expect(page.locator("#autoEventModal")).to_be_visible()
    page.locator("#cancelAutoEventsBtn").click()


def test_auto_event_apply_creates_event_tags(server, page: Page):
    with sqlite3.connect(server["env"]["db_path"]) as conn:
        initial_event_count = conn.execute("SELECT COUNT(*) FROM tags WHERE category = 'events'").fetchone()[0]
    page.goto(server["url"])
    select_two_photos(page)
    with page.expect_response(lambda response: response.url.endswith("/api/media/event-suggestions")):
        page.locator("#batchAutoEventBtn").click()
    expect(page.locator("#autoEventModal")).to_be_visible()
    expect(page.locator("#autoEventLoading")).to_be_hidden()
    if page.locator(".auto-event-group").count() == 0:
        page.locator("#cancelAutoEventsBtn").click()
        return
    with page.expect_response(lambda response: response.url.endswith("/api/media/batch-tags") and response.request.method == "POST"):
        page.locator("#applyAutoEventsBtn").click()
    expect(page.locator("#autoEventModal")).to_be_hidden()
    with sqlite3.connect(server["env"]["db_path"]) as conn:
        applied_event_count = conn.execute("SELECT COUNT(*) FROM tags WHERE category = 'events'").fetchone()[0]
    assert applied_event_count > initial_event_count


def test_auto_event_locks_review_while_applying_and_retries_failed_group(server, page: Page):
    page.goto(server["url"])
    page.evaluate("""async () => {
      const { api } = await import('./api.js');
      const original = api.post;
      window.eventCalls = [];
      api.post = async (url, body, ...rest) => {
        if (url.endsWith('/event-suggestions')) return { groups: [
          { name_source: 'event', name_value: 'First', start_date: 1700000000, end_date: 1700000000, photos: [{id: 1}] },
          { name_source: 'event', name_value: 'Second', start_date: 1700000000, end_date: 1700000000, photos: [{id: 2}] }
        ] };
        if (url.endsWith('/batch-tags')) {
          window.eventCalls.push(body);
          if (window.eventCalls.length === 1) return await new Promise(resolve => { window.finishFirstEvent = resolve; });
          if (window.eventCalls.length === 2) return { failed_ids: body.ids };
          return { failed_ids: [] };
        }
        return original(url, body, ...rest);
      };
      const events = await import('./auto-events.js');
      await events.openAutoEventForIds([1, 2]);
    }""")
    page.locator("#applyAutoEventsBtn").click()
    expect(page.locator(".auto-event-name")).to_have_count(2)
    for name in page.locator(".auto-event-name").all():
        expect(name).to_be_disabled()
    for checkbox in page.locator(".auto-event-group-checkbox").all():
        expect(checkbox).to_be_disabled()
    expect(page.locator("#cancelAutoEventsBtn")).to_be_disabled()
    page.evaluate("window.finishFirstEvent({failed_ids: []})")
    expect(page.locator("#retryAutoEventsBtn")).to_be_visible()
    expect(page.locator(".auto-event-name").first).to_be_disabled()
    page.locator(".auto-event-name").nth(1).fill("Second reviewed")
    page.locator("#retryAutoEventsBtn").click()
    expect(page.locator("#autoEventModal")).to_be_hidden()
    calls = page.evaluate("window.eventCalls")
    assert [(call["ids"], call["name"]) for call in calls] == [([1], "First"), ([2], "Second"), ([2], "Second reviewed")]


def test_interrupted_semantic_job_allows_new_scan(server, page: Page):
    page.goto(server["url"])
    result = page.evaluate("""async () => {
      const sem = await import('./semantic-search.js');
      const { api } = await import('./api.js');
      const { state } = await import('./state.js');
      const { dom } = await import('./dom.js');
      state.semanticSearchAvailable = true;
      let starts = 0;
      api.post = async url => url === '/api/semantic/jobs' ? {job_id: `job-${++starts}`} : {};
      api.get = async () => ({id: 'job-1', state: 'interrupted', total: 2, processed: 1});
      await sem.startSemanticIndexing();
      await sem.getSemanticJobProgress('job-1');
      const stopHidden = dom.semanticJobStopBtn.hidden;
      sem.openSemanticIndexModal();
      const startVisible = !dom.semanticJobStartBtn.hidden;
      await sem.startSemanticScan();
      return {stopHidden, startVisible, starts};
    }""")
    assert result == {"stopHidden": True, "startVisible": True, "starts": 2}
