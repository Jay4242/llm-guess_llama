#include "guess_llama.h"

static void drawImageGenerationProgressScreen(void) {
    beginVirtualFrame();
    ClearBackground(RAYWHITE);
    pthread_mutex_lock(&mutex);
    DrawText(generation_status_message, 10, 10, 20, BLACK);
    DrawText(current_percent, 10, 40, 20, BLACK);
    pthread_mutex_unlock(&mutex);
    endVirtualFrame();
}

int main(void) {
    typedef enum {
        PLAYER_TURN_PHASE_ASK_QUESTION,
        PLAYER_TURN_PHASE_WAITING_FOR_ANSWER,
        PLAYER_TURN_PHASE_ELIMINATION
    } PlayerTurnPhase;

    char theme_input_buffer[100] = {0};
    bool themeInputSelected = false;
    Rectangle themeInputBox = {100, 100, 200, 30};
    Rectangle llmThemeButton = {320, 100, 200, 30};
    bool llmThemeSelected = false;
    bool guessingRoundStarted = false;
    bool playerTurnActive = false;
    bool playerQuestionInputSelected = false;
    bool playerQuestionRequestPending = false;
    char playerQuestionInput[256] = {0};
    char playerLastQuestion[256] = {0};
    char playerLastAnswer[64] = {0};
    PlayerTurnPhase playerTurnPhase = PLAYER_TURN_PHASE_ASK_QUESTION;

    initRuntimeConfig();
    srand((unsigned int)time(NULL));

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Guess Llama");
    SetWindowMinSize(SCREEN_WIDTH, SCREEN_HEIGHT);
    SetTargetFPS(60);

    if (!initVirtualRendering()) {
        CloseWindow();
        return 1;
    }

    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed.\n");
        if (virtualRenderTarget.id != 0) {
            UnloadRenderTexture(virtualRenderTarget);
            virtualRenderTarget.id = 0;
        }
        CloseWindow();
        return 1;
    }

    if (pthread_cond_init(&regen_cond, NULL) != 0) {
        fprintf(stderr, "Condition variable initialization failed.\n");
        pthread_mutex_destroy(&mutex);
        if (virtualRenderTarget.id != 0) {
            UnloadRenderTexture(virtualRenderTarget);
            virtualRenderTarget.id = 0;
        }
        CloseWindow();
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (!WindowShouldClose() && currentGameState != GAME_STATE_EXIT) {
        GameState state;
        bool current_confirm_regen_prompt_active;

        refreshVirtualViewport();

        pthread_mutex_lock(&mutex);
        state = currentGameState;
        current_confirm_regen_prompt_active = confirm_regen_prompt_active;
        pthread_mutex_unlock(&mutex);

        if (state == GAME_STATE_IMAGE_GENERATION && current_confirm_regen_prompt_active) {
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_CONFIRM_REGENERATE_IMAGES;
            pthread_mutex_unlock(&mutex);
            state = GAME_STATE_CONFIRM_REGENERATE_IMAGES;
        }

        switch (state) {
            case GAME_STATE_THEME_SELECTION: {
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    themeInputSelected = CheckCollisionPointRec(getVirtualMousePosition(), themeInputBox);
                }

                int key = GetCharPressed();
                while (key > 0) {
                    if (themeInputSelected) {
                        int len = (int)strlen(theme_input_buffer);
                        if (key >= 32 && key <= 125 && len < (int)sizeof(theme_input_buffer) - 1) {
                            theme_input_buffer[len] = (char)key;
                            theme_input_buffer[len + 1] = '\0';
                        }
                    }
                    key = GetCharPressed();
                }

                if (IsKeyPressed(KEY_BACKSPACE) && themeInputSelected) {
                    int len = (int)strlen(theme_input_buffer);
                    if (len > 0) {
                        theme_input_buffer[len - 1] = '\0';
                    }
                }

                if (IsKeyPressed(KEY_ENTER) && themeInputSelected && strlen(theme_input_buffer) > 0) {
                    llmThemeSelected = false;
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_THEME_READY;
                    pthread_mutex_unlock(&mutex);
                }

                if (CheckCollisionPointRec(getVirtualMousePosition(), llmThemeButton) &&
                    IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    llmThemeSelected = true;
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_THEME_READY;
                    pthread_mutex_unlock(&mutex);
                }

                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText("Enter a theme:", 100, 70, 20, GRAY);
                DrawRectangleRec(themeInputBox, LIGHTGRAY);
                DrawText(theme_input_buffer, (int)themeInputBox.x + 5, (int)themeInputBox.y + 8, 20, BLACK);
                if (themeInputSelected) {
                    DrawRectangleLines(
                        (int)themeInputBox.x,
                        (int)themeInputBox.y,
                        (int)themeInputBox.width,
                        (int)themeInputBox.height,
                        BLUE
                    );
                }

                DrawRectangleRec(llmThemeButton, ORANGE);
                DrawText(
                    "LLM Random Theme",
                    (int)llmThemeButton.x + 5,
                    (int)llmThemeButton.y + 8,
                    20,
                    BLACK
                );

                endVirtualFrame();
                break;
            }

            case GAME_STATE_THEME_READY: {
                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText(
                    "Theme selected!",
                    SCREEN_WIDTH / 2 - MeasureText("Theme selected!", 30) / 2,
                    SCREEN_HEIGHT / 2 - 50,
                    30,
                    BLACK
                );

                if (llmThemeSelected) {
                    DrawText(
                        "LLM will choose a theme...",
                        SCREEN_WIDTH / 2 - MeasureText("LLM will choose a theme...", 25) / 2,
                        SCREEN_HEIGHT / 2,
                        25,
                        DARKGRAY
                    );
                } else {
                    DrawText(
                        theme_input_buffer,
                        SCREEN_WIDTH / 2 - MeasureText(theme_input_buffer, 25) / 2,
                        SCREEN_HEIGHT / 2,
                        25,
                        DARKGRAY
                    );
                }

                DrawText(
                    "Press SPACE to continue...",
                    SCREEN_WIDTH / 2 - MeasureText("Press SPACE to continue...", 20) / 2,
                    SCREEN_HEIGHT / 2 + 50,
                    20,
                    GRAY
                );
                endVirtualFrame();

                if (IsKeyPressed(KEY_SPACE) && !setup_in_progress) {
                    SetupThreadArgs* args = malloc(sizeof(SetupThreadArgs));
                    if (!args) {
                        fprintf(stderr, "Failed to allocate memory for setup thread args.\n");
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_EXIT;
                        pthread_mutex_unlock(&mutex);
                        break;
                    }

                    strncpy(args->theme_input, theme_input_buffer, sizeof(args->theme_input) - 1);
                    args->theme_input[sizeof(args->theme_input) - 1] = '\0';
                    args->llm_selected = llmThemeSelected;

                    pthread_mutex_lock(&mutex);
                    setup_in_progress = true;
                    snprintf(generation_status_message, sizeof(generation_status_message), "Preparing game data...");
                    pthread_mutex_unlock(&mutex);

                    if (pthread_create(&gameSetupThreadId, NULL, gameSetupThread, args) != 0) {
                        fprintf(stderr, "Failed to create game setup thread.\n");
                        free(args);
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_EXIT;
                        setup_in_progress = false;
                        pthread_mutex_unlock(&mutex);
                    } else {
                        setup_thread_started = true;
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_IMAGE_GENERATION;
                        pthread_mutex_unlock(&mutex);
                    }
                }
                break;
            }

            case GAME_STATE_IMAGE_GENERATION: {
                bool current_generating_status;
                bool current_setup_in_progress;
                bool current_confirm_regen_prompt_active_local;

                pthread_mutex_lock(&mutex);
                current_generating_status = generating_images;
                current_setup_in_progress = setup_in_progress;
                current_confirm_regen_prompt_active_local = confirm_regen_prompt_active;
                pthread_mutex_unlock(&mutex);

                if (!current_generating_status &&
                    !current_setup_in_progress &&
                    !current_confirm_regen_prompt_active_local) {
                    if (image_gen_thread_started) {
                        pthread_join(image_gen_master_thread, NULL);
                        image_gen_thread_started = false;
                    }
                    if (setup_thread_started) {
                        pthread_join(gameSetupThreadId, NULL);
                        setup_thread_started = false;
                    }

                    loadPlayerCharacterTexture();
                    loadBoardCharacterTextures();

                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                }

                drawImageGenerationProgressScreen();
                break;
            }

            case GAME_STATE_CONFIRM_REGENERATE_IMAGES: {
                Rectangle yesButton = {SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT / 2 + 30, 80, 30};
                Rectangle noButton = {SCREEN_WIDTH / 2 + 20, SCREEN_HEIGHT / 2 + 30, 80, 30};

                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText(
                    "Theme directory already exists.",
                    SCREEN_WIDTH / 2 - MeasureText("Theme directory already exists.", 25) / 2,
                    SCREEN_HEIGHT / 2 - 50,
                    25,
                    BLACK
                );
                DrawText(
                    "Re-create images for this theme?",
                    SCREEN_WIDTH / 2 - MeasureText("Re-create images for this theme?", 25) / 2,
                    SCREEN_HEIGHT / 2 - 20,
                    25,
                    BLACK
                );

                DrawRectangleRec(yesButton, GREEN);
                DrawText("Yes", (int)yesButton.x + 20, (int)yesButton.y + 5, 20, WHITE);
                DrawRectangleRec(noButton, RED);
                DrawText("No", (int)noButton.x + 20, (int)noButton.y + 5, 20, WHITE);

                endVirtualFrame();

                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    pthread_mutex_lock(&mutex);
                    if (CheckCollisionPointRec(getVirtualMousePosition(), yesButton)) {
                        regen_choice = 1;
                        confirm_regen_prompt_active = false;
                        pthread_cond_signal(&regen_cond);
                        currentGameState = GAME_STATE_IMAGE_GENERATION;
                    } else if (CheckCollisionPointRec(getVirtualMousePosition(), noButton)) {
                        regen_choice = 0;
                        confirm_regen_prompt_active = false;
                        pthread_cond_signal(&regen_cond);
                        currentGameState = GAME_STATE_IMAGE_GENERATION;
                    }
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }

            case GAME_STATE_PLAYING: {
                Rectangle startGuessingButton = {10, 70, 250, 30};
                Rectangle questionInputBox = {10, 44, 580, 34};
                Rectangle submitQuestionButton = {600, 44, 190, 34};
                Rectangle endTurnButton = {600, 44, 190, 34};
                const int boardColumns = 6;
                const int boardRows = NUM_CHARACTERS / boardColumns;
                const float virtualScaleX = getVirtualScaleX();
                const float virtualScaleY = getVirtualScaleY();
                const float boardPadding = 10.0f;
                const float boardOriginX = boardPadding;
                const float boardOriginY =
                    (playerTurnPhase == PLAYER_TURN_PHASE_ELIMINATION) ? 98.0f : 88.0f;
                const float boardWidth = (float)SCREEN_WIDTH - boardPadding * 2.0f;
                const float boardHeight = (float)SCREEN_HEIGHT - boardOriginY - boardPadding;
                const float cellGap = 6.0f;
                const float cellGapOnScreenX = cellGap * virtualScaleX;
                const float cellGapOnScreenY = cellGap * virtualScaleY;
                const float boardWidthOnScreen = boardWidth * virtualScaleX;
                const float boardHeightOnScreen = boardHeight * virtualScaleY;
                const float cellSizeOnScreenX =
                    (boardWidthOnScreen - cellGapOnScreenX * (float)(boardColumns - 1)) /
                    (float)boardColumns;
                const float cellSizeOnScreenY =
                    (boardHeightOnScreen - cellGapOnScreenY * (float)(boardRows - 1)) /
                    (float)boardRows;
                const float cellSizeOnScreen =
                    (cellSizeOnScreenX < cellSizeOnScreenY) ? cellSizeOnScreenX : cellSizeOnScreenY;
                const float cellWidth = cellSizeOnScreen / virtualScaleX;
                const float cellHeight = cellSizeOnScreen / virtualScaleY;
                const float gridWidth =
                    cellWidth * (float)boardColumns + cellGap * (float)(boardColumns - 1);
                const float gridHeight =
                    cellHeight * (float)boardRows + cellGap * (float)(boardRows - 1);
                const float gridOriginX = boardOriginX + (boardWidth - gridWidth) * 0.5f;
                const float gridOriginY = boardOriginY + (boardHeight - gridHeight) * 0.5f;

                if (!guessingRoundStarted) {
                    beginVirtualFrame();
                    ClearBackground(RAYWHITE);

                    DrawRectangleRec(startGuessingButton, BLUE);
                    DrawText(
                        "Start Guessing Round",
                        (int)startGuessingButton.x + 5,
                        (int)startGuessingButton.y + 8,
                        20,
                        WHITE
                    );

                    endVirtualFrame();

                    if (CheckCollisionPointRec(getVirtualMousePosition(), startGuessingButton) &&
                        IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                        llmGuessingRound(
                            llmCharacter,
                            selectedTheme,
                            NUM_CHARACTERS,
                            charactersRemaining,
                            &remainingCount,
                            playerCharacterTexture,
                            playerCharacter,
                            imageDirectoryPath
                        );

                        if (currentGameState == GAME_STATE_PLAYING) {
                            guessingRoundStarted = true;
                            playerTurnActive = true;
                            playerTurnPhase = PLAYER_TURN_PHASE_ASK_QUESTION;
                            playerQuestionInputSelected = false;
                            playerQuestionInput[0] = '\0';
                        }
                    }
                    break;
                }

                if (!playerTurnActive) {
                    llmGuessingRound(
                        llmCharacter,
                        selectedTheme,
                        NUM_CHARACTERS,
                        charactersRemaining,
                        &remainingCount,
                        playerCharacterTexture,
                        playerCharacter,
                        imageDirectoryPath
                    );

                    if (currentGameState == GAME_STATE_PLAYING) {
                        playerTurnActive = true;
                        playerTurnPhase = PLAYER_TURN_PHASE_ASK_QUESTION;
                        playerQuestionInputSelected = false;
                        playerQuestionInput[0] = '\0';
                    }
                    break;
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ASK_QUESTION &&
                    IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    playerQuestionInputSelected =
                        CheckCollisionPointRec(getVirtualMousePosition(), questionInputBox);
                }

                {
                    int key = GetCharPressed();
                    while (key > 0) {
                        if (playerTurnPhase == PLAYER_TURN_PHASE_ASK_QUESTION && playerQuestionInputSelected) {
                            int len = (int)strlen(playerQuestionInput);
                            if (key >= 32 && key <= 125 && len < (int)sizeof(playerQuestionInput) - 1) {
                                playerQuestionInput[len] = (char)key;
                                playerQuestionInput[len + 1] = '\0';
                            }
                        }
                        key = GetCharPressed();
                    }
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ASK_QUESTION &&
                    IsKeyPressed(KEY_BACKSPACE) &&
                    playerQuestionInputSelected) {
                    int len = (int)strlen(playerQuestionInput);
                    if (len > 0) {
                        playerQuestionInput[len - 1] = '\0';
                    }
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ASK_QUESTION &&
                    CheckCollisionPointRec(getVirtualMousePosition(), submitQuestionButton) &&
                    IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    if (strlen(playerQuestionInput) > 0 && !playerQuestionRequestPending) {
                        strncpy(playerLastQuestion, playerQuestionInput, sizeof(playerLastQuestion) - 1);
                        playerLastQuestion[sizeof(playerLastQuestion) - 1] = '\0';
                        snprintf(playerLastAnswer, sizeof(playerLastAnswer), "Waiting for LLM answer...");

                        if (startPlayerQuestionThread(
                                selectedTheme,
                                playerQuestionInput,
                                llmCharacter,
                                imageDirectoryPath
                            )) {
                            playerQuestionRequestPending = true;
                            playerTurnPhase = PLAYER_TURN_PHASE_WAITING_FOR_ANSWER;
                        } else {
                            snprintf(playerLastAnswer, sizeof(playerLastAnswer), "Failed to start request");
                        }
                    }
                }

                if (playerQuestionRequestPending) {
                    bool requestInProgress;
                    bool requestSuccess;
                    char* answerToConsume = NULL;

                    pthread_mutex_lock(&mutex);
                    requestInProgress = player_question_in_progress;
                    requestSuccess = player_question_success;
                    if (!requestInProgress && pending_player_answer) {
                        answerToConsume = pending_player_answer;
                        pending_player_answer = NULL;
                    }
                    pthread_mutex_unlock(&mutex);

                    if (!requestInProgress) {
                        playerQuestionRequestPending = false;
                        if (requestSuccess && answerToConsume) {
                            strncpy(playerLastAnswer, answerToConsume, sizeof(playerLastAnswer) - 1);
                            playerLastAnswer[sizeof(playerLastAnswer) - 1] = '\0';
                            playerTurnPhase = PLAYER_TURN_PHASE_ELIMINATION;
                        } else {
                            snprintf(playerLastAnswer, sizeof(playerLastAnswer), "Could not parse yes/no");
                            playerTurnPhase = PLAYER_TURN_PHASE_ASK_QUESTION;
                        }
                    }

                    if (answerToConsume) {
                        free(answerToConsume);
                    }
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ELIMINATION &&
                    CheckCollisionPointRec(getVirtualMousePosition(), endTurnButton) &&
                    IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    if (playerRemainingCount == 1 && playerCharacterActive[llmCharacter]) {
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_PLAYER_WINS;
                        pthread_mutex_unlock(&mutex);
                    } else {
                        playerTurnActive = false;
                        playerTurnPhase = PLAYER_TURN_PHASE_ASK_QUESTION;
                        playerQuestionInputSelected = false;
                        playerQuestionInput[0] = '\0';
                    }
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ELIMINATION &&
                    IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    Vector2 mousePos = getVirtualMousePosition();
                    for (int i = 0; i < NUM_CHARACTERS; ++i) {
                        int row = i / boardColumns;
                        int col = i % boardColumns;
                        Rectangle cellRect = {
                            boardOriginX + (float)col * (cellWidth + cellGap),
                            boardOriginY + (float)row * (cellHeight + cellGap),
                            cellWidth,
                            cellHeight
                        };

                        if (CheckCollisionPointRec(mousePos, cellRect)) {
                            playerCharacterActive[i] = !playerCharacterActive[i];
                            playerRemainingCount += playerCharacterActive[i] ? 1 : -1;
                            break;
                        }
                    }
                }

                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText("Player Turn", 10, 10, 24, BLACK);

                {
                    char remainingText[64];
                    snprintf(remainingText, sizeof(remainingText), "Player candidates: %d", playerRemainingCount);
                    DrawText(remainingText, 600, 12, 18, MAROON);
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_WAITING_FOR_ANSWER) {
                    const char* waitingText = "Waiting for LLM answer...";
                    int waitingTextWidth = MeasureText(waitingText, 34);
                    DrawText(
                        waitingText,
                        (SCREEN_WIDTH - waitingTextWidth) / 2,
                        SCREEN_HEIGHT / 2 - 18,
                        34,
                        DARKGRAY
                    );
                    if (playerLastQuestion[0] != '\0') {
                        char questionLine[sizeof(playerLastQuestion) + sizeof("Question: ")];
                        snprintf(questionLine, sizeof(questionLine), "Question: %s", playerLastQuestion);
                        int questionLineWidth = MeasureText(questionLine, 20);
                        DrawText(
                            questionLine,
                            (SCREEN_WIDTH - questionLineWidth) / 2,
                            SCREEN_HEIGHT / 2 + 34,
                            20,
                            BLACK
                        );
                    }
                    endVirtualFrame();
                    break;
                }

                if (playerTurnPhase == PLAYER_TURN_PHASE_ASK_QUESTION) {
                    DrawRectangleRec(questionInputBox, LIGHTGRAY);
                    DrawText(
                        (strlen(playerQuestionInput) > 0) ? playerQuestionInput : "Type your yes/no question here...",
                        (int)questionInputBox.x + 8,
                        (int)questionInputBox.y + 8,
                        18,
                        (strlen(playerQuestionInput) > 0) ? BLACK : GRAY
                    );
                    if (playerQuestionInputSelected) {
                        DrawRectangleLines(
                            (int)questionInputBox.x,
                            (int)questionInputBox.y,
                            (int)questionInputBox.width,
                            (int)questionInputBox.height,
                            BLUE
                        );
                    }

                    DrawRectangleRec(submitQuestionButton, BLUE);
                    DrawText("Ask LLM", (int)submitQuestionButton.x + 55, (int)submitQuestionButton.y + 8, 18, WHITE);
                } else if (playerTurnPhase == PLAYER_TURN_PHASE_ELIMINATION) {
                    DrawText("Question:", 10, 44, 18, DARKGRAY);
                    DrawText(playerLastQuestion, 100, 44, 18, BLACK);
                    DrawText("Answer:", 10, 68, 18, DARKGRAY);
                    DrawText(playerLastAnswer, 100, 68, 18, BLACK);
                    DrawRectangleRec(endTurnButton, DARKGREEN);
                    DrawText("End Turn", (int)endTurnButton.x + 48, (int)endTurnButton.y + 8, 18, WHITE);
                }

                for (int i = 0; i < NUM_CHARACTERS; ++i) {
                    int row = i / boardColumns;
                    int col = i % boardColumns;
                    Rectangle cellRect = {
                        gridOriginX + (float)col * (cellWidth + cellGap),
                        gridOriginY + (float)row * (cellHeight + cellGap),
                        cellWidth,
                        cellHeight
                    };
                    const float imagePaddingOnScreen = 4.0f *
                        ((virtualScaleX < virtualScaleY) ? virtualScaleX : virtualScaleY);
                    const float imagePaddingX = imagePaddingOnScreen / virtualScaleX;
                    const float imagePaddingY = imagePaddingOnScreen / virtualScaleY;
                    Rectangle imageRect = {
                        cellRect.x + imagePaddingX,
                        cellRect.y + imagePaddingY,
                        cellRect.width - imagePaddingX * 2.0f,
                        cellRect.height - imagePaddingY * 2.0f
                    };

                    DrawRectangleLines((int)cellRect.x, (int)cellRect.y, (int)cellRect.width, (int)cellRect.height, GRAY);

                    if (boardCharacterTextures[i].id != 0) {
                        float imageWidthOnScreen = imageRect.width * virtualScaleX;
                        float imageHeightOnScreen = imageRect.height * virtualScaleY;
                        float squareSizeOnScreen =
                            (imageWidthOnScreen < imageHeightOnScreen) ?
                                imageWidthOnScreen :
                                imageHeightOnScreen;
                        Rectangle squareRect = {
                            imageRect.x + (imageRect.width - squareSizeOnScreen / virtualScaleX) * 0.5f,
                            imageRect.y + (imageRect.height - squareSizeOnScreen / virtualScaleY) * 0.5f,
                            squareSizeOnScreen / virtualScaleX,
                            squareSizeOnScreen / virtualScaleY
                        };
                        Texture2D texture = boardCharacterTextures[i];
                        float scaleX = squareRect.width / (float)texture.width;
                        float scaleY = squareRect.height / (float)texture.height;
                        float scale = (scaleX < scaleY) ? scaleX : scaleY;
                        float drawWidth = (float)texture.width * scale;
                        float drawHeight = (float)texture.height * scale;
                        Rectangle drawRect = {
                            squareRect.x + (squareRect.width - drawWidth) * 0.5f,
                            squareRect.y + (squareRect.height - drawHeight) * 0.5f,
                            drawWidth,
                            drawHeight
                        };

                        DrawTexturePro(
                            texture,
                            (Rectangle){0, 0, (float)texture.width, (float)texture.height},
                            drawRect,
                            (Vector2){0},
                            0.0f,
                            WHITE
                        );
                    } else {
                        DrawRectangleRec(imageRect, LIGHTGRAY);
                        DrawText("No image", (int)imageRect.x + 8, (int)imageRect.y + 20, 14, DARKGRAY);
                    }

                    if (!playerCharacterActive[i]) {
                        DrawRectangleRec(cellRect, (Color){25, 25, 25, 180});
                        DrawLine(
                            (int)cellRect.x + 6,
                            (int)cellRect.y + 6,
                            (int)(cellRect.x + cellRect.width - 6),
                            (int)(cellRect.y + cellRect.height - 6),
                            RED
                        );
                        DrawLine(
                            (int)(cellRect.x + cellRect.width - 6),
                            (int)cellRect.y + 6,
                            (int)cellRect.x + 6,
                            (int)(cellRect.y + cellRect.height - 6),
                            RED
                        );
                    }
                }

                endVirtualFrame();
                break;
            }

            case GAME_STATE_PLAYER_WINS: {
                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText(
                    "YOU WIN!",
                    SCREEN_WIDTH / 2 - MeasureText("YOU WIN!", 40) / 2,
                    SCREEN_HEIGHT / 2 - 20,
                    40,
                    GREEN
                );
                DrawText(
                    "You found the hidden character.",
                    SCREEN_WIDTH / 2 - MeasureText("You found the hidden character.", 20) / 2,
                    SCREEN_HEIGHT / 2 + 20,
                    20,
                    DARKGRAY
                );
                DrawText(
                    "Press ESC to exit",
                    SCREEN_WIDTH / 2 - MeasureText("Press ESC to exit", 20) / 2,
                    SCREEN_HEIGHT / 2 + 60,
                    20,
                    GRAY
                );

                endVirtualFrame();

                if (IsKeyPressed(KEY_ESCAPE)) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_EXIT;
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }

            case GAME_STATE_LLM_WINS: {
                beginVirtualFrame();
                ClearBackground(RAYWHITE);

                DrawText(
                    "LLM WINS!",
                    SCREEN_WIDTH / 2 - MeasureText("LLM WINS!", 40) / 2,
                    SCREEN_HEIGHT / 2 - 20,
                    40,
                    RED
                );
                DrawText(
                    "The LLM guessed your character!",
                    SCREEN_WIDTH / 2 - MeasureText("The LLM guessed your character!", 20) / 2,
                    SCREEN_HEIGHT / 2 + 20,
                    20,
                    DARKGRAY
                );
                DrawText(
                    "Press ESC to exit",
                    SCREEN_WIDTH / 2 - MeasureText("Press ESC to exit", 20) / 2,
                    SCREEN_HEIGHT / 2 + 60,
                    20,
                    GRAY
                );

                endVirtualFrame();

                if (IsKeyPressed(KEY_ESCAPE)) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_EXIT;
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }

            case GAME_STATE_EXIT:
                break;
        }
    }

    freeGameResources();
    CloseWindow();
    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&regen_cond);
    curl_global_cleanup();

    return 0;
}
