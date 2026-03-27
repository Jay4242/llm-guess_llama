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
    int* validCount
) {
    char** imagePaths = malloc((size_t)remainingCharacterCount * sizeof(char*));

    if (!imagePaths) {
        fprintf(stderr, "Failed to allocate memory for image paths\n");
        return NULL;
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
            return NULL;
        }

        snprintf(
            imagePaths[*validCount],
            MAX_FILEPATH_BUFFER_SIZE,
            "%s/character_%d.png",
            imageDirectory,
            i + 1
        );
        (*validCount)++;
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

static char* llm_generate_question(
    int llmCharacterIndex,
    const char* theme,
    int numCharacters,
    int* remainingCharacters,
    int remainingCharacterCount,
    const char* imageDirectory
) {
    char** imagePaths = NULL;
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
        &validCharacterCount
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
        return NULL;
    }

    printf("Sending %d character images to LLM...\n", validCharacterCount);

    llmQuestionResponse = getLLMResponseWithVision(
        initialPrompt,
        (const char**)imagePaths,
        validCharacterCount,
        finalPrompt,
        0.7
    );

    free(initialPrompt);
    freeImagePaths(imagePaths, validCharacterCount);

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

        question_filtered = filter_think_tags(json_string_value(content));
        json_decref(root);
    }

    free(llmQuestionResponse);
    return question_filtered;
}

void* llmGuessThread(void* arg) {
    LLMGuessThreadArgs* args = (LLMGuessThreadArgs*)arg;

    pthread_mutex_lock(&mutex);
    llm_guess_in_progress = true;
    llm_guess_success = false;
    pending_llm_question = NULL;
    if (pending_elimination_list) {
        for (int i = 0; i < pending_elimination_count; i++) {
            free(pending_elimination_list[i]);
        }
        free(pending_elimination_list);
        pending_elimination_list = NULL;
        pending_elimination_count = 0;
    }
    pthread_mutex_unlock(&mutex);

    pending_llm_question = llm_generate_question(
        args->llmCharacter,
        args->theme,
        args->numCharacters,
        args->charactersRemaining,
        *args->remainingCount,
        args->imageDirectoryPath
    );

    pthread_mutex_lock(&mutex);
    if (pending_llm_question) {
        llm_guess_success = true;
    }
    llm_guess_in_progress = false;
    pthread_mutex_unlock(&mutex);

    free(args->imageDirectoryPath);
    free(args);

    return NULL;
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
    LLMGuessThreadArgs* threadArgs = NULL;
    bool waiting_for_question = true;
    bool waiting_for_answer = false;
    bool waiting_for_elimination = false;

    threadArgs = malloc(sizeof(LLMGuessThreadArgs));
    if (!threadArgs) {
        fprintf(stderr, "Failed to allocate memory for LLM thread args\n");
        return;
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
        return;
    }

    pthread_mutex_lock(&mutex);
    llm_should_continue = true;
    pthread_mutex_unlock(&mutex);

    if (pthread_create(&llm_guess_thread, NULL, llmGuessThread, threadArgs) != 0) {
        fprintf(stderr, "Failed to create LLM guess thread\n");
        free(threadArgs->theme);
        free(threadArgs->imageDirectoryPath);
        free(threadArgs);
        return;
    }
    llm_guess_thread_started = true;

    while (!WindowShouldClose()) {
        pthread_mutex_lock(&mutex);
        bool guess_in_progress = llm_guess_in_progress;
        bool guess_success = llm_guess_success;
        char* question = pending_llm_question;
        pthread_mutex_unlock(&mutex);

        if (waiting_for_question) {
            if (!guess_in_progress) {
                if (guess_success && question) {
                    pthread_mutex_lock(&mutex);
                    strncpy(currentQuestion, question, sizeof(currentQuestion) - 1);
                    currentQuestion[sizeof(currentQuestion) - 1] = '\0';
                    pending_llm_question = NULL;
                    currentAnswer = -1;
                    pthread_mutex_unlock(&mutex);
                    waiting_for_question = false;
                    waiting_for_answer = true;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            }
        }

        if (waiting_for_answer) {
            Rectangle yesButton = {100, 120, 80, 30};
            Rectangle noButton = {200, 120, 80, 30};

            BeginDrawing();
            ClearBackground(RAYWHITE);

            DrawText(playerCharacterString, 10, 10, 20, BLACK);
            if (playerTexture.id != 0) {
                DrawTextureEx(playerTexture, (Vector2){195.2f, 170.0f}, 0.0f, 0.8f, WHITE);
            }

            pthread_mutex_lock(&mutex);
            DrawText(currentQuestion, 100, 70, 20, GRAY);
            pthread_mutex_unlock(&mutex);

            DrawRectangleRec(yesButton, GREEN);
            DrawText("Yes", (int)yesButton.x + 20, (int)yesButton.y + 5, 20, WHITE);
            DrawRectangleRec(noButton, RED);
            DrawText("No", (int)noButton.x + 20, (int)noButton.y + 5, 20, WHITE);

            EndDrawing();

            if (CheckCollisionPointRec(GetMousePosition(), yesButton) &&
                IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 1;
                waiting_for_answer = false;
                waiting_for_elimination = true;
            }
            if (CheckCollisionPointRec(GetMousePosition(), noButton) &&
                IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 0;
                waiting_for_answer = false;
                waiting_for_elimination = true;
            }

            if (WindowShouldClose()) {
                pthread_mutex_lock(&mutex);
                llm_should_continue = false;
                if (pending_elimination_list) {
                    for (int i = 0; i < pending_elimination_count; i++) {
                        free(pending_elimination_list[i]);
                    }
                    free(pending_elimination_list);
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
                if (elim_success && elim_list && elim_count > 0) {
                    for (int i = 0; i < elim_count; i++) {
                        int characterToEliminate = atoi(elim_list[i]) - 1;

                        if (characterToEliminate == playerCharacterIndex) {
                            pthread_mutex_lock(&mutex);
                            currentGameState = GAME_STATE_PLAYER_WINS;
                            pthread_mutex_unlock(&mutex);
                            printf("LLM eliminated player's character! Player wins!\n");
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

                    for (int i = 0; i < elim_count; i++) {
                        free(elim_list[i]);
                    }
                    free(elim_list);

                    pthread_mutex_lock(&mutex);
                    pending_elimination_list = NULL;
                    pending_elimination_count = 0;
                    pthread_mutex_unlock(&mutex);

                    if (*remainingCharacterCount == 1 && remainingCharacters[0] == playerCharacterIndex) {
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_LLM_WINS;
                        pthread_mutex_unlock(&mutex);
                        printf("LLM guessed player's character! LLM wins!\n");
                        return;
                    }

                    pthread_mutex_lock(&mutex);
                    llm_guess_success = false;
                    pthread_mutex_unlock(&mutex);
                    waiting_for_elimination = false;
                    waiting_for_question = true;
                } else {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                    return;
                }
            } else {
                BeginDrawing();
                ClearBackground(RAYWHITE);
                DrawText("LLM is processing your answer...",
                    SCREEN_WIDTH / 2 - MeasureText("LLM is processing your answer...", 25) / 2,
                    SCREEN_HEIGHT / 2, 25, GRAY);
                EndDrawing();
            }
        }

        if (!waiting_for_question && !waiting_for_answer && !waiting_for_elimination) {
            break;
        }
    }
}
