"""
Deterministic UI tests for Internationalization (i18n) and multilingual support:
- English (default)
- Spanish (es)
- German (de)
- Russian (ru)
- Chinese (zh)
"""

import re
from playwright.sync_api import Page, expect


def test_i18n_default_language_is_english(server, page: Page):
    """Catalog defaults to English with all standard English UI elements."""
    page.goto(server["url"])

    expect(page.locator("#langSelect")).to_have_value("en")
    lang_attr = page.locator("html").get_attribute("lang")
    assert lang_attr == "en"

    # Top tabs
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Media")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("People")
    expect(page.locator('.tab-btn[data-tab="places"] span')).to_have_text("Places")
    expect(page.locator('.tab-btn[data-tab="events"] span')).to_have_text("Events")

    # Sidebar Navigation
    expect(page.locator('#navAllMedia [data-i18n="nav_all_media"]')).to_have_text("All Media")
    expect(page.locator('#navPicks [data-i18n="nav_picks"]')).to_have_text("Picks")
    expect(page.locator('#navRejects [data-i18n="nav_rejects"]')).to_have_text("Rejects")
    expect(page.locator('#navNotRejects [data-i18n="nav_not_rejects"]')).to_have_text("Not Rejects")
    expect(page.locator('#navUnrated [data-i18n="nav_unrated"]')).to_have_text("Unrated")

    # Toolbar
    expect(page.locator("#filterLabel")).to_contain_text("All Photos")
    expect(page.locator("#viewGridBtn span")).to_have_text("Grid")
    expect(page.locator("#viewMapBtn span")).to_have_text("Map")
    expect(page.locator(".sort-selector label")).to_have_text("Sort:")


def test_i18n_switch_to_spanish(server, page: Page):
    """Switching to Spanish updates navigation, sidebar, toolbar, inspector, and category views."""
    page.goto(server["url"])

    # Switch language to Spanish
    page.locator("#langSelect").select_option("es")

    expect(page.locator("#langSelect")).to_have_value("es")
    expect(page.locator("html")).to_have_attribute("lang", "es")

    # Verify localStorage
    stored_lang = page.evaluate("() => localStorage.getItem('imagine_language')")
    assert stored_lang == "es"

    # Top tabs
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Medios")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("Personas")
    expect(page.locator('.tab-btn[data-tab="places"] span')).to_have_text("Lugares")
    expect(page.locator('.tab-btn[data-tab="events"] span')).to_have_text("Eventos")

    # Sidebar Navigation
    expect(page.locator('#navAllMedia [data-i18n="nav_all_media"]')).to_have_text("Todos los medios")
    expect(page.locator('#navPicks [data-i18n="nav_picks"]')).to_have_text("Seleccionadas")
    expect(page.locator('#navRejects [data-i18n="nav_rejects"]')).to_have_text("Rechazadas")
    expect(page.locator('#navNotRejects [data-i18n="nav_not_rejects"]')).to_have_text("No rechazadas")
    expect(page.locator('#navUnrated [data-i18n="nav_unrated"]')).to_have_text("Sin calificar")

    # Sidebar headers
    expect(page.locator('.sidebar-section:has(#albumsList) .section-title')).to_have_text("Álbumes")
    expect(page.locator('.sidebar-section:has(#tagsTree) .section-title')).to_have_text("Etiquetas de palabras clave")
    expect(page.locator('.sidebar-section:has(#foldersTree) .section-title')).to_have_text("Carpetas")

    # Content Toolbar
    expect(page.locator("#filterLabel")).to_contain_text("Todas las fotos")
    expect(page.locator("#viewGridBtn span")).to_have_text("Cuadrícula")
    expect(page.locator("#viewMapBtn span")).to_have_text("Mapa")
    expect(page.locator(".sort-selector label")).to_have_text("Ordenar:")

    # Select a photo and verify inspector
    card = page.locator(".photo-card", has_text="sunset.bmp")
    card.click()
    expect(page.locator("#inspectorSelection")).to_be_visible()
    expect(page.locator("#fileInfoSection .section-heading")).to_have_text("Información del archivo")
    expect(page.locator("#inspectorDeleteBtn span")).to_have_text("Eliminar del catálogo")


def test_i18n_switch_to_german(server, page: Page):
    """Switching to German updates UI text and headers."""
    page.goto(server["url"])

    page.locator("#langSelect").select_option("de")

    expect(page.locator("#langSelect")).to_have_value("de")
    expect(page.locator("html")).to_have_attribute("lang", "de")

    # Top tabs
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Medien")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("Personen")
    expect(page.locator('.tab-btn[data-tab="places"] span')).to_have_text("Orte")
    expect(page.locator('.tab-btn[data-tab="events"] span')).to_have_text("Ereignisse")

    # Sidebar Navigation
    expect(page.locator('#navAllMedia [data-i18n="nav_all_media"]')).to_have_text("Alle Medien")
    expect(page.locator('#navPicks [data-i18n="nav_picks"]')).to_have_text("Ausgewählte")
    expect(page.locator('#navRejects [data-i18n="nav_rejects"]')).to_have_text("Abgelehnte")
    expect(page.locator('#navUnrated [data-i18n="nav_unrated"]')).to_have_text("Unbewertet")

    # Toolbar
    expect(page.locator("#filterLabel")).to_contain_text("Alle Fotos")
    expect(page.locator("#viewGridBtn span")).to_have_text("Raster")
    expect(page.locator("#viewMapBtn span")).to_have_text("Karte")
    expect(page.locator(".sort-selector label")).to_have_text("Sortieren:")


def test_i18n_switch_to_russian(server, page: Page):
    """Switching to Russian updates UI text to Cyrillic."""
    page.goto(server["url"])

    page.locator("#langSelect").select_option("ru")

    expect(page.locator("#langSelect")).to_have_value("ru")
    expect(page.locator("html")).to_have_attribute("lang", "ru")

    # Top tabs
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Медиа")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("Люди")
    expect(page.locator('.tab-btn[data-tab="places"] span')).to_have_text("Места")
    expect(page.locator('.tab-btn[data-tab="events"] span')).to_have_text("События")

    # Sidebar Navigation
    expect(page.locator('#navAllMedia [data-i18n="nav_all_media"]')).to_have_text("Все медиа")
    expect(page.locator('#navPicks [data-i18n="nav_picks"]')).to_have_text("Избранные")
    expect(page.locator('#navRejects [data-i18n="nav_rejects"]')).to_have_text("Отклонённые")
    expect(page.locator('#navNotRejects [data-i18n="nav_not_rejects"]')).to_have_text("Не отклонённые")
    expect(page.locator('#navUnrated [data-i18n="nav_unrated"]')).to_have_text("Без оценки")

    # Toolbar
    expect(page.locator("#filterLabel")).to_contain_text("Все фото")
    expect(page.locator("#viewGridBtn span")).to_have_text("Сетка")
    expect(page.locator("#viewMapBtn span")).to_have_text("Карта")
    expect(page.locator(".sort-selector label")).to_have_text("Сортировка:")


def test_i18n_switch_to_chinese(server, page: Page):
    """Switching to Chinese updates UI text to Simplified Chinese."""
    page.goto(server["url"])

    page.locator("#langSelect").select_option("zh")

    expect(page.locator("#langSelect")).to_have_value("zh")
    expect(page.locator("html")).to_have_attribute("lang", "zh")

    # Top tabs
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("媒体")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("人物")
    expect(page.locator('.tab-btn[data-tab="places"] span')).to_have_text("地点")
    expect(page.locator('.tab-btn[data-tab="events"] span')).to_have_text("事件")

    # Sidebar Navigation
    expect(page.locator('#navAllMedia [data-i18n="nav_all_media"]')).to_have_text("所有媒体")
    expect(page.locator('#navPicks [data-i18n="nav_picks"]')).to_have_text("精选")
    expect(page.locator('#navRejects [data-i18n="nav_rejects"]')).to_have_text("已排除")
    expect(page.locator('#navNotRejects [data-i18n="nav_not_rejects"]')).to_have_text("未排除")
    expect(page.locator('#navUnrated [data-i18n="nav_unrated"]')).to_have_text("未评分")

    # Toolbar
    expect(page.locator("#filterLabel")).to_contain_text("所有照片")
    expect(page.locator("#viewGridBtn span")).to_have_text("网格")
    expect(page.locator("#viewMapBtn span")).to_have_text("地图")
    expect(page.locator(".sort-selector label")).to_have_text("排序：")


def test_i18n_language_persistence_across_page_reload(server, page: Page):
    """Selected language persists in localStorage across browser page reloads."""
    page.goto(server["url"])

    # Change to Chinese
    page.locator("#langSelect").select_option("zh")
    expect(page.locator("#langSelect")).to_have_value("zh")

    # Reload page
    page.reload()

    # Verify language is still Chinese after reload
    expect(page.locator("#langSelect")).to_have_value("zh")
    expect(page.locator("html")).to_have_attribute("lang", "zh")
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("媒体")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("人物")

    # Change to German
    page.locator("#langSelect").select_option("de")
    expect(page.locator("#langSelect")).to_have_value("de")

    # Reload page
    page.reload()

    expect(page.locator("#langSelect")).to_have_value("de")
    expect(page.locator('.tab-btn[data-tab="media"] span')).to_have_text("Medien")
    expect(page.locator('.tab-btn[data-tab="people"] span')).to_have_text("Personen")


def test_i18n_batch_action_bar_translations(server, page: Page):
    """Batch action bar buttons and counts translate when selecting items."""
    page.goto(server["url"])

    # Select two photos
    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="mountain.bmp")
    card1.click()
    card2.click(modifiers=["Control"])

    expect(page.locator("#batchActionBar")).to_be_visible()
    expect(page.locator("#batchSelectedCount")).to_have_text("2 selected")

    # Switch to Spanish
    page.locator("#langSelect").select_option("es")
    expect(page.locator("#batchSelectedCount")).to_have_text("2 seleccionados")
    expect(page.locator("#batchPickBtn span")).to_have_text("Seleccionar")
    expect(page.locator("#batchRejectBtn span")).to_have_text("Rechazar")
    expect(page.locator("#batchDeleteBtn span")).to_have_text("Eliminar")

    # Switch to German
    page.locator("#langSelect").select_option("de")
    expect(page.locator("#batchSelectedCount")).to_have_text("2 ausgewählt")
    expect(page.locator("#batchPickBtn span")).to_have_text("Auswählen")
    expect(page.locator("#batchRejectBtn span")).to_have_text("Ablehnen")
    expect(page.locator("#batchDeleteBtn span")).to_have_text("Löschen")

    # Switch to Russian
    page.locator("#langSelect").select_option("ru")
    expect(page.locator("#batchSelectedCount")).to_have_text("Выбрано: 2")
    expect(page.locator("#batchPickBtn span")).to_have_text("Выбрать")
    expect(page.locator("#batchRejectBtn span")).to_have_text("Отклонить")
    expect(page.locator("#batchDeleteBtn span")).to_have_text("Удалить")

    # Switch to Chinese
    page.locator("#langSelect").select_option("zh")
    expect(page.locator("#batchSelectedCount")).to_have_text("已选择 2 项")
    expect(page.locator("#batchPickBtn span")).to_have_text("挑选")
    expect(page.locator("#batchRejectBtn span")).to_have_text("排除")
    expect(page.locator("#batchDeleteBtn span")).to_have_text("删除")


def test_i18n_modal_dialogs_translations(server, page: Page):
    """Modals for New Album, New Tag, and Import show translated titles and buttons."""
    page.goto(server["url"])

    # Switch to Spanish
    page.locator("#langSelect").select_option("es")

    # Open New Album Modal
    page.locator("#newAlbumBtn").click()
    modal = page.locator("#newAlbumModal")
    expect(modal).to_be_visible()
    expect(modal.locator("h3")).to_have_text("Crear nuevo álbum")
    expect(modal.locator("#createAlbumSubmitBtn")).to_have_text("Crear álbum")
    page.locator("#cancelAlbumBtn").click()
    expect(modal).to_be_hidden()

    # Open New Tag Modal
    page.locator("#newTagBtn").click()
    tag_modal = page.locator("#newTagModal")
    expect(tag_modal).to_be_visible()
    expect(tag_modal.locator("#tagModalHeading")).to_have_text("Crear etiqueta")
    expect(tag_modal.locator("#createTagSubmitBtn")).to_have_text("Crear etiqueta")
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()

    # Open Import Modal
    page.locator("#importBtn").click()
    import_modal = page.locator("#importModal")
    expect(import_modal).to_be_visible()
    expect(import_modal.locator("h3")).to_have_text("Importar fotos al catálogo")
    expect(import_modal.locator("#startImportBtn")).to_have_text("Iniciar importación")
    page.locator("#cancelImportBtn").click()
    expect(import_modal).to_be_hidden()


def test_i18n_batch_album_date_move_translations(server, page: Page):
    """Verify translated strings for Add to Album, Date adjust & preview, and Move modals across all languages."""
    page.goto(server["url"])

    # Select two photos
    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="mountain.bmp")
    card1.click()
    card2.click(modifiers=["Control"])

    expect(page.locator("#batchActionBar")).to_be_visible()

    # 1. English (default)
    expect(page.locator("#batchAddAlbumBtn")).to_have_text("Add to Album")
    page.locator("#batchAddAlbumBtn").click()
    expect(page.locator("#addToAlbumTargetCount")).to_have_text("Add 2 selected photos to:")
    page.locator("#cancelAddToAlbumBtn").click()

    page.locator("#batchDateBtn").click()
    expect(page.locator("#batchDateTargetCount")).to_have_text("Adjust date & time for 2 selected photos.")
    expect(page.locator("#batchDatePreview")).to_contain_text("Preview: No time shift (dates unchanged). Affects 2 photos.")
    page.locator("#cancelBatchDateBtn").click()

    page.locator("#batchMoveBtn").click()
    expect(page.locator("#batchMoveTargetCount")).to_have_text("Move 2 selected photos to a folder inside the photos directory:")
    expect(page.locator("#batchMoveModalTitle")).to_have_text("Move Photos")
    page.locator("#cancelBatchMoveBtn").click()

    # 2. Spanish
    page.locator("#langSelect").select_option("es")
    expect(page.locator("#batchAddAlbumBtn")).to_have_text("Agregar al álbum")
    page.locator("#batchAddAlbumBtn").click()
    expect(page.locator("#addToAlbumTargetCount")).to_have_text("Agregar 2 fotos seleccionadas a:")
    page.locator("#cancelAddToAlbumBtn").click()

    page.locator("#batchDateBtn").click()
    expect(page.locator("#batchDateTargetCount")).to_have_text("Ajustar fecha y hora para 2 fotos seleccionadas.")
    expect(page.locator("#batchDatePreview")).to_contain_text("Vista previa: Sin cambio de hora (fechas sin cambios). Afecta a 2 fotos.")
    page.locator("#cancelBatchDateBtn").click()

    page.locator("#batchMoveBtn").click()
    expect(page.locator("#batchMoveTargetCount")).to_have_text("Mover 2 fotos seleccionadas a una carpeta dentro del directorio de fotos:")
    expect(page.locator("#batchMoveModalTitle")).to_have_text("Mover fotos")
    page.locator("#cancelBatchMoveBtn").click()

    # 3. German
    page.locator("#langSelect").select_option("de")
    expect(page.locator("#batchAddAlbumBtn")).to_have_text("Zu Album hinzufügen")
    page.locator("#batchAddAlbumBtn").click()
    expect(page.locator("#addToAlbumTargetCount")).to_have_text("2 ausgewählte Fotos hinzufügen zu:")
    page.locator("#cancelAddToAlbumBtn").click()

    page.locator("#batchDateBtn").click()
    expect(page.locator("#batchDateTargetCount")).to_have_text("Datum & Uhrzeit für 2 ausgewählte Fotos anpassen.")
    expect(page.locator("#batchDatePreview")).to_contain_text("Vorschau: Keine Zeitverschiebung (Daten unverändert). Betrifft 2 Fotos.")
    page.locator("#cancelBatchDateBtn").click()

    page.locator("#batchMoveBtn").click()
    expect(page.locator("#batchMoveTargetCount")).to_have_text("2 ausgewählte Fotos in einen Ordner im Fotoverzeichnis verschieben:")
    expect(page.locator("#batchMoveModalTitle")).to_have_text("Fotos verschieben")
    page.locator("#cancelBatchMoveBtn").click()

    # 4. Russian
    page.locator("#langSelect").select_option("ru")
    expect(page.locator("#batchAddAlbumBtn")).to_have_text("Добавить в альбом")
    page.locator("#batchAddAlbumBtn").click()
    expect(page.locator("#addToAlbumTargetCount")).to_have_text("Добавить 2 выбранных фото в:")
    page.locator("#cancelAddToAlbumBtn").click()

    page.locator("#batchDateBtn").click()
    expect(page.locator("#batchDateTargetCount")).to_have_text("Настроить дату и время для 2 выбранных фото.")
    expect(page.locator("#batchDatePreview")).to_contain_text("Предпросмотр: Без смещения времени (даты без изменений). Затрагивает 2 фото.")
    page.locator("#cancelBatchDateBtn").click()

    page.locator("#batchMoveBtn").click()
    expect(page.locator("#batchMoveTargetCount")).to_have_text("Переместить 2 выбранных фото в папку внутри каталога фотографий:")
    expect(page.locator("#batchMoveModalTitle")).to_have_text("Переместить фото")
    page.locator("#cancelBatchMoveBtn").click()

    # 5. Chinese
    page.locator("#langSelect").select_option("zh")
    expect(page.locator("#batchAddAlbumBtn")).to_have_text("添加到相册")
    page.locator("#batchAddAlbumBtn").click()
    expect(page.locator("#addToAlbumTargetCount")).to_have_text("将 2 张所选照片添加到：")
    page.locator("#cancelAddToAlbumBtn").click()

    page.locator("#batchDateBtn").click()
    expect(page.locator("#batchDateTargetCount")).to_have_text("调整 2 张所选照片的日期和时间。")
    expect(page.locator("#batchDatePreview")).to_contain_text("预览: 无时间偏移（日期保持不变）。影响 2 张照片。")
    page.locator("#cancelBatchDateBtn").click()

    page.locator("#batchMoveBtn").click()
    expect(page.locator("#batchMoveTargetCount")).to_have_text("将 2 张所选照片移动到照片目录内的文件夹：")
    expect(page.locator("#batchMoveModalTitle")).to_have_text("移动照片")
    page.locator("#cancelBatchMoveBtn").click()


def test_i18n_tag_modal_single_line_for_multiple_photos(server, page: Page):
    """Verify tag modal shows only one clean, translated line when multiple photos are selected."""
    page.goto(server["url"])

    # Select two photos
    card1 = page.locator(".photo-card", has_text="sunset.bmp")
    card2 = page.locator(".photo-card", has_text="mountain.bmp")
    card1.click()
    card2.click(modifiers=["Control"])

    # Open tag modal in English
    page.locator("#batchAddTagBtn").click()
    tag_modal = page.locator("#newTagModal")
    expect(tag_modal).to_be_visible()

    expect(page.locator("#tagModalPhotoName")).to_have_text("2 photos selected")
    expect(page.locator("#tagModalPhotoMeta")).to_be_hidden()
    expect(page.locator("#tagModalPhotoTagsRow")).to_be_hidden()
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()

    # Switch to German
    page.locator("#langSelect").select_option("de")
    page.locator("#batchAddTagBtn").click()
    expect(tag_modal).to_be_visible()
    expect(page.locator("#tagModalHeading")).to_have_text("Schlagwort zu 2 Fotos hinzufügen")
    expect(page.locator("#tagModalPhotoName")).to_have_text("2 Fotos ausgewählt")
    expect(page.locator("#tagModalPhotoMeta")).to_be_hidden()
    expect(page.locator("#tagModalPhotoTagsRow")).to_be_hidden()
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()

    # Switch to Spanish
    page.locator("#langSelect").select_option("es")
    page.locator("#batchAddTagBtn").click()
    expect(tag_modal).to_be_visible()
    expect(page.locator("#tagModalHeading")).to_have_text("Agregar etiqueta a 2 fotos")
    expect(page.locator("#tagModalPhotoName")).to_have_text("2 fotos seleccionadas")
    expect(page.locator("#tagModalPhotoMeta")).to_be_hidden()
    expect(page.locator("#tagModalPhotoTagsRow")).to_be_hidden()
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()

    # Switch to Russian
    page.locator("#langSelect").select_option("ru")
    page.locator("#batchAddTagBtn").click()
    expect(tag_modal).to_be_visible()
    expect(page.locator("#tagModalHeading")).to_have_text("Добавить тег к 2 фото")
    expect(page.locator("#tagModalPhotoName")).to_have_text("Выбрано 2 фото")
    expect(page.locator("#tagModalPhotoMeta")).to_be_hidden()
    expect(page.locator("#tagModalPhotoTagsRow")).to_be_hidden()
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()

    # Switch to Chinese
    page.locator("#langSelect").select_option("zh")
    page.locator("#batchAddTagBtn").click()
    expect(tag_modal).to_be_visible()
    expect(page.locator("#tagModalHeading")).to_have_text("向 2 张照片添加标签")
    expect(page.locator("#tagModalPhotoName")).to_have_text("已选择 2 张照片")
    expect(page.locator("#tagModalPhotoMeta")).to_be_hidden()
    expect(page.locator("#tagModalPhotoTagsRow")).to_be_hidden()
    page.locator("#cancelTagBtn").click()
    expect(tag_modal).to_be_hidden()
