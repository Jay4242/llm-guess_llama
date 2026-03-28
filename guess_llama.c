#include "guess_llama.h"

static void drawImageGenerationProgressScreen(void) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    pthread_mutex_lock(&mutex);
    DrawText(generation_status_message, 10, 10, 20, BLACK);
    DrawText(current_percent, 10, 40, 20, BLACK);
    pthread_mutex_unlock(&mutex);
    EndDrawing();
}

int main(void) {
    char theme_input_buffer[100] = {0};
    bool themeInputSelected = false;
    Rectangle themeInputBox = {100, 100, 200, 30};
    Rectangle llmThemeButton = {320, 100, 200, 30};
    bool llmThemeSelected = false;

    srand((unsigned int)time(NULL));

    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Guess Llama");
    SetTargetFPS(60);

    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed.\n");
        CloseWindow();
        return 1;
    }

    if (pthread_cond_init(&regen_cond, NULL) != 0) {
        fprintf(stderr, "Condition variable initialization failed.\n");
        pthread_mutex_destroy(&mutex);
        CloseWindow();
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    while (!WindowShouldClose() && currentGameState != GAME_STATE_EXIT) {
        GameState state;
        bool current_confirm_regen_prompt_active;

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
                    themeInputSelected = CheckCollisionPointRec(GetMousePosition(), themeInputBox);
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

                if (CheckCollisionPointRec(GetMousePosition(), llmThemeButton) &&
                    IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    llmThemeSelected = true;
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_THEME_READY;
                    pthread_mutex_unlock(&mutex);
                }

                BeginDrawing();
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

                EndDrawing();
                break;
            }

            case GAME_STATE_THEME_READY: {
                BeginDrawing();
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
                EndDrawing();

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

                BeginDrawing();
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

                EndDrawing();

                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    pthread_mutex_lock(&mutex);
                    if (CheckCollisionPointRec(GetMousePosition(), yesButton)) {
                        regen_choice = 1;
                        confirm_regen_prompt_active = false;
                        pthread_cond_signal(&regen_cond);
                        currentGameState = GAME_STATE_IMAGE_GENERATION;
                    } else if (CheckCollisionPointRec(GetMousePosition(), noButton)) {
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

                BeginDrawing();
                ClearBackground(RAYWHITE);

                DrawText(playerCharacterString, 10, 10, 20, BLACK);
                DrawRectangleRec(startGuessingButton, BLUE);
                DrawText(
                    "Start Guessing Round",
                    (int)startGuessingButton.x + 5,
                    (int)startGuessingButton.y + 8,
                    20,
                    WHITE
                );

                if (CheckCollisionPointRec(GetMousePosition(), startGuessingButton) &&
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
                }

                EndDrawing();
                break;
            }

            case GAME_STATE_PLAYER_WINS: {
                BeginDrawing();
                ClearBackground(RAYWHITE);

                DrawText(
                    "YOU WIN!",
                    SCREEN_WIDTH / 2 - MeasureText("YOU WIN!", 40) / 2,
                    SCREEN_HEIGHT / 2 - 20,
                    40,
                    GREEN
                );
                DrawText(
                    "The LLM eliminated your character!",
                    SCREEN_WIDTH / 2 - MeasureText("The LLM eliminated your character!", 20) / 2,
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

                EndDrawing();

                if (IsKeyPressed(KEY_ESCAPE)) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_EXIT;
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }

            case GAME_STATE_LLM_WINS: {
                BeginDrawing();
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

                EndDrawing();

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
