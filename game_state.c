#include "guess_llama.h"

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

Texture2D playerCharacterTexture = {0};

const char* username = "username";
const char* server_url = "localhost:1234";
const char* llmServerAddress = "http://localhost:9090";

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

void freeGameResources(void) {
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
}
