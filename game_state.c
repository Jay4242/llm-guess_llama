#include "guess_llama.h"
#include <rlgl.h>

GameState currentGameState = GAME_STATE_THEME_SELECTION;

char currentQuestion[256] = {0};
int currentAnswer = -1;
char characterSelectionText[256] = {0};
char playerCharacterString[256] = {0};
char current_percent[16] = {0};

pthread_mutex_t mutex;
pthread_cond_t regen_cond;

bool generating_images = false;
int total_characters_to_generate = 0;
int characters_generated_count = 0;
int current_image_index = 0;
char generation_status_message[256] = {0};
pthread_t image_gen_master_thread = (pthread_t)0;
bool image_gen_thread_started = false;

pthread_t gameSetupThreadId = (pthread_t)0;
bool setup_thread_started = false;
bool setup_in_progress = false;

pthread_t llm_guess_thread = (pthread_t)0;
bool llm_guess_thread_started = false;
bool llm_guess_in_progress = false;
char* pending_llm_question = NULL;
char** pending_elimination_list = NULL;
int pending_elimination_count = 0;
bool llm_guess_success = false;
bool llm_should_continue = true;

pthread_t player_question_thread = (pthread_t)0;
bool player_question_thread_started = false;
bool player_question_in_progress = false;
bool player_question_success = false;
char* pending_player_answer = NULL;

char formattedThemeName[256] = {0};
char imageDirectoryPath[MAX_PATH_BUFFER_SIZE] = {0};
bool confirm_regen_prompt_active = false;
int regen_choice = -1;

char* selectedTheme = NULL;
char** characterFeatures = NULL;
char*** characterTraits = NULL;
int featureCount = 0;
int playerCharacter = -1;
int llmCharacter = -1;
int* charactersRemaining = NULL;
int remainingCount = 0;
bool playerCharacterActive[NUM_CHARACTERS] = {0};
int playerRemainingCount = 0;

Texture2D playerCharacterTexture = {0};
Texture2D boardCharacterTextures[NUM_CHARACTERS] = {0};
bool boardCharacterTexturesLoaded = false;
RenderTexture2D virtualRenderTarget = {0};

static float virtualScaleX = 1.0f;
static float virtualScaleY = 1.0f;
static float virtualViewportWidth = (float)SCREEN_WIDTH;
static float virtualViewportHeight = (float)SCREEN_HEIGHT;

static bool ensureVirtualRenderTargetSize(void) {
    int targetWidth = GetScreenWidth();
    int targetHeight = GetScreenHeight();
    RenderTexture2D newRenderTarget = {0};

    if (targetWidth <= 0) {
        targetWidth = SCREEN_WIDTH;
    }
    if (targetHeight <= 0) {
        targetHeight = SCREEN_HEIGHT;
    }

    if (virtualRenderTarget.id != 0 &&
        virtualRenderTarget.texture.width == targetWidth &&
        virtualRenderTarget.texture.height == targetHeight) {
        return true;
    }

    newRenderTarget = LoadRenderTexture(targetWidth, targetHeight);
    if (newRenderTarget.id == 0) {
        fprintf(stderr, "Failed to create virtual render target (%dx%d).\n", targetWidth, targetHeight);
        return false;
    }

    if (virtualRenderTarget.id != 0) {
        UnloadRenderTexture(virtualRenderTarget);
    }

    virtualRenderTarget = newRenderTarget;

    SetTextureFilter(virtualRenderTarget.texture, TEXTURE_FILTER_BILINEAR);
    return true;
}

const char* username = "username";
const char* server_url = "localhost:1234";
const char* llmServerAddress = "http://localhost:9090";
const char* llmApiKey = "";
const char* llmModel = "qwen3.5";

static const char* getEnvOrDefault(const char* name, const char* fallback) {
    const char* value = getenv(name);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return value;
}

static char* trimWhitespace(char* text) {
    char* end;

    if (text == NULL) {
        return NULL;
    }

    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }

    if (*text == '\0') {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        --end;
    }

    return text;
}

static bool isValidEnvKey(const char* key) {
    if (key == NULL || key[0] == '\0') {
        return false;
    }

    if (!(isalpha((unsigned char)key[0]) || key[0] == '_')) {
        return false;
    }

    for (const char* cursor = key + 1; *cursor != '\0'; ++cursor) {
        if (!(isalnum((unsigned char)*cursor) || *cursor == '_')) {
            return false;
        }
    }

    return true;
}

static void stripOuterQuotes(char* value) {
    size_t length;

    if (value == NULL) {
        return;
    }

    length = strlen(value);
    if (length < 2) {
        return;
    }

    if ((value[0] == '"' && value[length - 1] == '"') || (value[0] == '\'' && value[length - 1] == '\'')) {
        memmove(value, value + 1, length - 2);
        value[length - 2] = '\0';
    }
}

static int setEnvIfAllowed(const char* key, const char* value, int overwrite) {
#ifdef _WIN32
    if (!overwrite && getenv(key) != NULL) {
        return 0;
    }
    return _putenv_s(key, value);
#else
    return setenv(key, value, overwrite);
#endif
}

static void loadDotEnvFile(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    char line[4096];
    int lineNumber = 0;

    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char* parsedLine;
        char* equals;
        char* key;
        char* value;
        size_t lineLength;

        ++lineNumber;
        lineLength = strlen(line);

        if (lineLength == sizeof(line) - 1 && line[lineLength - 1] != '\n') {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {
            }
        }

        while (lineLength > 0 && (line[lineLength - 1] == '\n' || line[lineLength - 1] == '\r')) {
            line[--lineLength] = '\0';
        }

        parsedLine = trimWhitespace(line);
        if (parsedLine[0] == '\0' || parsedLine[0] == '#') {
            continue;
        }

        if (strncmp(parsedLine, "export", 6) == 0 && isspace((unsigned char)parsedLine[6])) {
            parsedLine = trimWhitespace(parsedLine + 6);
        }

        equals = strchr(parsedLine, '=');
        if (equals == NULL) {
            fprintf(stderr, "Skipping invalid .env line %d in %s (missing '=')\n", lineNumber, filepath);
            continue;
        }

        *equals = '\0';
        key = trimWhitespace(parsedLine);
        value = trimWhitespace(equals + 1);

        if (!isValidEnvKey(key)) {
            fprintf(stderr, "Skipping invalid .env key '%s' in %s line %d\n", key, filepath, lineNumber);
            continue;
        }

        stripOuterQuotes(value);

        if (setEnvIfAllowed(key, value, 0) != 0) {
            fprintf(stderr, "Failed to set environment variable '%s' from %s: %s\n", key, filepath, strerror(errno));
        }
    }

    fclose(file);
}

void initRuntimeConfig(void) {
    loadDotEnvFile(".env.local");
    loadDotEnvFile(".env");

    username = getEnvOrDefault("GUESS_LLAMA_USERNAME", "username");
    server_url = getEnvOrDefault("GUESS_LLAMA_SERVER_URL", "localhost:1234");
    llmServerAddress = getEnvOrDefault("GUESS_LLAMA_LLM_SERVER", "http://localhost:9090");
    llmApiKey = getEnvOrDefault("GUESS_LLAMA_LLM_API_KEY", "");
    llmModel = getEnvOrDefault("GUESS_LLAMA_LLM_MODEL", "qwen3.5");
}

static void updateVirtualViewport(void) {
    virtualViewportWidth = (float)GetScreenWidth();
    virtualViewportHeight = (float)GetScreenHeight();

    if (virtualViewportWidth <= 0.0f) {
        virtualViewportWidth = (float)SCREEN_WIDTH;
    }
    if (virtualViewportHeight <= 0.0f) {
        virtualViewportHeight = (float)SCREEN_HEIGHT;
    }

    virtualScaleX = virtualViewportWidth / (float)SCREEN_WIDTH;
    virtualScaleY = virtualViewportHeight / (float)SCREEN_HEIGHT;

    if (virtualScaleX <= 0.0f) {
        virtualScaleX = 1.0f;
    }
    if (virtualScaleY <= 0.0f) {
        virtualScaleY = 1.0f;
    }

}

void refreshVirtualViewport(void) {
    updateVirtualViewport();
}

bool initVirtualRendering(void) {
    if (!ensureVirtualRenderTargetSize()) {
        return false;
    }

    updateVirtualViewport();
    return true;
}

void beginVirtualFrame(void) {
    updateVirtualViewport();
    if (virtualRenderTarget.id == 0 && !ensureVirtualRenderTargetSize()) {
        return;
    }

    ensureVirtualRenderTargetSize();

    BeginTextureMode(virtualRenderTarget);
    rlPushMatrix();
    rlScalef(virtualScaleX, virtualScaleY, 1.0f);
}

void endVirtualFrame(void) {
    if (virtualRenderTarget.id == 0) {
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        return;
    }

    Rectangle src = {
        0.0f,
        0.0f,
        (float)virtualRenderTarget.texture.width,
        -(float)virtualRenderTarget.texture.height
    };
    Rectangle dst = {0.0f, 0.0f, virtualViewportWidth, virtualViewportHeight};

    rlPopMatrix();
    EndTextureMode();
    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(virtualRenderTarget.texture, src, dst, (Vector2){0.0f, 0.0f}, 0.0f, WHITE);
    EndDrawing();
}

Vector2 getVirtualMousePosition(void) {
    updateVirtualViewport();

    Vector2 mousePos = GetMousePosition();
    Vector2 virtualMousePos = {-1000.0f, -1000.0f};

    if (virtualScaleX <= 0.0f || virtualScaleY <= 0.0f) {
        return virtualMousePos;
    }

    virtualMousePos.x = mousePos.x / virtualScaleX;
    virtualMousePos.y = mousePos.y / virtualScaleY;
    return virtualMousePos;
}

float getVirtualScaleX(void) {
    return virtualScaleX;
}

float getVirtualScaleY(void) {
    return virtualScaleY;
}

bool loadPlayerCharacterTexture(void) {
    char playerImagePath[MAX_FILEPATH_BUFFER_SIZE];
    Image playerImage;

    snprintf(
        playerImagePath,
        sizeof(playerImagePath),
        "%s/character_%d.png",
        imageDirectoryPath,
        playerCharacter + 1
    );

    playerImage = LoadImage(playerImagePath);
    if (playerImage.data == NULL) {
        fprintf(stderr, "Failed to load player character image: %s\n", playerImagePath);
        return false;
    }

    if (playerCharacterTexture.id != 0) {
        UnloadTexture(playerCharacterTexture);
    }

    playerCharacterTexture = LoadTextureFromImage(playerImage);
    UnloadImage(playerImage);

    printf("Player character image loaded: %s\n", playerImagePath);
    return true;
}

bool loadBoardCharacterTextures(void) {
    bool allLoaded = true;

    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        char imagePath[MAX_FILEPATH_BUFFER_SIZE];
        Image characterImage;

        snprintf(
            imagePath,
            sizeof(imagePath),
            "%s/character_%d.png",
            imageDirectoryPath,
            i + 1
        );

        characterImage = LoadImage(imagePath);
        if (characterImage.data == NULL) {
            fprintf(stderr, "Failed to load board character image: %s\n", imagePath);
            allLoaded = false;
            continue;
        }

        if (boardCharacterTextures[i].id != 0) {
            UnloadTexture(boardCharacterTextures[i]);
            boardCharacterTextures[i].id = 0;
        }

        boardCharacterTextures[i] = LoadTextureFromImage(characterImage);
        UnloadImage(characterImage);
    }

    boardCharacterTexturesLoaded = allLoaded;
    return allLoaded;
}

void freeGameResources(void) {
    if (player_question_thread_started) {
        pthread_join(player_question_thread, NULL);
        player_question_thread_started = false;
    }

    if (selectedTheme) {
        free(selectedTheme);
        selectedTheme = NULL;
    }

    if (characterFeatures) {
        for (int i = 0; i < featureCount; ++i) {
            free(characterFeatures[i]);
        }
        free(characterFeatures);
        characterFeatures = NULL;
    }

    if (characterTraits) {
        for (int i = 0; i < NUM_CHARACTERS; ++i) {
            if (!characterTraits[i]) {
                continue;
            }
            for (int j = 0; j < 2; ++j) {
                free(characterTraits[i][j]);
            }
            free(characterTraits[i]);
        }
        free(characterTraits);
        characterTraits = NULL;
    }

    if (charactersRemaining) {
        free(charactersRemaining);
        charactersRemaining = NULL;
    }

    if (playerCharacterTexture.id != 0) {
        UnloadTexture(playerCharacterTexture);
        playerCharacterTexture.id = 0;
    }

    if (virtualRenderTarget.id != 0) {
        UnloadRenderTexture(virtualRenderTarget);
        virtualRenderTarget.id = 0;
    }

    for (int i = 0; i < NUM_CHARACTERS; ++i) {
        if (boardCharacterTextures[i].id != 0) {
            UnloadTexture(boardCharacterTextures[i]);
            boardCharacterTextures[i].id = 0;
        }
        playerCharacterActive[i] = false;
    }
    boardCharacterTexturesLoaded = false;
    playerRemainingCount = 0;

    if (pending_llm_question) {
        free(pending_llm_question);
        pending_llm_question = NULL;
    }

    if (pending_elimination_list) {
        for (int i = 0; i < pending_elimination_count; i++) {
            free(pending_elimination_list[i]);
        }
        free(pending_elimination_list);
        pending_elimination_list = NULL;
        pending_elimination_count = 0;
    }

    if (pending_player_answer) {
        free(pending_player_answer);
        pending_player_answer = NULL;
    }
    player_question_in_progress = false;
    player_question_success = false;
}
