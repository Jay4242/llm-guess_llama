#include "guess_llama.h"

static void setGenerationStatus(const char* message) {
    pthread_mutex_lock(&mutex);
    snprintf(generation_status_message, sizeof(generation_status_message), "%s", message);
    pthread_mutex_unlock(&mutex);
}

static void markSetupFailed(void) {
    pthread_mutex_lock(&mutex);
    currentGameState = GAME_STATE_EXIT;
    setup_in_progress = false;
    pthread_mutex_unlock(&mutex);
}

static void initializeRemainingCharacters(void) {
    charactersRemaining = malloc(NUM_CHARACTERS * sizeof(int));
    if (!charactersRemaining) {
        fprintf(stderr, "Memory allocation failed\n");
        return;
    }

    remainingCount = NUM_CHARACTERS;
    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        charactersRemaining[i] = i;
    }
}

static void initializePlayerRemainingCharacters(void) {
    playerRemainingCount = NUM_CHARACTERS;
    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        playerCharacterActive[i] = true;
    }

    if (playerCharacter >= 0 && playerCharacter < NUM_CHARACTERS) {
        playerCharacterActive[playerCharacter] = false;
        playerRemainingCount -= 1;
    }
}

static bool assignPlayerAndOpponent(void) {
    playerCharacter = rand() % NUM_CHARACTERS;
    printf("\nYou are character number %d\n", playerCharacter + 1);

    pthread_mutex_lock(&mutex);
    snprintf(
        playerCharacterString,
        sizeof(playerCharacterString),
        "Character %d: %s, %s",
        playerCharacter + 1,
        characterTraits[playerCharacter][0],
        characterTraits[playerCharacter][1]
    );
    snprintf(
        characterSelectionText,
        sizeof(characterSelectionText),
        "You are character number %d",
        playerCharacter + 1
    );
    pthread_mutex_unlock(&mutex);

    do {
        llmCharacter = rand() % NUM_CHARACTERS;
    } while (llmCharacter == playerCharacter);

    initializeRemainingCharacters();
    if (!charactersRemaining) {
        markSetupFailed();
        return false;
    }

    initializePlayerRemainingCharacters();

    return true;
}

static bool buildCharacterTraits(void) {
    typedef struct {
        int first;
        int second;
    } TraitPair;

    size_t pairCount = 0;
    size_t pairIndex = 0;
    TraitPair* pairs = NULL;

    if (featureCount < 2) {
        fprintf(stderr, "Need at least 2 features to assign character traits.\n");
        return false;
    }

    pairCount = ((size_t)featureCount * (size_t)(featureCount - 1)) / 2;
    if (pairCount < (size_t)NUM_CHARACTERS) {
        fprintf(
            stderr,
            "Not enough unique feature pairs: have %zu, need %d.\n",
            pairCount,
            NUM_CHARACTERS
        );
        return false;
    }

    pairs = malloc(pairCount * sizeof(TraitPair));
    if (!pairs) {
        fprintf(stderr, "Memory allocation failed\n");
        return false;
    }

    for (int i = 0; i < featureCount; ++i) {
        for (int j = i + 1; j < featureCount; ++j) {
            pairs[pairIndex].first = i;
            pairs[pairIndex].second = j;
            ++pairIndex;
        }
    }

    for (size_t i = pairCount - 1; i > 0; --i) {
        size_t j = (size_t)(rand() % (int)(i + 1));
        TraitPair temp = pairs[i];
        pairs[i] = pairs[j];
        pairs[j] = temp;
    }

    characterTraits = calloc(NUM_CHARACTERS, sizeof(char**));
    if (!characterTraits) {
        fprintf(stderr, "Memory allocation failed\n");
        free(pairs);
        return false;
    }

    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        characterTraits[i] = calloc(2, sizeof(char*));
        if (!characterTraits[i]) {
            fprintf(stderr, "Memory allocation failed\n");
            free(pairs);
            return false;
        }

        characterTraits[i][0] = strdup(characterFeatures[pairs[i].first]);
        characterTraits[i][1] = strdup(characterFeatures[pairs[i].second]);
        if (!characterTraits[i][0] || !characterTraits[i][1]) {
            fprintf(stderr, "Memory allocation failed\n");
            free(pairs);
            return false;
        }
    }

    free(pairs);

    printf("\nCharacter Traits:\n");
    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        printf("Character %d: %s, %s\n", i + 1, characterTraits[i][0], characterTraits[i][1]);
    }

    return true;
}

static bool startImageGeneration(void) {
    BatchImageGenData* batch_data = malloc(sizeof(BatchImageGenData));

    if (!batch_data) {
        fprintf(stderr, "Failed to allocate memory for batch image generation data.\n");
        return false;
    }

    batch_data->num_images = NUM_CHARACTERS;
    batch_data->image_dir = imageDirectoryPath;
    batch_data->images_data = calloc(NUM_CHARACTERS, sizeof(SingleImageGenData));
    if (!batch_data->images_data) {
        fprintf(stderr, "Failed to allocate memory for images_data array.\n");
        free(batch_data);
        return false;
    }

    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        if (asprintf(
                &batch_data->images_data[i].prompt,
                "A %s, %s, %s",
                selectedTheme,
                characterTraits[i][0],
                characterTraits[i][1]
            ) == -1) {
            fprintf(stderr, "Failed to construct prompt for character %d\n", i + 1);
            for (int j = 0; j < i; ++j) {
                free(batch_data->images_data[j].prompt);
            }
            free(batch_data->images_data);
            free(batch_data);
            return false;
        }

        batch_data->images_data[i].character_number = i + 1;
    }

    setGenerationStatus("Starting image generation...");
    if (pthread_create(&image_gen_master_thread, NULL, generateImageThread, batch_data) != 0) {
        fprintf(stderr, "Failed to create master image generation thread.\n");
        for (int i = 0; i < batch_data->num_images; ++i) {
            free(batch_data->images_data[i].prompt);
        }
        free(batch_data->images_data);
        free(batch_data);
        return false;
    }

    image_gen_thread_started = true;
    return true;
}

static bool loadExistingGameData(const char* gameDataFilePath) {
    char* loadedTheme = NULL;
    char** loadedFeatures = NULL;
    int loadedFeatureCount = 0;
    char*** loadedTraits = NULL;

    if (!load_game_data(
            gameDataFilePath,
            &loadedTheme,
            &loadedFeatures,
            &loadedFeatureCount,
            &loadedTraits
        )) {
        fprintf(stderr, "Failed to load existing game data. Cannot proceed without re-generating images. Exiting.\n");
        return false;
    }

    free(selectedTheme);
    selectedTheme = loadedTheme;
    characterFeatures = loadedFeatures;
    featureCount = loadedFeatureCount;
    characterTraits = loadedTraits;
    return true;
}

void* gameSetupThread(void* arg) {
    SetupThreadArgs* args = (SetupThreadArgs*)arg;
    char gameDataFilePath[MAX_FILEPATH_BUFFER_SIZE];

    setGenerationStatus("Preparing game data...");

    if (args->llm_selected) {
        int themeCount = 0;
        char** themes = getThemesFromLLM(&themeCount);

        if (themes && themeCount > 0) {
            int randomIndex = rand() % themeCount;
            selectedTheme = strdup(themes[randomIndex]);
            printf("LLM selected theme: %s\n", selectedTheme);
            for (int i = 0; i < themeCount; ++i) {
                free(themes[i]);
            }
            free(themes);
        } else {
            selectedTheme = strdup("Default");
            printf("Using default theme: Default\n");
        }
    } else {
        selectedTheme = strdup(args->theme_input);
        printf("Using theme: %s\n", selectedTheme);
    }

    if (!selectedTheme) {
        fprintf(stderr, "Failed to allocate memory for selected theme.\n");
        free(args);
        markSetupFailed();
        return NULL;
    }

    {
        char* tempFormattedName = format_theme_name(selectedTheme);
        strncpy(formattedThemeName, tempFormattedName, sizeof(formattedThemeName) - 1);
        formattedThemeName[sizeof(formattedThemeName) - 1] = '\0';
        free(tempFormattedName);
    }

    snprintf(imageDirectoryPath, sizeof(imageDirectoryPath), "images/%s", formattedThemeName);
    snprintf(gameDataFilePath, sizeof(gameDataFilePath), "%s/game_data.json", imageDirectoryPath);

    if (directory_exists(imageDirectoryPath)) {
        pthread_mutex_lock(&mutex);
        confirm_regen_prompt_active = true;
        regen_choice = -1;
        pthread_mutex_unlock(&mutex);

        pthread_mutex_lock(&mutex);
        while (confirm_regen_prompt_active && !WindowShouldClose()) {
            pthread_cond_wait(&regen_cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);

        if (WindowShouldClose()) {
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            free(args);
            return NULL;
        }

        if (regen_choice == 1) {
            setGenerationStatus("Deleting old images...");
            if (delete_files_in_directory(imageDirectoryPath) != 0) {
                fprintf(stderr, "Failed to delete old images in %s.\n", imageDirectoryPath);
                free(args);
                markSetupFailed();
                return NULL;
            }
        } else {
            setGenerationStatus("Skipping image generation. Loading existing game data...");
            if (!loadExistingGameData(gameDataFilePath) || !assignPlayerAndOpponent()) {
                free(args);
                return NULL;
            }

            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            free(args);
            return NULL;
        }
    }

    setGenerationStatus("Creating image directory...");
    if (create_directory_recursive(imageDirectoryPath) != 0) {
        fprintf(stderr, "Failed to create image directory %s.\n", imageDirectoryPath);
        free(args);
        markSetupFailed();
        return NULL;
    }

    free(args);

    setGenerationStatus("Getting character features...");
    characterFeatures = getCharacterFeatures(selectedTheme, &featureCount);
    if (!characterFeatures || featureCount <= 0) {
        printf("No character features found.\n");
        markSetupFailed();
        return NULL;
    }

    if (!buildCharacterTraits()) {
        markSetupFailed();
        return NULL;
    }

    if (!save_game_data(
            gameDataFilePath,
            selectedTheme,
            characterFeatures,
            featureCount,
            characterTraits,
            NUM_CHARACTERS
        )) {
        fprintf(stderr, "Failed to save game data after generation setup. This game's data will not be reusable.\n");
    }

    if (!startImageGeneration() || !assignPlayerAndOpponent()) {
        markSetupFailed();
        return NULL;
    }

    pthread_mutex_lock(&mutex);
    setup_in_progress = false;
    pthread_mutex_unlock(&mutex);

    return NULL;
}
