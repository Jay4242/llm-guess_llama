#ifndef GUESS_LLAMA_H
#define GUESS_LLAMA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include <curl/curl.h>
#include <jansson.h>
#include <raylib.h>

#include <pthread.h>
#include <unistd.h>

#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>

#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define NUM_CHARACTERS 24
#define MAX_PATH_BUFFER_SIZE 2048
#define MAX_FILEPATH_BUFFER_SIZE (MAX_PATH_BUFFER_SIZE + 256)

typedef enum {
    GAME_STATE_THEME_SELECTION,
    GAME_STATE_THEME_READY,
    GAME_STATE_IMAGE_GENERATION,
    GAME_STATE_CONFIRM_REGENERATE_IMAGES,
    GAME_STATE_PLAYING,
    GAME_STATE_PLAYER_WINS,
    GAME_STATE_LLM_WINS,
    GAME_STATE_EXIT
} GameState;

typedef struct {
    char theme_input[100];
    bool llm_selected;
} SetupThreadArgs;

typedef struct {
    char* prompt;
    int character_number;
} SingleImageGenData;

typedef struct {
    SingleImageGenData* images_data;
    int num_images;
    const char* image_dir;
} BatchImageGenData;

extern GameState currentGameState;

extern char currentQuestion[256];
extern int currentAnswer;
extern char characterSelectionText[256];
extern char playerCharacterString[256];
extern char current_percent[16];
extern char gameOverReasonText[128];

extern pthread_mutex_t mutex;
extern pthread_cond_t regen_cond;

extern bool generating_images;
extern int total_characters_to_generate;
extern int characters_generated_count;
extern int current_image_index;
extern char generation_status_message[256];
extern pthread_t image_gen_master_thread;
extern bool image_gen_thread_started;

extern pthread_t gameSetupThreadId;
extern bool setup_thread_started;
extern bool setup_in_progress;

extern pthread_t llm_guess_thread;
extern bool llm_guess_thread_started;
extern bool llm_guess_in_progress;
extern char* pending_llm_question;
extern char** pending_elimination_list;
extern int pending_elimination_count;
extern bool llm_guess_success;
extern bool llm_should_continue;

extern pthread_t player_question_thread;
extern bool player_question_thread_started;
extern bool player_question_in_progress;
extern bool player_question_success;
extern char* pending_player_answer;

extern char formattedThemeName[256];
extern char imageDirectoryPath[MAX_PATH_BUFFER_SIZE];
extern bool confirm_regen_prompt_active;
extern int regen_choice;

extern char* selectedTheme;
extern char** characterFeatures;
extern char*** characterTraits;
extern int featureCount;
extern int playerCharacter;
extern int llmCharacter;
extern int* charactersRemaining;
extern int remainingCount;
extern bool playerCharacterActive[NUM_CHARACTERS];
extern int playerRemainingCount;

extern Texture2D playerCharacterTexture;
extern Texture2D boardCharacterTextures[NUM_CHARACTERS];
extern bool boardCharacterTexturesLoaded;

extern RenderTexture2D virtualRenderTarget;

extern const char* server_url;
extern const char* serverModel;
extern const char* serverApiKey;
extern const char* llmServerAddress;
extern const char* llmApiKey;
extern const char* llmModel;
void initRuntimeConfig(void);

char* filter_think_tags(const char* input_str);
char* extract_json_from_markdown(const char* input_str);
char* getLLMResponse(const char* prompt, double temperature);
char* getLLMResponseWithVision(
    const char* initial_prompt,
    const char** image_paths,
    int image_count,
    const int* image_labels,
    const char* final_prompt,
    double temperature
);
void clear_llm_vision_image_cache(void);
bool cache_llm_vision_image_from_loaded_image(const char* filepath, const Image* loadedImage);
char** getThemesFromLLM(int* themeCount);
char** getCharacterFeatures(const char* theme, int* featureCount);

char* format_theme_name(const char* theme);
bool directory_exists(const char* path);
int create_directory_recursive(const char* path);
int delete_files_in_directory(const char* path);
bool save_game_data(
    const char* filename,
    const char* theme,
    char** features,
    int featureCount,
    char*** characterTraits,
    int numCharacters
);
bool load_game_data(
    const char* filename,
    char** loadedTheme,
    char*** loadedFeatures,
    int* loadedFeatureCount,
    char**** loadedCharacterTraits
);

int generate_character_image(const char* prompt, int character_number, const char* image_dir);
int generate_character_image_openrouter(const char* prompt, int character_number, const char* image_dir);
bool is_openrouter_server_url(const char* url);
void* generateImageThread(void* arg);

typedef struct {
    int llmCharacter;
    char* theme;
    int numCharacters;
    int* charactersRemaining;
    int* remainingCount;
    Texture2D playerTexture;
    int playerCharacter;
    char* imageDirectoryPath;
} LLMGuessThreadArgs;

void llmGuessingRound(
    int llmCharacter,
    const char* theme,
    int numCharacters,
    int* charactersRemaining,
    int* remainingCount,
    Texture2D playerTexture,
    int playerCharacter,
    const char* imageDirectoryPath
);
void* gameSetupThread(void* arg);
void* llmGuessThread(void* arg);

extern pthread_t llm_guess_thread;
extern bool llm_guess_thread_started;
extern bool llm_guess_in_progress;
extern char* pending_llm_question;
extern char** pending_elimination_list;
extern int pending_elimination_count;
extern bool llm_guess_success;
extern bool llm_should_continue;

bool loadPlayerCharacterTexture(void);
bool loadBoardCharacterTextures(void);
bool initVirtualRendering(void);
void refreshVirtualViewport(void);
void beginVirtualFrame(void);
void endVirtualFrame(void);
Vector2 getVirtualMousePosition(void);
float getVirtualScaleX(void);
float getVirtualScaleY(void);
void freeGameResources(void);

char* getPlayerQuestionYesNoAnswer(
    const char* theme,
    const char* question,
    int llmCharacterIndex,
    const char* imageDirectory
);
bool startPlayerQuestionThread(
    const char* theme,
    const char* question,
    int llmCharacterIndex,
    const char* imageDirectory
);

#endif
