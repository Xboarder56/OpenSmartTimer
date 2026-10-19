"""Landing visibility is not enrollment policy or a partner-access switch."""

import pytest


def test_defaults_and_settings_require_admin(client, admin_headers):
    assert client.get("/").status_code == 200
    assert client.get("/admin/site-settings").status_code == 401
    assert client.put("/admin/site-settings", json={"landing_page_visible": False}).status_code == 401
    state = client.get("/admin/site-settings", headers=admin_headers).json()
    assert state == {"landing_page_visible": True, "landing_page_allowed": True,
                     "landing_page_enabled": True}
    overview = client.get("/admin/overview", headers=admin_headers).json()
    assert all(overview[key] == value for key, value in state.items())


def test_hiding_home_keeps_partner_setup_and_private_preview(client, admin_headers, monkeypatch):
    from ostrelay import config
    monkeypatch.setattr(config._settings, "enrollment", "open")
    monkeypatch.setattr(config._settings, "spki_pin", "A" * 43)
    response = client.put("/admin/site-settings", headers=admin_headers,
                          json={"landing_page_visible": False})
    assert response.status_code == 200 and not response.json()["landing_page_enabled"]
    hidden = client.get("/")
    assert hidden.status_code == 404 and hidden.headers["cache-control"] == "no-store"
    for path in ("/setup", "/admin/", "/admin/landing-preview", "/api/v1/health"):
        assert client.get(path).status_code == 200
    preview = client.get("/admin/landing-preview")
    assert "Admin preview" in preview.text and 'href="/admin/"' in preview.text
    assert "unsafe-inline" not in preview.headers["content-security-policy"]
    assert preview.headers["cache-control"] == "no-store"
    assert client.put("/admin/site-settings", headers=admin_headers,
                      json={"landing_page_visible": True}).status_code == 200
    assert client.get("/").status_code == 200


def test_environment_is_hard_gate_and_does_not_erase_saved_preference(client, admin_headers, monkeypatch):
    from ostrelay import config
    monkeypatch.setattr(config._settings, "landing_page_enabled", False)
    state = client.get("/admin/site-settings", headers=admin_headers).json()
    assert state == {"landing_page_visible": True, "landing_page_allowed": False,
                     "landing_page_enabled": False}
    assert client.get("/").status_code == 404
    response = client.put("/admin/site-settings", headers=admin_headers,
                          json={"landing_page_visible": True})
    assert response.status_code == 409
    assert response.json()["detail"] == "landing_page_disabled_by_environment"
    monkeypatch.setattr(config._settings, "landing_page_enabled", True)
    assert client.get("/").status_code == 200


@pytest.mark.parametrize("body", [{}, {"landing_page_visible": "false"},
                                  {"landing_page_visible": 1},
                                  {"landing_page_visible": False, "enrollment": "open"}])
def test_settings_are_strict_and_do_not_change_enrollment(client, admin_headers, body):
    assert client.put("/admin/site-settings", headers=admin_headers, json=body).status_code == 422
    assert client.get("/").status_code == 200
    assert client.get("/setup").status_code == 404


def test_cross_origin_setting_write_is_rejected(client, admin_headers):
    result = client.put("/admin/site-settings", json={"landing_page_visible": False},
                        headers={**admin_headers, "Origin": "https://other.example"})
    assert result.status_code == 403
    assert client.get("/").status_code == 200


def test_saved_setting_survives_database_reopen(fresh_settings, tmp_path, monkeypatch):
    from ostrelay import config, db, site_settings
    monkeypatch.setattr(config._settings, "db_path", str(tmp_path / "relay.sqlite"))
    monkeypatch.setattr(db, "_conn", None)
    try:
        site_settings.set_landing_page_visible(False)
        db.connect().close()
        db._conn = None
        assert site_settings.landing_page_settings()["landing_page_visible"] is False
        # Initializing the schema again does not overwrite an operator choice.
        assert site_settings.landing_page_settings()["landing_page_enabled"] is False
    finally:
        db.connect().close()
        db._conn = None


def test_environment_variable_is_supported(monkeypatch):
    from ostrelay.config import Settings
    monkeypatch.setenv("OSTRELAY_LANDING_PAGE_ENABLED", "false")
    assert Settings(_env_file=None).landing_page_enabled is False


def test_admin_contains_setting_and_preview_but_no_management_links_on_root(client):
    page = client.get("/admin/").text
    assert 'id="landingToggle"' in page
    assert "/admin/landing-preview" in page
    assert "OSTRELAY_LANDING_PAGE_ENABLED" in page
    assert "/admin/" not in client.get("/").text
