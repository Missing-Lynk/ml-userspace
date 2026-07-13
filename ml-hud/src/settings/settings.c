/** @file settings.c @brief See settings.h. */
#include "settings.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct settings {
    char  *path;
    cJSON *root;   /* a JSON object holding the top-level settings */
};

/* Read a whole file into a freshly allocated NUL-terminated buffer, or NULL on any error. */
static char *read_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);
    if (size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);

    char *text = malloc((size_t) size + 1);
    if (text == NULL) {
        fclose(file);
        return NULL;
    }

    size_t read = fread(text, 1, (size_t) size, file);
    fclose(file);
    text[read] = '\0';

    return text;
}

/* Create the parent directory of @p path (one level; the grandparent must already exist). */
static void make_parent_dir(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL || slash == path) {
        return;
    }

    size_t len = (size_t) (slash - path);
    char dir[512];
    if (len >= sizeof(dir)) {
        return;
    }

    memcpy(dir, path, len);
    dir[len] = '\0';
    mkdir(dir, 0755);
}

settings_t *settings_open(const char *path)
{
    settings_t *settings = calloc(1, sizeof(*settings));
    if (settings == NULL) {
        return NULL;
    }

    settings->path = strdup(path);
    if (settings->path == NULL) {
        free(settings);
        return NULL;
    }

    char *text = read_file(path);
    if (text != NULL) {
        settings->root = cJSON_Parse(text);
        free(text);
    }

    if (!cJSON_IsObject(settings->root)) {
        cJSON_Delete(settings->root);
        settings->root = cJSON_CreateObject();
    }

    if (settings->root == NULL) {
        free(settings->path);
        free(settings);
        return NULL;
    }

    return settings;
}

int settings_save(settings_t *settings)
{
    char *text = cJSON_Print(settings->root);
    if (text == NULL) {
        return -1;
    }

    make_parent_dir(settings->path);

    char tmp[512];
    if ((size_t) snprintf(tmp, sizeof(tmp), "%s.tmp", settings->path) >= sizeof(tmp)) {
        free(text);
        return -1;
    }

    int rc = -1;
    FILE *file = fopen(tmp, "wb");
    if (file != NULL) {
        size_t len = strlen(text);
        if (fwrite(text, 1, len, file) == len && fputc('\n', file) != EOF) {
            rc = 0;
        }

        if (fclose(file) != 0) {
            rc = -1;
        }

        if (rc == 0 && rename(tmp, settings->path) != 0) {
            rc = -1;
        }

        if (rc != 0) {
            remove(tmp);
        }
    }

    free(text);
    return rc;
}

void settings_close(settings_t *settings)
{
    if (settings == NULL) {
        return;
    }

    cJSON_Delete(settings->root);
    free(settings->path);
    free(settings);
}

const char *settings_get_string(settings_t *settings, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(settings->root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }

    return fallback;
}

int settings_get_int(settings_t *settings, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(settings->root, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }

    return fallback;
}

int settings_get_bool(settings_t *settings, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(settings->root, key);
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }

    return fallback;
}

int settings_set_string(settings_t *settings, const char *key, const char *value)
{
    cJSON *item = cJSON_CreateString(value);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(settings->root, key);
    cJSON_AddItemToObject(settings->root, key, item);
    return settings_save(settings);
}

int settings_set_int(settings_t *settings, const char *key, int value)
{
    cJSON *item = cJSON_CreateNumber((double) value);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(settings->root, key);
    cJSON_AddItemToObject(settings->root, key, item);
    return settings_save(settings);
}

int settings_set_bool(settings_t *settings, const char *key, int value)
{
    cJSON *item = cJSON_CreateBool(value ? 1 : 0);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(settings->root, key);
    cJSON_AddItemToObject(settings->root, key, item);
    return settings_save(settings);
}

/* Return the nested object named @p section, reading only. NULL if it is absent or not an object. */
static cJSON *section_get(settings_t *settings, const char *section)
{
    cJSON *object = cJSON_GetObjectItemCaseSensitive(settings->root, section);
    return cJSON_IsObject(object) ? object : NULL;
}

/* Return the nested object named @p section, creating it if it is absent. NULL only on allocation
 * failure or if the name is taken by a non-object value.
 */
static cJSON *section_get_or_create(settings_t *settings, const char *section)
{
    cJSON *object = cJSON_GetObjectItemCaseSensitive(settings->root, section);
    if (cJSON_IsObject(object)) {
        return object;
    }

    if (object != NULL) {
        return NULL;   /* the name is held by a non-object value; refuse to clobber it */
    }

    object = cJSON_CreateObject();
    if (object == NULL) {
        return NULL;
    }

    cJSON_AddItemToObject(settings->root, section, object);
    return object;
}

const char *settings_get_string_in(settings_t *settings, const char *section, const char *key, const char *fallback)
{
    cJSON *object = section_get(settings, section);
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return item->valuestring;
    }

    return fallback;
}

int settings_get_int_in(settings_t *settings, const char *section, const char *key, int fallback)
{
    cJSON *object = section_get(settings, section);
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }

    return fallback;
}

int settings_get_bool_in(settings_t *settings, const char *section, const char *key, int fallback)
{
    cJSON *object = section_get(settings, section);
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
    if (cJSON_IsBool(item)) {
        return cJSON_IsTrue(item) ? 1 : 0;
    }

    return fallback;
}

int settings_set_string_in(settings_t *settings, const char *section, const char *key, const char *value)
{
    cJSON *object = section_get_or_create(settings, section);
    if (object == NULL) {
        return -1;
    }

    cJSON *item = cJSON_CreateString(value);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddItemToObject(object, key, item);
    return settings_save(settings);
}

int settings_set_int_in(settings_t *settings, const char *section, const char *key, int value)
{
    cJSON *object = section_get_or_create(settings, section);
    if (object == NULL) {
        return -1;
    }

    cJSON *item = cJSON_CreateNumber((double) value);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddItemToObject(object, key, item);
    return settings_save(settings);
}

int settings_set_bool_in(settings_t *settings, const char *section, const char *key, int value)
{
    cJSON *object = section_get_or_create(settings, section);
    if (object == NULL) {
        return -1;
    }

    cJSON *item = cJSON_CreateBool(value ? 1 : 0);
    if (item == NULL) {
        return -1;
    }

    cJSON_DeleteItemFromObjectCaseSensitive(object, key);
    cJSON_AddItemToObject(object, key, item);

    return settings_save(settings);
}
