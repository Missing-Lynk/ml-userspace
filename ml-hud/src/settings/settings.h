/**
 * @file settings.h
 * @brief The HUD settings object: a JSON file of general settings, loaded once and persisted when a
 *        value changes. Shared by the HUD and the menu.
 *
 * Top-level keys are the general settings that apply across the whole UI (e.g. "language"). Getters
 * return a fallback when a key is missing or the wrong type; setters update memory and write the file.
 */
#ifndef HUD_SETTINGS_H
#define HUD_SETTINGS_H

typedef struct settings settings_t;

/**
 * @brief Open the settings file at @p path and load its JSON. A missing or unparseable file yields an
 *        empty settings object (the file is created on the first set/save).
 * @return the object, or NULL on allocation failure.
 */
settings_t *settings_open(const char *path);

/** @brief Write the settings back to the file (atomic replace). @return 0 on success, -1 on error. */
int settings_save(settings_t *settings);

/** @brief Free the object. Does not save; use settings_save first if there are unsaved changes. */
void settings_close(settings_t *settings);

/* Top-level getters. Return @p fallback if the key is absent or holds a different type. A returned
 * string points into the object and stays valid until that key is set again or the object is closed.
 */
const char *settings_get_string(settings_t *settings, const char *key, const char *fallback);
int settings_get_int(settings_t *settings, const char *key, int fallback);
int settings_get_bool(settings_t *settings, const char *key, int fallback);

/* Top-level setters. Update the value in memory and persist immediately. @return 0 on success. */
int settings_set_string(settings_t *settings, const char *key, const char *value);
int settings_set_int(settings_t *settings, const char *key, int value);
int settings_set_bool(settings_t *settings, const char *key, int value);

/* Section getters/setters. The same as above, but the key lives in a nested object named @p section
 * (e.g. the "goggle" section holds every Goggles-menu setting). Setters create the section on demand.
 */
const char *settings_get_string_in(settings_t *settings, const char *section, const char *key, const char *fallback);
int settings_get_int_in(settings_t *settings, const char *section, const char *key, int fallback);
int settings_get_bool_in(settings_t *settings, const char *section, const char *key, int fallback);
int settings_set_string_in(settings_t *settings, const char *section, const char *key, const char *value);
int settings_set_int_in(settings_t *settings, const char *section, const char *key, int value);
int settings_set_bool_in(settings_t *settings, const char *section, const char *key, int value);

#endif /* HUD_SETTINGS_H */
