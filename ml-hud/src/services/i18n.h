/**
 * @file i18n.h
 * @brief Hot-pluggable translations for the menu.
 *
 * Strings are referenced by key (e.g. "goggles.brightness") and resolved at runtime from a
 * plain-text catalog file per language, <dir>/<code>.lang, holding "key = value" lines. The files
 * are editable without recompiling, and a language switches by loading a different catalog. Missing
 * keys fall back to the key itself, so the UI never shows blank.
 */
#ifndef HUD_I18N_H
#define HUD_I18N_H

/**
 * @brief Load the catalog for a language code, reading <catalog_dir>/<language_code>.lang.
 *        Replaces any previously loaded catalog.
 * @return 0 on success, -1 if the file is missing.
 */
int i18n_load_language(const char *catalog_dir, const char *language_code);

/** @brief Free the loaded catalog. */
void i18n_unload(void);

/**
 * @brief Resolve a key to its translated text, or @p key itself if not found (so callers always
 *        render something). Valid until the next load/unload.
 */
const char *i18n_translate(const char *key);

/** @brief Short alias for i18n_translate(), used throughout the UI code. */
#define T(key) i18n_translate(key)

#endif /* HUD_I18N_H */
