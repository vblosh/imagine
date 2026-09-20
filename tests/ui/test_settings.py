"""Settings dialog and browser-side API authentication coverage."""

from playwright.sync_api import Page, expect


def test_api_token_settings_persist_and_attach_auth_header(server, page: Page):
    """The Settings dialog stores the token and API requests use it as a Bearer token."""
    page.goto(server["url"])
    page.evaluate("() => localStorage.removeItem('imagine_api_token')")
    page.reload()

    page.locator("#settingsBtn").click()
    modal = page.locator("#settingsModal")
    expect(modal).to_be_visible()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("")

    page.locator("#settingsApiTokenInput").fill("browser-secret")
    page.locator("#saveSettingsBtn").click()
    expect(modal).to_be_hidden()
    assert page.evaluate("() => localStorage.getItem('imagine_api_token')") == "browser-secret"

    with page.expect_request("**/api/stats") as request_info:
        page.evaluate("""async () => {
            try {
                await window._imagineApp.api.get('/api/stats');
            } catch (_) {}
        }""")
    assert request_info.value.headers.get("authorization") == "Bearer browser-secret"

    page.locator("#settingsBtn").click()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("browser-secret")
    page.locator("#clearApiTokenBtn").click()
    expect(page.locator("#settingsApiTokenInput")).to_have_value("")
    assert page.evaluate("() => localStorage.getItem('imagine_api_token')") is None

    with page.expect_request("**/api/stats") as cleared_request_info:
        page.evaluate("""async () => {
            try {
                await window._imagineApp.api.get('/api/stats');
            } catch (_) {}
        }""")
    assert "authorization" not in cleared_request_info.value.headers
