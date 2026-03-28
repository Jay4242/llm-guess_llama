#include "guess_llama.h"

static void free_string_array(char** items, int count) {
    if (!items) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        free(items[i]);
    }
    free(items);
}

static void free_character_traits_array(char*** traits, int characterCount) {
    if (!traits) {
        return;
    }

    for (int i = 0; i < characterCount; ++i) {
        if (!traits[i]) {
            continue;
        }
        for (int j = 0; j < 2; ++j) {
            free(traits[i][j]);
        }
        free(traits[i]);
    }
    free(traits);
}

char* format_theme_name(const char* theme) {
    size_t len;
    char* formatted;
    size_t out_index = 0;

    if (!theme) {
        return strdup("");
    }

    len = strlen(theme);
    formatted = malloc(len + 1);
    if (!formatted) {
        fprintf(stderr, "Failed to allocate memory for formatted theme name.\n");
        return strdup("");
    }

    for (size_t i = 0; i < len; ++i) {
        char c = (char)tolower((unsigned char)theme[i]);
        if (isalnum((unsigned char)c)) {
            formatted[out_index++] = c;
        } else if (c == ' ') {
            formatted[out_index++] = '_';
        }
    }

    formatted[out_index] = '\0';
    return formatted;
}

bool directory_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int create_directory_recursive(const char* path) {
    char* tmp = strdup(path);
    size_t len;
    int ret = 0;

    if (!tmp) {
        fprintf(stderr, "Failed to allocate memory for path.\n");
        return -1;
    }

    len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (char* p = tmp + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir(tmp, S_IRWXU) != 0 && errno != EEXIST) {
            fprintf(stderr, "Failed to create directory %s: %s\n", tmp, strerror(errno));
            ret = -1;
            *p = '/';
            break;
        }
        *p = '/';
    }

    if (ret == 0 && mkdir(tmp, S_IRWXU) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", tmp, strerror(errno));
        ret = -1;
    }

    free(tmp);
    return ret;
}

int delete_files_in_directory(const char* path) {
    DIR* directory = opendir(path);
    struct dirent* entry;
    char filepath[MAX_FILEPATH_BUFFER_SIZE];
    int ret = 0;

    if (!directory) {
        fprintf(stderr, "Failed to open directory %s: %s\n", path, strerror(errno));
        return -1;
    }

    while ((entry = readdir(directory)) != NULL) {
        struct stat st;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", path, entry->d_name);
        if (stat(filepath, &st) != 0) {
            fprintf(stderr, "Failed to stat file %s: %s\n", filepath, strerror(errno));
            ret = -1;
            continue;
        }

        if (S_ISREG(st.st_mode) && unlink(filepath) != 0) {
            fprintf(stderr, "Failed to delete file %s: %s\n", filepath, strerror(errno));
            ret = -1;
        }
    }

    closedir(directory);
    return ret;
}

bool save_game_data(
    const char* filename,
    const char* theme,
    char** features,
    int featureCountLocal,
    char*** characterTraitsLocal,
    int numCharacters
) {
    json_t* root = json_object();
    json_t* features_array = json_array();
    json_t* characters_array = json_array();
    int result;

    if (!root || !features_array || !characters_array) {
        fprintf(stderr, "Failed to create JSON containers.\n");
        json_decref(root);
        json_decref(features_array);
        json_decref(characters_array);
        return false;
    }

    json_object_set_new(root, "theme", json_string(theme));

    for (int i = 0; i < featureCountLocal; ++i) {
        json_array_append_new(features_array, json_string(features[i]));
    }
    json_object_set_new(root, "features", features_array);

    for (int i = 0; i < numCharacters; ++i) {
        json_t* character_obj = json_object();
        json_t* traits_array = json_array();
        char* prompt = NULL;

        if (!character_obj || !traits_array) {
            fprintf(stderr, "Failed to create JSON character containers.\n");
            json_decref(character_obj);
            json_decref(traits_array);
            json_decref(characters_array);
            json_decref(root);
            return false;
        }

        json_object_set_new(character_obj, "id", json_integer(i + 1));
        for (int j = 0; j < 2; ++j) {
            json_array_append_new(traits_array, json_string(characterTraitsLocal[i][j]));
        }
        json_object_set_new(character_obj, "traits", traits_array);

        if (asprintf(&prompt, "A %s, %s, %s", theme, characterTraitsLocal[i][0], characterTraitsLocal[i][1]) == -1) {
            fprintf(stderr, "Failed to reconstruct prompt for character %d for saving.\n", i + 1);
            json_decref(character_obj);
            json_decref(characters_array);
            json_decref(root);
            return false;
        }

        json_object_set_new(character_obj, "prompt", json_string(prompt));
        free(prompt);

        json_array_append_new(characters_array, character_obj);
    }

    json_object_set_new(root, "characters", characters_array);
    result = json_dump_file(root, filename, JSON_INDENT(4) | JSON_PRESERVE_ORDER);
    json_decref(root);

    if (result != 0) {
        fprintf(stderr, "Failed to save game data to %s.\n", filename);
        return false;
    }

    printf("Game data saved to %s\n", filename);
    return true;
}

bool load_game_data(
    const char* filename,
    char** loadedTheme,
    char*** loadedFeatures,
    int* loadedFeatureCount,
    char**** loadedCharacterTraits
) {
    json_error_t error;
    json_t* root = json_load_file(filename, 0, &error);
    json_t* theme_json;
    json_t* features_array;
    json_t* characters_array;
    char* theme = NULL;
    char** features = NULL;
    char*** traits = NULL;

    if (!root) {
        fprintf(
            stderr,
            "Error loading game data from %s: %s (line %d, col %d)\n",
            filename,
            error.text,
            error.line,
            error.column
        );
        return false;
    }

    theme_json = json_object_get(root, "theme");
    if (!json_is_string(theme_json)) {
        fprintf(stderr, "Invalid or missing 'theme' in game data.\n");
        json_decref(root);
        return false;
    }

    theme = strdup(json_string_value(theme_json));
    if (!theme) {
        fprintf(stderr, "Failed to allocate memory for theme.\n");
        json_decref(root);
        return false;
    }

    features_array = json_object_get(root, "features");
    if (!json_is_array(features_array)) {
        fprintf(stderr, "Invalid or missing 'features' array in game data.\n");
        free(theme);
        json_decref(root);
        return false;
    }

    *loadedFeatureCount = (int)json_array_size(features_array);
    features = calloc((size_t)*loadedFeatureCount, sizeof(char*));
    if (!features) {
        fprintf(stderr, "Failed to allocate memory for features.\n");
        free(theme);
        json_decref(root);
        return false;
    }

    for (int i = 0; i < *loadedFeatureCount; ++i) {
        json_t* feature_json = json_array_get(features_array, (size_t)i);
        if (!json_is_string(feature_json)) {
            fprintf(stderr, "Invalid feature entry in game data.\n");
            free_string_array(features, *loadedFeatureCount);
            free(theme);
            json_decref(root);
            return false;
        }

        features[i] = strdup(json_string_value(feature_json));
        if (!features[i]) {
            fprintf(stderr, "Failed to allocate memory for feature %d.\n", i + 1);
            free_string_array(features, *loadedFeatureCount);
            free(theme);
            json_decref(root);
            return false;
        }
    }

    characters_array = json_object_get(root, "characters");
    if (!json_is_array(characters_array)) {
        fprintf(stderr, "Invalid or missing 'characters' array in game data.\n");
        free_string_array(features, *loadedFeatureCount);
        free(theme);
        json_decref(root);
        return false;
    }

    if ((int)json_array_size(characters_array) != NUM_CHARACTERS) {
        fprintf(
            stderr,
            "Mismatch in character count. Expected %d, got %zu. Aborting load.\n",
            NUM_CHARACTERS,
            json_array_size(characters_array)
        );
        free_string_array(features, *loadedFeatureCount);
        free(theme);
        json_decref(root);
        return false;
    }

    traits = calloc(NUM_CHARACTERS, sizeof(char**));
    if (!traits) {
        fprintf(stderr, "Failed to allocate memory for character traits.\n");
        free_string_array(features, *loadedFeatureCount);
        free(theme);
        json_decref(root);
        return false;
    }

    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        json_t* character_obj = json_array_get(characters_array, (size_t)i);
        json_t* traits_array_json;

        if (!json_is_object(character_obj)) {
            fprintf(stderr, "Invalid character object in game data.\n");
            free_character_traits_array(traits, NUM_CHARACTERS);
            free_string_array(features, *loadedFeatureCount);
            free(theme);
            json_decref(root);
            return false;
        }

        traits_array_json = json_object_get(character_obj, "traits");
        if (!json_is_array(traits_array_json) || json_array_size(traits_array_json) != 2) {
            fprintf(stderr, "Invalid or missing 'traits' array for character %d.\n", i + 1);
            free_character_traits_array(traits, NUM_CHARACTERS);
            free_string_array(features, *loadedFeatureCount);
            free(theme);
            json_decref(root);
            return false;
        }

        traits[i] = calloc(2, sizeof(char*));
        if (!traits[i]) {
            fprintf(stderr, "Failed to allocate memory for character %d traits.\n", i + 1);
            free_character_traits_array(traits, NUM_CHARACTERS);
            free_string_array(features, *loadedFeatureCount);
            free(theme);
            json_decref(root);
            return false;
        }

        for (int j = 0; j < 2; ++j) {
            json_t* trait_json = json_array_get(traits_array_json, (size_t)j);
            if (!json_is_string(trait_json)) {
                fprintf(stderr, "Invalid trait entry for character %d, trait %d.\n", i + 1, j + 1);
                free_character_traits_array(traits, NUM_CHARACTERS);
                free_string_array(features, *loadedFeatureCount);
                free(theme);
                json_decref(root);
                return false;
            }

            traits[i][j] = strdup(json_string_value(trait_json));
            if (!traits[i][j]) {
                fprintf(stderr, "Failed to allocate memory for trait %d for character %d.\n", j + 1, i + 1);
                free_character_traits_array(traits, NUM_CHARACTERS);
                free_string_array(features, *loadedFeatureCount);
                free(theme);
                json_decref(root);
                return false;
            }
        }
    }

    json_decref(root);

    *loadedTheme = theme;
    *loadedFeatures = features;
    *loadedCharacterTraits = traits;

    printf("Game data loaded from %s\n", filename);
    return true;
}
