/**
 * @file i18n.c
 * @brief A small "key = value" catalog loader; see i18n.h.
 *
 * Format of <code>.lang (UTF-8):
 *   # comment lines start with '#'
 *   goggles.brightness = Brightness
 * Leading/trailing whitespace around the key and value is trimmed.
 */
#include "i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I18N_PATH_BUF_LEN 512

typedef struct {
    char *key;
    char *value;
} translation_entry_t;

static translation_entry_t *g_entries;
static size_t g_entry_count;

static char *duplicate_string(const char *text)
{
    size_t length = strlen(text);
    char *copy = (char *) malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }

    return copy;
}

/** @brief Trim surrounding whitespace in place. @return the trimmed start. */
static char *trim_in_place(char *text)
{
    while (*text == ' ' || *text == '\t') {
        text++;
    }

    size_t length = strlen(text);
    while (length > 0) {
        char last = text[length - 1];
        if (last == ' ' || last == '\t' || last == '\r' || last == '\n') {
            text[length - 1] = '\0';
            length--;
        } else {
            break;
        }
    }

    return text;
}

void i18n_unload(void)
{
    for (size_t i = 0; i < g_entry_count; i++) {
        free(g_entries[i].key);
        free(g_entries[i].value);
    }

    free(g_entries);
    g_entries = NULL;
    g_entry_count = 0;
}

/** @brief Append a copied key/value pair to the catalog; a no-op on allocation failure. */
static void add_entry(const char *key, const char *value)
{
    char *key_copy = duplicate_string(key);
    char *value_copy = duplicate_string(value);
    if (key_copy == NULL || value_copy == NULL) {
        free(key_copy);
        free(value_copy);
        return;
    }

    translation_entry_t *grown =
        (translation_entry_t *) realloc(g_entries, (g_entry_count + 1) * sizeof(*g_entries));
    if (grown == NULL) {
        free(key_copy);
        free(value_copy);
        return;
    }

    g_entries = grown;
    g_entries[g_entry_count].key = key_copy;
    g_entries[g_entry_count].value = value_copy;
    g_entry_count++;
}

int i18n_load_language(const char *catalog_dir, const char *language_code)
{
    char path[I18N_PATH_BUF_LEN];
    snprintf(path, sizeof(path), "%s/%s.lang", catalog_dir, language_code);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    i18n_unload();

    char line[1024];
    while (fgets(line, sizeof(line), file) != NULL) {
        char *trimmed = trim_in_place(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') {
            continue;
        }

        char *separator = strchr(trimmed, '=');
        if (separator == NULL) {
            continue;
        }

        *separator = '\0';
        char *key = trim_in_place(trimmed);
        char *value = trim_in_place(separator + 1);
        if (key[0] != '\0') {
            add_entry(key, value);
        }
    }

    fclose(file);
    return 0;
}

const char *i18n_translate(const char *key)
{
    for (size_t i = 0; i < g_entry_count; i++) {
        if (strcmp(g_entries[i].key, key) == 0) {
            return g_entries[i].value;
        }
    }

    return key;
}
