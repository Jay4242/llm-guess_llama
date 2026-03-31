#include "guess_llama.h"

static bool isCharacterStillInGame(int characterIndex, const int* remainingCharacters, int remainingCharacterCount) {
    for (int i = 0; i < remainingCharacterCount; ++i) {
        if (remainingCharacters[i] == characterIndex) {
            return true;
        }
    }
    return false;
}

static char** buildRemainingImagePaths(
    int llmCharacterIndex,
    int numCharacters,
    const int* remainingCharacters,
    int remainingCharacterCount,
    const char* imageDirectory,
    int* validCount,
    int** characterNumbers
) {
    char** imagePaths = malloc((size_t)remainingCharacterCount * sizeof(char*));
    int* labels = NULL;

    if (!imagePaths) {
        fprintf(stderr, "Failed to allocate memory for image paths\n");
        return NULL;
    }

    if (characterNumbers) {
        labels = calloc((size_t)remainingCharacterCount, sizeof(int));
        if (!labels) {
            fprintf(stderr, "Failed to allocate memory for image labels\n");
            free(imagePaths);
            return NULL;
        }
    }

    *validCount = 0;
    for (int i = 0; i < numCharacters; ++i) {
        if (i == llmCharacterIndex ||
            !isCharacterStillInGame(i, remainingCharacters, remainingCharacterCount)) {
            continue;
        }

        imagePaths[*validCount] = malloc(MAX_FILEPATH_BUFFER_SIZE);
        if (!imagePaths[*validCount]) {
            fprintf(stderr, "Failed to allocate memory for image path\n");
            for (int j = 0; j < *validCount; ++j) {
                free(imagePaths[j]);
            }
            free(imagePaths);
            free(labels);
            return NULL;
        }

        snprintf(
            imagePaths[*validCount],
            MAX_FILEPATH_BUFFER_SIZE,
            "%s/character_%d.png",
            imageDirectory,
            i + 1
        );

        if (labels) {
            labels[*validCount] = i + 1;
        }
        (*validCount)++;
    }

    if (characterNumbers) {
        *characterNumbers = labels;
    }

    return imagePaths;
}

static void freeImagePaths(char** imagePaths, int count) {
    if (!imagePaths) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        free(imagePaths[i]);
    }
    free(imagePaths);
}

static void freeEliminationList(char** eliminationList, int eliminationCount) {
    if (!eliminationList) {
        return;
    }

    for (int i = 0; i < eliminationCount; ++i) {
        free(eliminationList[i]);
    }
    free(eliminationList);
}

static void drawCenteredPlayerTexture(Texture2D playerTexture, float centerX, float topY, float textureScale) {
    if (playerTexture.id == 0) {
        return;
    }

    const float imageWidthOnScreen =
        (float)playerTexture.width * textureScale * getVirtualScaleX();
    const float imageHeightOnScreen =
        (float)playerTexture.height * textureScale * getVirtualScaleY();
    const float squareSizeOnScreen =
        (imageWidthOnScreen < imageHeightOnScreen) ?
            imageWidthOnScreen :
            imageHeightOnScreen;
    const float destinationWidth = squareSizeOnScreen / getVirtualScaleX();
    const float destinationHeight = squareSizeOnScreen / getVirtualScaleY();
    const Rectangle destination = {
        centerX - destinationWidth * 0.5f,
        topY,
        destinationWidth,
        destinationHeight
    };

    DrawTexturePro(
        playerTexture,
        (Rectangle){0, 0, (float)playerTexture.width, (float)playerTexture.height},
        destination,
        (Vector2){0.0f, 0.0f},
        0.0f,
        WHITE
    );
}

static float getPlayerTextureSquareSize(Texture2D playerTexture, float textureScale) {
    if (playerTexture.id == 0) {
        return 0.0f;
    }

    const float imageWidthOnScreen =
        (float)playerTexture.width * textureScale * getVirtualScaleX();
    const float imageHeightOnScreen =
        (float)playerTexture.height * textureScale * getVirtualScaleY();
    const float squareSizeOnScreen =
        (imageWidthOnScreen < imageHeightOnScreen) ?
            imageWidthOnScreen :
            imageHeightOnScreen;

    return squareSizeOnScreen / getVirtualScaleY();
}

static char* normalizeYesNoAnswer(const char* answerText) {
    const char* cursor = answerText;
    char token[16] = {0};
    int tokenLength = 0;

    if (!answerText) {
        return NULL;
    }

    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        ++cursor;
    }

    while (*cursor != '\0' && tokenLength < (int)sizeof(token) - 1) {
        if (isalpha((unsigned char)*cursor)) {
            token[tokenLength++] = (char)tolower((unsigned char)*cursor);
        } else if (tokenLength > 0) {
            break;
        }
        ++cursor;
    }

    token[tokenLength] = '\0';

    if (strncmp(token, "yes", 3) == 0) {
        return strdup("Yes");
    }

    if (strncmp(token, "no", 2) == 0) {
        return strdup("No");
    }

    return NULL;
}

typedef struct {
    char* theme;
    char* question;
    int llmCharacterIndex;
    char* imageDirectoryPath;
} PlayerQuestionThreadArgs;

static void* playerQuestionThread(void* arg) {
    PlayerQuestionThreadArgs* args = (PlayerQuestionThreadArgs*)arg;
    char* answer = getPlayerQuestionYesNoAnswer(
        args->theme,
        args->question,
        args->llmCharacterIndex,
        args->imageDirectoryPath
    );

    pthread_mutex_lock(&mutex);
    if (pending_player_answer) {
        free(pending_player_answer);
        pending_player_answer = NULL;
    }
    pending_player_answer = answer;
    player_question_success = answer != NULL;
    player_question_in_progress = false;
    pthread_mutex_unlock(&mutex);

    free(args->theme);
    free(args->question);
    free(args->imageDirectoryPath);
    free(args);
    return NULL;
}

bool startPlayerQuestionThread(
    const char* theme,
    const char* question,
    int llmCharacterIndex,
    const char* imageDirectory
) {
    PlayerQuestionThreadArgs* threadArgs = malloc(sizeof(PlayerQuestionThreadArgs));

    if (!threadArgs) {
        fprintf(stderr, "Failed to allocate player question thread args\n");
        return false;
    }

    threadArgs->theme = strdup(theme);
    threadArgs->question = strdup(question);
    threadArgs->llmCharacterIndex = llmCharacterIndex;
    threadArgs->imageDirectoryPath = strdup(imageDirectory);

    if (!threadArgs->theme || !threadArgs->question || !threadArgs->imageDirectoryPath) {
        fprintf(stderr, "Failed to allocate player question thread strings\n");
        free(threadArgs->theme);
        free(threadArgs->question);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return false;
    }

    if (player_question_thread_started) {
        pthread_join(player_question_thread, NULL);
        player_question_thread_started = false;
    }

    pthread_mutex_lock(&mutex);
    if (pending_player_answer) {
        free(pending_player_answer);
        pending_player_answer = NULL;
    }
    player_question_in_progress = true;
    player_question_success = false;
    pthread_mutex_unlock(&mutex);

    if (pthread_create(&player_question_thread, NULL, playerQuestionThread, threadArgs) != 0) {
        fprintf(stderr, "Failed to create player question thread\n");
        pthread_mutex_lock(&mutex);
        player_question_in_progress = false;
        pthread_mutex_unlock(&mutex);
        free(threadArgs->theme);
        free(threadArgs->question);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return false;
    }

    player_question_thread_started = true;
    return true;
}

char* getPlayerQuestionYesNoAnswer(
    const char* theme,
    const char* question,
    int llmCharacterIndex,
    const char* imageDirectory
) {
    char llmCharacterImagePath[MAX_FILEPATH_BUFFER_SIZE];
    const char* imagePaths[1];
    int imageLabel = llmCharacterIndex + 1;
    char* initialPrompt = NULL;
    const char* finalPrompt =
        "Answer this question about the shown character using only one word: yes or no. "
        "Do not include punctuation or any additional words.";
    char* llmResponse = NULL;
    char* responseContent = NULL;
    char* filteredContent = NULL;
    char* normalizedAnswer = NULL;
    json_error_t error;
    json_t* root = NULL;
    json_t* choices = NULL;
    json_t* firstChoice = NULL;
    json_t* message = NULL;
    json_t* content = NULL;

    snprintf(
        llmCharacterImagePath,
        sizeof(llmCharacterImagePath),
        "%s/character_%d.png",
        imageDirectory,
        llmCharacterIndex + 1
    );
    imagePaths[0] = llmCharacterImagePath;

    if (asprintf(
            &initialPrompt,
            "You are answering a player's Guess Who question for theme '%s'. "
            "The player asked: '%s'. I am showing only your secret character image. "
            "Return whether the statement is true for this character.",
            theme,
            question
        ) == -1) {
        fprintf(stderr, "Failed to build player question prompt\n");
        return NULL;
    }

    llmResponse = getLLMResponseWithVision(
        initialPrompt,
        imagePaths,
        1,
        &imageLabel,
        finalPrompt,
        0.1
    );
    free(initialPrompt);

    if (!llmResponse) {
        fprintf(stderr, "Failed to get player question response from LLM\n");
        return NULL;
    }

    root = json_loads(llmResponse, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing LLM JSON response: %s\n", error.text);
        goto cleanup;
    }

    choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        fprintf(stderr, "Error: choices missing in player question response\n");
        goto cleanup;
    }

    firstChoice = json_array_get(choices, 0);
    if (!json_is_object(firstChoice)) {
        fprintf(stderr, "Error: first choice missing in player question response\n");
        goto cleanup;
    }

    message = json_object_get(firstChoice, "message");
    if (!json_is_object(message)) {
        fprintf(stderr, "Error: message missing in player question response\n");
        goto cleanup;
    }

    content = json_object_get(message, "content");
    if (!json_is_string(content)) {
        fprintf(stderr, "Error: content missing in player question response\n");
        goto cleanup;
    }

    responseContent = strdup(json_string_value(content));
    if (!responseContent) {
        fprintf(stderr, "Failed to allocate player response content\n");
        goto cleanup;
    }

    filteredContent = filter_think_tags(responseContent);
    if (!filteredContent) {
        fprintf(stderr, "Failed to filter player response content\n");
        goto cleanup;
    }

    normalizedAnswer = normalizeYesNoAnswer(filteredContent);
    if (!normalizedAnswer) {
        fprintf(stderr, "Could not normalize LLM yes/no response: %s\n", filteredContent);
    }

cleanup:
    free(llmResponse);
    free(responseContent);
    free(filteredContent);
    if (root) {
        json_decref(root);
    }
    return normalizedAnswer;
}

static bool getSingleCandidateFromLLMPerspective(
    int llmCharacterIndex,
    const int* remainingCharacters,
    int remainingCharacterCount,
    int* candidateIndex
) {
    int possibleCount = 0;
    int lastCandidate = -1;

    for (int i = 0; i < remainingCharacterCount; ++i) {
        if (remainingCharacters[i] == llmCharacterIndex) {
            continue;
        }

        lastCandidate = remainingCharacters[i];
        ++possibleCount;
        if (possibleCount > 1) {
            break;
        }
    }

    if (candidateIndex) {
        *candidateIndex = (possibleCount == 1) ? lastCandidate : -1;
    }

    return possibleCount == 1;
}

static char** llm_generate_elimination_list(
    int llmCharacterIndex,
    const char* theme,
    const char* question,
    int answer,
    int numCharacters,
    int* remainingCharacters,
    int remainingCharacterCount,
    const char* imageDirectory,
    int* eliminationCount
) {
    char** imagePaths = NULL;
    int* imageCharacterNumbers = NULL;
    int validCharacterCount = 0;
    char* eliminationInstruction = NULL;
    char* eliminationInitialPrompt = NULL;
    char* eliminationFinalPrompt = NULL;
    char* llmEliminationResponse = NULL;
    char* filteredContent = NULL;
    char* jsonContent = NULL;
    char** eliminationList = NULL;
    const char* answerString = answer ? "yes" : "no";
    json_error_t error;
    json_t* root = NULL;
    json_t* choices = NULL;
    json_t* firstChoice = NULL;
    json_t* message = NULL;
    json_t* content = NULL;
    json_t* contentArray = NULL;

    *eliminationCount = 0;

    imagePaths = buildRemainingImagePaths(
        llmCharacterIndex,
        numCharacters,
        remainingCharacters,
        remainingCharacterCount,
        imageDirectory,
        &validCharacterCount,
        &imageCharacterNumbers
    );
    if (!imagePaths) {
        return NULL;
    }

    if (answer == 0) {
        if (asprintf(
                &eliminationInstruction,
                "This means the player's character does NOT have the feature asked about. "
                "Therefore, eliminate all characters from the list that do have the feature asked about."
            ) == -1) {
            fprintf(stderr, "Failed to construct elimination instruction for NO\n");
            freeImagePaths(imagePaths, validCharacterCount);
            free(imageCharacterNumbers);
            return NULL;
        }
    } else {
        if (asprintf(
                &eliminationInstruction,
                "This means the player's character DOES have the feature asked about. "
                "Therefore, eliminate all characters from the list that do not have the feature asked about."
            ) == -1) {
            fprintf(stderr, "Failed to construct elimination instruction for YES\n");
            freeImagePaths(imagePaths, validCharacterCount);
            free(imageCharacterNumbers);
            return NULL;
        }
    }

    if (asprintf(
            &eliminationInitialPrompt,
            "You are playing a game of 'Guess Who?'. The theme is '%s'. The question '%s' "
            "was asked, and the answer was '%s'. I will show you the remaining character "
            "images (excluding your own), each labeled as 'Character N'.",
            theme,
            question,
            answerString
        ) == -1) {
        fprintf(stderr, "Failed to construct elimination initial prompt\n");
        freeImagePaths(imagePaths, validCharacterCount);
        free(imageCharacterNumbers);
        free(eliminationInstruction);
        return NULL;
    }

    if (asprintf(
            &eliminationFinalPrompt,
            "%s Return a JSON list of integers containing only the character numbers "
            "to eliminate, and nothing else.",
            eliminationInstruction
        ) == -1) {
        fprintf(stderr, "Failed to construct elimination final prompt\n");
        freeImagePaths(imagePaths, validCharacterCount);
        free(imageCharacterNumbers);
        free(eliminationInstruction);
        free(eliminationInitialPrompt);
        return NULL;
    }

    printf("[LLM/Elimination] Sending %d character images to LLM...\n", validCharacterCount);
    printf("[LLM/Elimination] Initial prompt: %s\n", eliminationInitialPrompt);
    printf("[LLM/Elimination] Final prompt: %s\n", eliminationFinalPrompt);

    llmEliminationResponse = getLLMResponseWithVision(
        eliminationInitialPrompt,
        (const char**)imagePaths,
        validCharacterCount,
        imageCharacterNumbers,
        eliminationFinalPrompt,
        0.7
    );

    freeImagePaths(imagePaths, validCharacterCount);
    free(imageCharacterNumbers);
    imagePaths = NULL;
    imageCharacterNumbers = NULL;

    if (!llmEliminationResponse) {
        fprintf(stderr, "Failed to get elimination response from LLM\n");
        free(eliminationInstruction);
        free(eliminationInitialPrompt);
        free(eliminationFinalPrompt);
        return NULL;
    }

    printf("Raw LLM Elimination Response: %s\n", llmEliminationResponse);

    root = json_loads(llmEliminationResponse, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        goto cleanup;
    }

    choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        fprintf(stderr, "Error: 'choices' is not a non-empty array for elimination.\n");
        goto cleanup;
    }

    firstChoice = json_array_get(choices, 0);
    if (!json_is_object(firstChoice)) {
        fprintf(stderr, "Error: First choice is not an object for elimination.\n");
        goto cleanup;
    }

    message = json_object_get(firstChoice, "message");
    if (!json_is_object(message)) {
        fprintf(stderr, "Error: 'message' is not an object for elimination.\n");
        goto cleanup;
    }

    content = json_object_get(message, "content");
    if (!json_is_string(content)) {
        fprintf(stderr, "Error: 'content' is not a string for elimination.\n");
        goto cleanup;
    }

    filteredContent = filter_think_tags(json_string_value(content));
    if (!filteredContent) {
        fprintf(stderr, "Failed to filter elimination content\n");
        goto cleanup;
    }

    jsonContent = extract_json_from_markdown(filteredContent);
    if (!jsonContent) {
        fprintf(stderr, "Failed to extract JSON from elimination response\n");
        goto cleanup;
    }

    contentArray = json_loads(jsonContent, 0, &error);
    if (!json_is_array(contentArray)) {
        fprintf(stderr, "Error parsing content as JSON array for elimination: %s\n", error.text);
        goto cleanup;
    }

    *eliminationCount = (int)json_array_size(contentArray);
    if (*eliminationCount > 0) {
        eliminationList = calloc((size_t)*eliminationCount, sizeof(char*));
        if (!eliminationList) {
            fprintf(stderr, "Failed to allocate elimination list\n");
            *eliminationCount = 0;
            goto cleanup;
        }
    }

    for (int i = 0; i < *eliminationCount; ++i) {
        json_t* item = json_array_get(contentArray, (size_t)i);
        long long value;

        if (json_is_integer(item)) {
            value = json_integer_value(item);
        } else if (json_is_string(item)) {
            value = atoll(json_string_value(item));
        } else {
            fprintf(stderr, "Error: elimination list item is not an integer/string\n");
            freeEliminationList(eliminationList, i);
            eliminationList = NULL;
            *eliminationCount = 0;
            goto cleanup;
        }

        if (asprintf(&eliminationList[i], "%lld", value) == -1) {
            fprintf(stderr, "Failed to allocate elimination list item\n");
            freeEliminationList(eliminationList, i);
            eliminationList = NULL;
            *eliminationCount = 0;
            goto cleanup;
        }
    }

cleanup:
    freeImagePaths(imagePaths, validCharacterCount);
    free(imageCharacterNumbers);
    free(eliminationInstruction);
    free(eliminationInitialPrompt);
    free(eliminationFinalPrompt);
    free(llmEliminationResponse);
    free(filteredContent);
    free(jsonContent);
    json_decref(root);
    json_decref(contentArray);

    return eliminationList;
}

typedef struct {
    int llmCharacter;
    char* theme;
    char* question;
    int answer;
    int numCharacters;
    int* remainingCharacters;
    int remainingCharacterCount;
    char* imageDirectoryPath;
} LLMEliminationThreadArgs;

static char* llm_generate_question(
    int llmCharacterIndex,
    const char* theme,
    int numCharacters,
    int* remainingCharacters,
    int remainingCharacterCount,
    const char* imageDirectory
) {
    char** imagePaths = NULL;
    int* imageCharacterNumbers = NULL;
    int validCharacterCount = 0;
    char* initialPrompt = NULL;
    const char* finalPrompt =
        "Based on the character images shown above, formulate a yes/no question "
        "about a single visual trait that will help you narrow down which character "
        "the player has. Return only the question, nothing else.";
    char* llmQuestionResponse = NULL;
    char* question_filtered = NULL;

    imagePaths = buildRemainingImagePaths(
        llmCharacterIndex,
        numCharacters,
        remainingCharacters,
        remainingCharacterCount,
        imageDirectory,
        &validCharacterCount,
        &imageCharacterNumbers
    );
    if (!imagePaths) {
        return NULL;
    }

    if (asprintf(
            &initialPrompt,
            "You are playing a game of 'Guess Who?'. The theme is '%s'. I will show "
            "you images of all the remaining characters (excluding your own character). "
            "Your goal is to guess the player's character by asking yes/no questions. "
            "Start by formulating a yes/no question that will help you narrow down the "
            "possibilities. The question should be about a single visual trait that can "
            "be seen in the images. Return only the question as a string, nothing else.",
            theme
        ) == -1) {
        fprintf(stderr, "Failed to construct initial prompt\n");
        freeImagePaths(imagePaths, validCharacterCount);
        free(imageCharacterNumbers);
        return NULL;
    }

    printf("Sending %d character images to LLM...\n", validCharacterCount);

    llmQuestionResponse = getLLMResponseWithVision(
        initialPrompt,
        (const char**)imagePaths,
        validCharacterCount,
        imageCharacterNumbers,
        finalPrompt,
        0.7
    );

    free(initialPrompt);
    freeImagePaths(imagePaths, validCharacterCount);
    free(imageCharacterNumbers);

    if (!llmQuestionResponse) {
        fprintf(stderr, "Failed to get response from LLM\n");
        return NULL;
    }

    {
        json_error_t error;
        json_t* root = json_loads(llmQuestionResponse, 0, &error);
        json_t* choices;
        json_t* firstChoice;
        json_t* message;
        json_t* content;

        if (!root) {
            fprintf(stderr, "Error parsing JSON: %s\n", error.text);
            free(llmQuestionResponse);
            return NULL;
        }

        choices = json_object_get(root, "choices");
        if (!json_is_array(choices) || json_array_size(choices) == 0) {
            fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return NULL;
        }

        firstChoice = json_array_get(choices, 0);
        if (!json_is_object(firstChoice)) {
            fprintf(stderr, "Error: First choice is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return NULL;
        }

        message = json_object_get(firstChoice, "message");
        if (!json_is_object(message)) {
            fprintf(stderr, "Error: 'message' is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return NULL;
        }

        content = json_object_get(message, "content");
        if (!json_is_string(content)) {
            fprintf(stderr, "Error: 'content' is not a string.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return NULL;
        }

        fprintf(stderr, "[LLM Response Raw]: %s\n", json_string_value(content));
        question_filtered = filter_think_tags(json_string_value(content));
        fprintf(stderr, "[LLM Response Filtered]: %s\n", question_filtered);
        json_decref(root);
    }

    free(llmQuestionResponse);
    return question_filtered;
}

void* llmGuessThread(void* arg) {
    LLMGuessThreadArgs* args = (LLMGuessThreadArgs*)arg;
    char* generatedQuestion = NULL;

    generatedQuestion = llm_generate_question(
        args->llmCharacter,
        args->theme,
        args->numCharacters,
        args->charactersRemaining,
        *args->remainingCount,
        args->imageDirectoryPath
    );

    pthread_mutex_lock(&mutex);
    pending_llm_question = generatedQuestion;
    if (pending_llm_question) {
        llm_guess_success = true;
    }
    llm_guess_in_progress = false;
    pthread_mutex_unlock(&mutex);

    free(args->theme);
    free(args->imageDirectoryPath);
    free(args);

    return NULL;
}

static void* llmEliminationThread(void* arg) {
    LLMEliminationThreadArgs* args = (LLMEliminationThreadArgs*)arg;
    char** eliminationList = NULL;
    int eliminationCount = 0;

    eliminationList = llm_generate_elimination_list(
        args->llmCharacter,
        args->theme,
        args->question,
        args->answer,
        args->numCharacters,
        args->remainingCharacters,
        args->remainingCharacterCount,
        args->imageDirectoryPath,
        &eliminationCount
    );

    pthread_mutex_lock(&mutex);
    pending_elimination_list = eliminationList;
    pending_elimination_count = eliminationCount;
    llm_guess_success = eliminationList != NULL || eliminationCount == 0;
    llm_guess_in_progress = false;
    pthread_mutex_unlock(&mutex);

    free(args->theme);
    free(args->question);
    free(args->imageDirectoryPath);
    free(args);
    return NULL;
}

static bool startQuestionGenerationThread(
    int llmCharacterIndex,
    const char* theme,
    int numCharacters,
    int* remainingCharacters,
    int* remainingCharacterCount,
    Texture2D playerTexture,
    int playerCharacterIndex,
    const char* imageDirectory
) {
    LLMGuessThreadArgs* threadArgs = malloc(sizeof(LLMGuessThreadArgs));

    if (llm_guess_thread_started) {
        pthread_join(llm_guess_thread, NULL);
        llm_guess_thread_started = false;
    }

    if (!threadArgs) {
        fprintf(stderr, "Failed to allocate memory for LLM thread args\n");
        return false;
    }

    threadArgs->llmCharacter = llmCharacterIndex;
    threadArgs->theme = strdup(theme);
    threadArgs->numCharacters = numCharacters;
    threadArgs->charactersRemaining = remainingCharacters;
    threadArgs->remainingCount = remainingCharacterCount;
    threadArgs->playerTexture = playerTexture;
    threadArgs->playerCharacter = playerCharacterIndex;
    threadArgs->imageDirectoryPath = strdup(imageDirectory);

    if (!threadArgs->theme || !threadArgs->imageDirectoryPath) {
        free(threadArgs->theme);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        fprintf(stderr, "Failed to allocate memory for thread args strings\n");
        return false;
    }

    pthread_mutex_lock(&mutex);
    llm_should_continue = true;
    llm_guess_in_progress = true;
    llm_guess_success = false;
    if (pending_llm_question) {
        free(pending_llm_question);
        pending_llm_question = NULL;
    }
    pthread_mutex_unlock(&mutex);

    if (pthread_create(&llm_guess_thread, NULL, llmGuessThread, threadArgs) != 0) {
        fprintf(stderr, "Failed to create LLM guess thread\n");
        pthread_mutex_lock(&mutex);
        llm_guess_in_progress = false;
        pthread_mutex_unlock(&mutex);
        free(threadArgs->theme);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return false;
    }

    llm_guess_thread_started = true;
    return true;
}

static bool startEliminationThread(
    int llmCharacterIndex,
    const char* theme,
    const char* question,
    int answer,
    int numCharacters,
    int* remainingCharacters,
    int remainingCharacterCount,
    const char* imageDirectory
) {
    LLMEliminationThreadArgs* threadArgs = malloc(sizeof(LLMEliminationThreadArgs));

    if (llm_guess_thread_started) {
        pthread_join(llm_guess_thread, NULL);
        llm_guess_thread_started = false;
    }

    if (!threadArgs) {
        fprintf(stderr, "Failed to allocate memory for elimination thread args\n");
        return false;
    }

    threadArgs->llmCharacter = llmCharacterIndex;
    threadArgs->theme = strdup(theme);
    threadArgs->question = strdup(question);
    threadArgs->answer = answer;
    threadArgs->numCharacters = numCharacters;
    threadArgs->remainingCharacters = remainingCharacters;
    threadArgs->remainingCharacterCount = remainingCharacterCount;
    threadArgs->imageDirectoryPath = strdup(imageDirectory);

    if (!threadArgs->theme || !threadArgs->question || !threadArgs->imageDirectoryPath) {
        fprintf(stderr, "Failed to allocate elimination thread strings\n");
        free(threadArgs->theme);
        free(threadArgs->question);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return false;
    }

    pthread_mutex_lock(&mutex);
    llm_guess_in_progress = true;
    llm_guess_success = false;
    if (pending_elimination_list) {
        freeEliminationList(pending_elimination_list, pending_elimination_count);
        pending_elimination_list = NULL;
        pending_elimination_count = 0;
    }
    pthread_mutex_unlock(&mutex);

    if (pthread_create(&llm_guess_thread, NULL, llmEliminationThread, threadArgs) != 0) {
        fprintf(stderr, "Failed to create LLM elimination thread\n");
        pthread_mutex_lock(&mutex);
        llm_guess_in_progress = false;
        pthread_mutex_unlock(&mutex);
        free(threadArgs->theme);
        free(threadArgs->question);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return false;
    }

    llm_guess_thread_started = true;
    return true;
}

void llmGuessingRound(
    int llmCharacterIndex,
    const char* theme,
    int numCharacters,
    int* remainingCharacters,
    int* remainingCharacterCount,
    Texture2D playerTexture,
    int playerCharacterIndex,
    const char* imageDirectory
) {
    bool waiting_for_question = true;
    bool waiting_for_answer = false;
    bool waiting_for_elimination = false;
    int singleCandidate = -1;
    char playerIdentityText[64];
    const float playerImageScale = 0.78f;

    snprintf(
        playerIdentityText,
        sizeof(playerIdentityText),
        "You are Character %d",
        playerCharacterIndex + 1
    );

    pthread_mutex_lock(&mutex);
    if (pending_elimination_list) {
        freeEliminationList(pending_elimination_list, pending_elimination_count);
        pending_elimination_list = NULL;
        pending_elimination_count = 0;
    }
    pthread_mutex_unlock(&mutex);

    if (getSingleCandidateFromLLMPerspective(
            llmCharacterIndex,
            remainingCharacters,
            *remainingCharacterCount,
            &singleCandidate
        ) &&
        singleCandidate == playerCharacterIndex) {
        pthread_mutex_lock(&mutex);
        currentGameState = GAME_STATE_LLM_WINS;
        pthread_mutex_unlock(&mutex);
        printf("LLM narrowed it down to the player's character! LLM wins!\n");
        return;
    }

    if (!startQuestionGenerationThread(
            llmCharacterIndex,
            theme,
            numCharacters,
            remainingCharacters,
            remainingCharacterCount,
            playerTexture,
            playerCharacterIndex,
            imageDirectory
        )) {
        return;
    }

    while (!WindowShouldClose()) {
        pthread_mutex_lock(&mutex);
        bool guess_in_progress = llm_guess_in_progress;
        bool guess_success = llm_guess_success;
        char* question = pending_llm_question;
        pthread_mutex_unlock(&mutex);

        if (waiting_for_question) {
            if (!guess_in_progress) {
                if (guess_success && question) {
                    char* consumedQuestion = NULL;
                    pthread_mutex_lock(&mutex);
                    consumedQuestion = pending_llm_question;
                    strncpy(currentQuestion, consumedQuestion, sizeof(currentQuestion) - 1);
                    currentQuestion[sizeof(currentQuestion) - 1] = '\0';
                    pending_llm_question = NULL;
                    currentAnswer = -1;
                    pthread_mutex_unlock(&mutex);
                    free(consumedQuestion);
                    waiting_for_question = false;
                    waiting_for_answer = true;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            } else {
                const int titleFontSize = 24;
                const float titleY = 18.0f;
                const float imageTopY = titleY + (float)titleFontSize + 12.0f;
                const float imageBottomY =
                    imageTopY + getPlayerTextureSquareSize(playerTexture, playerImageScale);
                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText(
                    playerIdentityText,
                    SCREEN_WIDTH / 2 - MeasureText(playerIdentityText, titleFontSize) / 2,
                    (int)titleY,
                    titleFontSize,
                    BLACK
                );
                drawCenteredPlayerTexture(playerTexture, (float)SCREEN_WIDTH * 0.5f, imageTopY, playerImageScale);

                DrawText(
                    "LLM is generating a question...",
                    SCREEN_WIDTH / 2 - MeasureText("LLM is generating a question...", 20) / 2,
                    (int)imageBottomY + 22,
                    20,
                    GRAY
                );

                endVirtualFrame();
                continue;
            }
        }

        if (waiting_for_answer) {
            const int titleFontSize = 24;
            const int questionFontSize = 20;
            const float titleY = 18.0f;
            const float imageTopY = titleY + (float)titleFontSize + 12.0f;
            const float imageBottomY = imageTopY + getPlayerTextureSquareSize(playerTexture, playerImageScale);
            const int buttonWidth = 80;
            const int buttonHeight = 30;
            const int buttonGap = 20;
            const int buttonsTotalWidth = buttonWidth * 2 + buttonGap;
            const int buttonsStartX = SCREEN_WIDTH / 2 - buttonsTotalWidth / 2;
            Rectangle yesButton = {
                (float)buttonsStartX,
                imageBottomY + 72.0f,
                (float)buttonWidth,
                (float)buttonHeight
            };
            Rectangle noButton = {
                (float)(buttonsStartX + buttonWidth + buttonGap),
                imageBottomY + 72.0f,
                (float)buttonWidth,
                (float)buttonHeight
            };

            beginVirtualFrame();
            ClearBackground(RAYWHITE);

            DrawText(
                playerIdentityText,
                SCREEN_WIDTH / 2 - MeasureText(playerIdentityText, titleFontSize) / 2,
                (int)titleY,
                titleFontSize,
                BLACK
            );
            drawCenteredPlayerTexture(playerTexture, (float)SCREEN_WIDTH * 0.5f, imageTopY, playerImageScale);

            pthread_mutex_lock(&mutex);
            DrawText(
                currentQuestion,
                SCREEN_WIDTH / 2 - MeasureText(currentQuestion, questionFontSize) / 2,
                (int)imageBottomY + 30,
                questionFontSize,
                GRAY
            );
            pthread_mutex_unlock(&mutex);

            DrawRectangleRec(yesButton, GREEN);
            DrawText("Yes", (int)yesButton.x + 20, (int)yesButton.y + 5, 20, WHITE);
            DrawRectangleRec(noButton, RED);
            DrawText("No", (int)noButton.x + 20, (int)noButton.y + 5, 20, WHITE);

            endVirtualFrame();

            if (CheckCollisionPointRec(getVirtualMousePosition(), yesButton) &&
                IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 1;
                if (startEliminationThread(
                        llmCharacterIndex,
                        theme,
                        currentQuestion,
                        currentAnswer,
                        numCharacters,
                        remainingCharacters,
                        *remainingCharacterCount,
                        imageDirectory
                    )) {
                    waiting_for_answer = false;
                    waiting_for_elimination = true;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            }
            if (CheckCollisionPointRec(getVirtualMousePosition(), noButton) &&
                IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 0;
                if (startEliminationThread(
                        llmCharacterIndex,
                        theme,
                        currentQuestion,
                        currentAnswer,
                        numCharacters,
                        remainingCharacters,
                        *remainingCharacterCount,
                        imageDirectory
                    )) {
                    waiting_for_answer = false;
                    waiting_for_elimination = true;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            }

            if (WindowShouldClose()) {
                pthread_mutex_lock(&mutex);
                llm_should_continue = false;
                if (pending_elimination_list) {
                    freeEliminationList(pending_elimination_list, pending_elimination_count);
                    pending_elimination_list = NULL;
                    pending_elimination_count = 0;
                }
                pthread_mutex_unlock(&mutex);
                return;
            }

            continue;
        }

        if (waiting_for_elimination) {
            pthread_mutex_lock(&mutex);
            bool elim_in_progress = llm_guess_in_progress;
            bool elim_success = llm_guess_success;
            char** elim_list = pending_elimination_list;
            int elim_count = pending_elimination_count;
            pthread_mutex_unlock(&mutex);

            if (!elim_in_progress) {
                if (elim_success) {
                    bool playerEliminated = false;

                    for (int i = 0; i < elim_count; i++) {
                        int characterToEliminate = atoi(elim_list[i]) - 1;

                        if (characterToEliminate == playerCharacterIndex) {
                            pthread_mutex_lock(&mutex);
                            currentGameState = GAME_STATE_PLAYER_WINS;
                            pthread_mutex_unlock(&mutex);
                            printf("LLM eliminated player's character! Player wins!\n");
                            playerEliminated = true;
                            break;
                        }

                        for (int j = 0; j < *remainingCharacterCount; ++j) {
                            if (remainingCharacters[j] != characterToEliminate) {
                                continue;
                            }

                            for (int k = j; k < *remainingCharacterCount - 1; ++k) {
                                remainingCharacters[k] = remainingCharacters[k + 1];
                            }
                            (*remainingCharacterCount)--;
                            printf("Eliminating character %d\n", characterToEliminate + 1);
                            break;
                        }
                    }

                    freeEliminationList(elim_list, elim_count);

                    pthread_mutex_lock(&mutex);
                    pending_elimination_list = NULL;
                    pending_elimination_count = 0;
                    pthread_mutex_unlock(&mutex);

                    if (playerEliminated) {
                        return;
                    }

                    if (getSingleCandidateFromLLMPerspective(
                            llmCharacterIndex,
                            remainingCharacters,
                            *remainingCharacterCount,
                            &singleCandidate
                        ) &&
                        singleCandidate == playerCharacterIndex) {
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_LLM_WINS;
                        pthread_mutex_unlock(&mutex);
                        printf("LLM narrowed it down to the player's character! LLM wins!\n");
                        return;
                    }

                    pthread_mutex_lock(&mutex);
                    llm_guess_success = false;
                    pthread_mutex_unlock(&mutex);
                    waiting_for_elimination = false;
                    return;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            } else {
                beginVirtualFrame();
                ClearBackground(RAYWHITE);
                DrawText("LLM is processing your answer...",
                    SCREEN_WIDTH / 2 - MeasureText("LLM is processing your answer...", 25) / 2,
                    SCREEN_HEIGHT / 2, 25, GRAY);
                endVirtualFrame();
            }
        }

        if (!waiting_for_question && !waiting_for_answer && !waiting_for_elimination) {
            break;
        }
    }
}
