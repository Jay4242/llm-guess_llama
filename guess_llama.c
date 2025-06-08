#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <pthread.h>
#include <raylib.h>
#include <time.h>
#include <unistd.h>
#include <jansson.h>
#include <sys/stat.h> // For stat, mkdir
#include <dirent.h>   // For opendir, readdir, closedir
#include <errno.h>    // For errno
#include <ctype.h>    // For tolower, isalnum

// Define screen dimensions
#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600
#define NUM_CHARACTERS 24 // Define numCharacters as a constant
#define MAX_PATH_BUFFER_SIZE 2048 // Max length for directory paths (e.g., imageDirectoryPath)
// Max length for full file paths, considering MAX_PATH_BUFFER_SIZE + max filename length (e.g., 255 for NAME_MAX) + '/' + null terminator
#define MAX_FILEPATH_BUFFER_SIZE (MAX_PATH_BUFFER_SIZE + 256) 

// Game States
typedef enum {
    GAME_STATE_THEME_SELECTION,
    GAME_STATE_THEME_READY, // New state for "Press SPACE to continue"
    GAME_STATE_IMAGE_GENERATION,
    GAME_STATE_CONFIRM_REGENERATE_IMAGES, // New state for prompting image re-creation
    GAME_STATE_PLAYING,
    GAME_STATE_PLAYER_WINS,
    GAME_STATE_LLM_WINS, // Placeholder for future LLM win condition
    GAME_STATE_EXIT
} GameState;

// Global game state variable
GameState currentGameState = GAME_STATE_THEME_SELECTION;

// Global variables for yes/no question
char currentQuestion[256] = {0};
int currentAnswer = -1; // -1: no answer, 0: no, 1: yes

// Character selection display string
char characterSelectionText[256] = {0};
char playerCharacterString[256] = {0};

char current_percent[8] = { 0 };

pthread_mutex_t mutex;

// Global variables for image generation status
bool generating_images = false;
int total_characters_to_generate = 0;
int characters_generated_count = 0;
char generation_status_message[256] = {0};
pthread_t image_gen_master_thread = (pthread_t)0; // Declare the global thread variable here, initialized to 0

// New global variables for setup thread
pthread_t gameSetupThreadId;
bool setup_in_progress = false;

// New global variables for image directory and regeneration prompt
char formattedThemeName[256] = {0};
char imageDirectoryPath[MAX_PATH_BUFFER_SIZE] = {0}; // Increased size for full path
bool confirm_regen_prompt_active = false;
int regen_choice = -1; // -1: no choice, 0: no, 1: yes
pthread_cond_t regen_cond; // Condition variable for waiting on user input

// Global variables for game data (populated by gameSetupThread)
char* selectedTheme = NULL;
char** characterFeatures = NULL;
char*** characterTraits = NULL;
int featureCount = 0;
int playerCharacter = -1;
int llmCharacter = -1;
int* charactersRemaining = NULL;
int remainingCount = 0;

// Function prototypes
void DrawImageGenerationProgressScreen(void);

// Global variable for player's character texture
Texture2D playerCharacterTexture = { 0 };

// Configuration
const char* username = "USERNAME";                             //Add username Here.
const char* server_url = "EASY_DIFFUSION_SERVER_ADDRESS:PORT";         //Add Easy Diffusion Server:Port here.

// Function to strip the port from the server URL
char* strip_port(const char* url) {
    char* stripped_url = strdup(url);
    char* colon = strchr(stripped_url, ':');
    if (colon != NULL) {
        *colon = '\0';
    }
    return stripped_url;
}

// Define the LLM server address
const char* llmServerAddress = "http://LLM_SERVER_ADDRESS:PORT";

// Struct to hold the response data
typedef struct {
    char* data;
    size_t size;
} ResponseData;

// Callback function to write the response data
size_t write_callback(char* ptr, size_t size, size_t nmemb, ResponseData* data) {
    size_t new_size = data->size + size * nmemb;
    data->data = realloc(data->data, new_size + 1);
    if (data->data == NULL) {
        fprintf(stderr, "realloc() failed\n");
        return 0;
    }
    memcpy(data->data + data->size, ptr, size * nmemb);
    data->data[new_size] = '\0';
    data->size = new_size;
    return size * nmemb;
}

// Base64 decoding table
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Function to generate a random seed
unsigned int get_random_seed() {
    return rand();
}

// Function to decode base64 data
unsigned char* base64_decode(const char* data, size_t data_len, size_t* output_len) {
    size_t i, j;

    if (data_len == 0) {
        *output_len = 0;
        return NULL;
    }

    // Calculate the length of the decoded data
    size_t padding = 0;
    if (data_len > 0 && data[data_len - 1] == '=') padding++;
    if (data_len > 1 && data[data_len - 2] == '=') padding++;
    *output_len = (data_len * 3) / 4 - padding;

    unsigned char* decoded_data = (unsigned char*)malloc(*output_len);
    if (decoded_data == NULL) {
        fprintf(stderr, "malloc() failed\n");
        return NULL;
    }

    // Decode the base64 data
    int val = 0, valb = -8;
    for (i = 0, j = 0; i < data_len; i++) {
        unsigned char c = data[i];
        if (c == '=')
            break;

        const char* p = strchr(b64_table, c);
        if (p == NULL)
            continue;

        int index = p - b64_table;
        val = (val << 6) | index;
        valb += 6;

        if (valb >= 0) {
            decoded_data[j++] = (unsigned char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }

    return decoded_data;
}

// Function to make an HTTP GET request using libcurl
char* make_http_get(const char* url) {
    CURL* curl;
    CURLcode res;
    ResponseData response_data = {NULL, 0};

    curl = curl_easy_init();
    if (curl) {
        //printf("GET URL: %s\n", url);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); // Timeout after 60 seconds

        // Disable SSL verification (equivalent of --insecure)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            return NULL;
        }

        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "curl_easy_init() failed\n");
    }

    return response_data.data;
}

// Function to make an HTTP POST request using libcurl
char* make_http_post(const char* url, const char* data) {
    CURL* curl;
    CURLcode res;
    ResponseData response_data = {NULL, 0};
    struct curl_slist *headers = NULL;

    curl = curl_easy_init();
    if (curl) {
        //printf("POST URL: %s\n", url);
        //printf("POST Data: %s\n", data);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); // Timeout after 60 seconds

        // Add headers
        headers = curl_slist_append(headers, "Accept: */*");
        headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
        headers = curl_slist_append(headers, "Cache-Control: no-cache");
        headers = curl_slist_append(headers, "Connection: keep-alive");
        headers = curl_slist_append(headers, "Content-Type: application/json");
        //char origin[256];
        //snprintf(origin, sizeof(origin), "Origin: http://%s", server_url);
        //headers = curl_slist_append(headers, origin);
        headers = curl_slist_append(headers, "Pragma: no-cache");
        //char referer[256];
        //snprintf(referer, sizeof(referer), "Referer: http://%s/", server_url);
        //headers = curl_slist_append(headers, referer);
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36");

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Disable SSL verification (equivalent of --insecure)
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            if (headers) curl_slist_free_all(headers);
            return NULL;
        }

        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "curl_easy_init() failed\n");
    }
    if (headers) curl_slist_free_all(headers);

    return response_data.data;
}

// Function to make a request to the LLM API
char* getLLMResponse(const char* prompt, double temperature) {
    CURL* curl;
    CURLcode res;
    ResponseData response = {NULL, 0};

    curl = curl_easy_init();
    if (curl) {
        // Construct the URL
        char* url;
        if (asprintf(&url, "%s/v1/chat/completions", llmServerAddress) == -1) {
            fprintf(stderr, "Failed to construct URL\n");
            curl_easy_cleanup(curl);
            return NULL;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);

        // Prepare the JSON payload
        char* data;
        if (asprintf(&data, "{\"model\": \"llama-3.2-3b-it-q8_0\", \"messages\": [{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}], \"temperature\": %f}", prompt, temperature) == -1) {
            fprintf(stderr, "Failed to construct JSON payload\n");
            curl_easy_cleanup(curl);
            free(url);
            return NULL;
        }

        // Set the request type to POST
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        // Set the POST data
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data); // Use data directly, curl will handle length

        // Set the callback function to write the response
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback); // Use write_callback
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // Set the content type to application/json
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Perform the request
        res = curl_easy_perform(curl);

        // Check for errors
        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            free(url);
            free(data); // Free data here
            return NULL;
        }

        // Always cleanup
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(url);
        free(data); // Free data here
    } else {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    return response.data;
}

// Helper function to parse a single JSON object from a string and extract progress
void parse_progress_json(const char* json_string) {
    json_error_t stream_error;
    json_t* stream_data = json_loads(json_string, 0, &stream_error);
    if (!stream_data) {
        // This can happen if the stream contains non-JSON data or incomplete JSON.
        // It's often just noise in the stream, so we print a debug message and continue.
        // fprintf(stderr, "Error parsing stream JSON object: %s (Content: %s)\n", stream_error.text, json_string);
        return;
    }

    json_t* steps_json = json_object_get(stream_data, "step");
    json_t* total_steps_json = json_object_get(stream_data, "total_steps");

    if (steps_json && total_steps_json && json_is_number(steps_json) && json_is_number(total_steps_json)) {
        int steps = json_integer_value(steps_json);
        int total_steps = json_integer_value(total_steps_json);
        if (total_steps > 0) {
            float percentage = (float)steps / total_steps * 100;
            pthread_mutex_lock(&mutex);
            snprintf(current_percent, sizeof(current_percent), "%.0f%%", percentage);
            pthread_mutex_unlock(&mutex);
        }
    }
    json_decref(stream_data);
}

// Function to parse a stream of concatenated JSON objects
void process_json_stream_response(const char* stream_response_raw) {
    if (!stream_response_raw) return;

    const char* current_ptr = stream_response_raw;
    const char* end_of_response = stream_response_raw + strlen(stream_response_raw);

    while (current_ptr < end_of_response) {
        // Find the start of the next JSON object
        const char* start = strchr(current_ptr, '{');
        if (!start) break; // No more JSON objects

        int brace_count = 0;
        const char* end = start;
        bool in_string = false;

        // Iterate to find the matching closing brace
        while (end < end_of_response) {
            if (*end == '"' && (end == start || *(end - 1) != '\\')) { // Handle escaped quotes
                in_string = !in_string;
            } else if (!in_string) {
                if (*end == '{') {
                    brace_count++;
                } else if (*end == '}') {
                    brace_count--;
                }
            }

            if (brace_count == 0 && *end == '}') {
                // Found a complete JSON object
                size_t obj_len = (end - start) + 1;
                char* json_obj_str = (char*)malloc(obj_len + 1);
                if (!json_obj_str) {
                    fprintf(stderr, "Failed to allocate memory for JSON object.\n");
                    return; // Critical error, exit function
                }
                strncpy(json_obj_str, start, obj_len);
                json_obj_str[obj_len] = '\0';

                parse_progress_json(json_obj_str);
                free(json_obj_str);

                current_ptr = end + 1; // Move past the current object
                break; // Break from inner loop to find next object from current_ptr
            }
            end++;
        }
        if (brace_count != 0) { // If we reached end of string but braces didn't balance
            // This indicates an incomplete or malformed JSON object at the end of the stream.
            // We can log it but continue, as previous objects might have been valid.
            // fprintf(stderr, "Incomplete JSON object or malformed stream at: %s\n", start);
            break; // Stop processing this stream as it's malformed from this point
        }
    }
}

// Function to filter out content within <think>...</think> tags if they appear at the beginning
// and trim leading/trailing whitespace.
// Returns a newly allocated string that must be freed by the caller.
char* filter_think_tags(const char* input_str) {
    if (!input_str) return strdup("");

    char* temp_str = strdup(input_str);
    if (!temp_str) {
        fprintf(stderr, "Failed to allocate memory for temporary string.\n");
        return strdup("");
    }

    const char* think_start_tag = "<think>";
    const char* think_end_tag = "</think>";
    size_t think_start_len = strlen(think_start_tag);
    size_t think_end_len = strlen(think_end_tag);

    char* current_content_start = temp_str;

    // Check if the string starts with <think>
    if (strncmp(current_content_start, think_start_tag, think_start_len) == 0) {
        char* content_after_start_tag = current_content_start + think_start_len;
        char* content_before_end_tag = strstr(content_after_start_tag, think_end_tag);

        if (content_before_end_tag != NULL) {
            // The actual response starts *after* the </think> tag
            current_content_start = content_before_end_tag + think_end_len;
        }
    }

    // Trim leading whitespace
    while (*current_content_start != '\0' && isspace((unsigned char)*current_content_start)) {
        current_content_start++;
    }

    // Trim trailing whitespace
    char* end_ptr = current_content_start + strlen(current_content_start) - 1;
    while (end_ptr >= current_content_start && isspace((unsigned char)*end_ptr)) {
        *end_ptr = '\0';
        end_ptr--;
    }

    // Return a new duplicate of the trimmed string
    char* result = strdup(current_content_start);
    free(temp_str); // Free the temporary mutable copy
    return result;
}

// --- New Helper Functions for Image System Refactor ---

// Function to format the theme name for directory creation
char* format_theme_name(const char* theme) {
    if (!theme) return strdup("");

    size_t len = strlen(theme);
    char* formatted = (char*)malloc(len + 1); // Max possible length
    if (!formatted) {
        fprintf(stderr, "Failed to allocate memory for formatted theme name.\n");
        return strdup("");
    }

    int j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = tolower((unsigned char)theme[i]);
        if (isalnum(c)) { // Keep alphanumeric characters
            formatted[j++] = c;
        } else if (c == ' ') { // Replace spaces with underscores
            formatted[j++] = '_';
        }
        // Other special characters are stripped
    }
    formatted[j] = '\0';
    return formatted;
}

// Function to check if a directory exists
bool directory_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Function to create a directory (and its parents if needed)
int create_directory_recursive(const char* path) {
    char* tmp = NULL;
    char* p = NULL;
    size_t len;
    int ret = 0;

    len = strlen(path);
    tmp = strdup(path);
    if (!tmp) {
        fprintf(stderr, "Failed to allocate memory for path.\n");
        return -1;
    }

    // Ensure path ends with a null terminator, not a slash for the loop
    if (len > 0 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0; // Temporarily null-terminate to create parent directory
            if (mkdir(tmp, S_IRWXU) != 0 && errno != EEXIST) {
                fprintf(stderr, "Failed to create directory %s: %s\n", tmp, strerror(errno));
                ret = -1;
                break;
            }
            *p = '/'; // Restore slash
        }
    }
    // Create the final directory
    if (ret == 0 && mkdir(tmp, S_IRWXU) != 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create directory %s: %s\n", tmp, strerror(errno));
        ret = -1;
    }
    free(tmp);
    return ret;
}

// Function to delete all files in a directory (non-recursive for subdirectories)
int delete_files_in_directory(const char* path) {
    DIR *d;
    struct dirent *dir;
    char filepath[MAX_FILEPATH_BUFFER_SIZE]; // Use MAX_FILEPATH_BUFFER_SIZE
    int ret = 0;

    d = opendir(path);
    if (!d) {
        fprintf(stderr, "Failed to open directory %s: %s\n", path, strerror(errno));
        return -1;
    }

    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }

        snprintf(filepath, sizeof(filepath), "%s/%s", path, dir->d_name);
        struct stat st;
        if (stat(filepath, &st) == 0) {
            if (S_ISREG(st.st_mode)) { // Only delete regular files
                if (unlink(filepath) != 0) {
                    fprintf(stderr, "Failed to delete file %s: %s\n", filepath, strerror(errno));
                    ret = -1; // Continue trying to delete others
                }
            }
            // Do not delete subdirectories or special files
        } else {
            fprintf(stderr, "Failed to stat file %s: %s\n", filepath, strerror(errno));
            ret = -1;
        }
    }
    closedir(d);
    return ret;
}

// Function to save game data to a JSON file
bool save_game_data(const char* filename, const char* theme, char** features, int featureCount, char*** characterTraits, int numCharacters) {
    json_t* root = json_object();
    if (!root) {
        fprintf(stderr, "Failed to create JSON root object.\n");
        return false;
    }

    json_object_set_new(root, "theme", json_string(theme));

    json_t* features_array = json_array();
    if (!features_array) {
        fprintf(stderr, "Failed to create JSON features array.\n");
        json_decref(root);
        return false;
    }
    for (int i = 0; i < featureCount; ++i) {
        json_array_append_new(features_array, json_string(features[i]));
    }
    json_object_set_new(root, "features", features_array);

    json_t* characters_array = json_array();
    if (!characters_array) {
        fprintf(stderr, "Failed to create JSON characters array.\n");
        json_decref(root);
        return false;
    }
    for (int i = 0; i < numCharacters; ++i) {
        json_t* character_obj = json_object();
        json_object_set_new(character_obj, "id", json_integer(i + 1));

        json_t* traits_array = json_array();
        for (int j = 0; j < 2; ++j) { // Assuming 2 traits per character
            json_array_append_new(traits_array, json_string(characterTraits[i][j]));
        }
        json_object_set_new(character_obj, "traits", traits_array);

        // Reconstruct the prompt for saving, as it's not stored directly in characterTraits
        char* prompt;
        if (asprintf(&prompt, "A character that is a %s, %s, %s. Cartoon style.", theme, characterTraits[i][0], characterTraits[i][1]) == -1) {
            fprintf(stderr, "Failed to reconstruct prompt for character %d for saving.\n", i + 1);
            json_decref(character_obj);
            json_decref(characters_array);
            json_decref(root);
            return false;
        }
        json_object_set_new(character_obj, "prompt", json_string(prompt));
        free(prompt); // Free the temporary prompt string

        json_array_append_new(characters_array, character_obj);
    }
    json_object_set_new(root, "characters", characters_array);

    int result = json_dump_file(root, filename, JSON_INDENT(4) | JSON_PRESERVE_ORDER);
    json_decref(root);

    if (result != 0) {
        fprintf(stderr, "Failed to save game data to %s.\n", filename);
        return false;
    }
    printf("Game data saved to %s\n", filename);
    return true;
}

// Function to load game data from a JSON file
// This function will populate the global selectedTheme, characterFeatures, featureCount, and characterTraits
// It assumes NUM_CHARACTERS is a global constant.
bool load_game_data(const char* filename, char** loadedTheme, char*** loadedFeatures, int* loadedFeatureCount, char**** loadedCharacterTraits) {
    json_error_t error;
    json_t* root = json_load_file(filename, 0, &error);
    if (!root) {
        fprintf(stderr, "Error loading game data from %s: %s (line %d, col %d)\n", filename, error.text, error.line, error.column);
        return false;
    }

    json_t* theme_json = json_object_get(root, "theme");
    if (!json_is_string(theme_json)) {
        fprintf(stderr, "Invalid or missing 'theme' in game data.\n");
        json_decref(root);
        return false;
    }
    *loadedTheme = strdup(json_string_value(theme_json));

    json_t* features_array = json_object_get(root, "features");
    if (!json_is_array(features_array)) {
        fprintf(stderr, "Invalid or missing 'features' array in game data.\n");
        free(*loadedTheme);
        json_decref(root);
        return false;
    }
    *loadedFeatureCount = json_array_size(features_array);
    *loadedFeatures = (char**)malloc(*loadedFeatureCount * sizeof(char*));
    if (!*loadedFeatures) {
        fprintf(stderr, "Failed to allocate memory for features.\n");
        free(*loadedTheme);
        json_decref(root);
        return false;
    }
    for (int i = 0; i < *loadedFeatureCount; ++i) {
        json_t* feature_json = json_array_get(features_array, i);
        if (!json_is_string(feature_json)) {
            fprintf(stderr, "Invalid feature entry in game data.\n");
            // Clean up already allocated features
            for (int j = 0; j < i; ++j) free((*loadedFeatures)[j]);
            free(*loadedFeatures);
            free(*loadedTheme);
            json_decref(root);
            return false;
        }
        (*loadedFeatures)[i] = strdup(json_string_value(feature_json));
    }

    json_t* characters_array = json_object_get(root, "characters");
    if (!json_is_array(characters_array)) {
        fprintf(stderr, "Invalid or missing 'characters' array in game data.\n");
        // Clean up features
        for (int i = 0; i < *loadedFeatureCount; ++i) free((*loadedFeatures)[i]);
        free(*loadedFeatures);
        free(*loadedTheme);
        json_decref(root);
        return false;
    }
    int actualNumCharacters = json_array_size(characters_array);
    if (actualNumCharacters != NUM_CHARACTERS) { // Validate against global constant
        fprintf(stderr, "Mismatch in character count. Expected %d, got %d. Aborting load.\n", NUM_CHARACTERS, actualNumCharacters);
        // Clean up features
        for (int i = 0; i < *loadedFeatureCount; ++i) free((*loadedFeatures)[i]);
        free(*loadedFeatures);
        free(*loadedTheme);
        json_decref(root);
        return false;
    }

    *loadedCharacterTraits = (char***)malloc(NUM_CHARACTERS * sizeof(char**));
    if (!*loadedCharacterTraits) {
        fprintf(stderr, "Failed to allocate memory for character traits.\n");
        // Clean up features
        for (int i = 0; i < *loadedFeatureCount; ++i) free((*loadedFeatures)[i]);
        free(*loadedFeatures);
        free(*loadedTheme);
        json_decref(root);
        return false;
    }

    for (int i = 0; i < NUM_CHARACTERS; ++i) { // Loop using global constant
        json_t* character_obj = json_array_get(characters_array, i);
        if (!json_is_object(character_obj)) {
            fprintf(stderr, "Invalid character object in game data.\n");
            // Clean up already allocated traits and features
            for (int j = 0; j < i; ++j) {
                if ((*loadedCharacterTraits)[j]) {
                    for (int k = 0; k < 2; ++k) free((*loadedCharacterTraits)[j][k]);
                    free((*loadedCharacterTraits)[j]);
                }
            }
            free(*loadedCharacterTraits);
            for (int j = 0; j < *loadedFeatureCount; ++j) free((*loadedFeatures)[j]);
            free(*loadedFeatures);
            free(*loadedTheme);
            json_decref(root);
            return false;
        }

        json_t* traits_array = json_object_get(character_obj, "traits");
        if (!json_is_array(traits_array) || json_array_size(traits_array) != 2) {
            fprintf(stderr, "Invalid or missing 'traits' array for character %d.\n", i + 1);
            // Clean up
            for (int k = 0; k < i; ++k) { // Free previously allocated character traits
                if ((*loadedCharacterTraits)[k]) {
                    for (int l = 0; l < 2; ++l) free((*loadedCharacterTraits)[k][l]);
                    free((*loadedCharacterTraits)[k]);
                }
            }
            free(*loadedCharacterTraits);
            for (int j = 0; j < *loadedFeatureCount; ++j) free((*loadedFeatures)[j]);
            free(*loadedFeatures);
            free(*loadedTheme);
            json_decref(root);
            return false;
        }

        (*loadedCharacterTraits)[i] = (char**)malloc(2 * sizeof(char*));
        if (!(*loadedCharacterTraits)[i]) {
            fprintf(stderr, "Failed to allocate memory for character %d traits.\n", i + 1);
            // Clean up
            for (int k = 0; k < i; ++k) { // Free previously allocated character traits
                if ((*loadedCharacterTraits)[k]) {
                    for (int l = 0; l < 2; ++l) free((*loadedCharacterTraits)[k][l]);
                    free((*loadedCharacterTraits)[k]);
                }
            }
            free(*loadedCharacterTraits);
            for (int j = 0; j < *loadedFeatureCount; ++j) free((*loadedFeatures)[j]);
            free(*loadedFeatures);
            free(*loadedTheme);
            json_decref(root);
            return false;
        }

        for (int j = 0; j < 2; ++j) {
            json_t* trait_json = json_array_get(traits_array, j);
            if (!json_is_string(trait_json)) {
                fprintf(stderr, "Invalid trait entry for character %d, trait %d.\n", i + 1, j + 1);
                // Clean up
                for (int k = 0; k < j; ++k) free((*loadedCharacterTraits)[i][k]);
                free((*loadedCharacterTraits)[i]);
                for (int l = 0; l < i; ++l) {
                    if ((*loadedCharacterTraits)[l]) {
                        for (int m = 0; m < 2; ++m) free((*loadedCharacterTraits)[l][m]);
                        free((*loadedCharacterTraits)[l]);
                    }
                }
                free(*loadedCharacterTraits);
                for (int l = 0; l < *loadedFeatureCount; ++l) free((*loadedFeatures)[l]);
                free(*loadedFeatures);
                free(*loadedTheme);
                json_decref(root);
                return false;
            }
            (*loadedCharacterTraits)[i][j] = strdup(json_string_value(trait_json));
        }
    }

    json_decref(root);
    printf("Game data loaded from %s\n", filename);
    return true;
}

// --- End New Helper Functions ---

// Adapted generate_image function for character generation
// MODIFIED: Added image_dir parameter
int generate_character_image(const char* prompt, int character_number, const char* image_dir) {
    char render_url[256];
    snprintf(render_url, sizeof(render_url), "http://%s/render", server_url);
    char* data = malloc(4096);
    if (!data) {
        fprintf(stderr, "Failed to allocate memory for data.\n");
        return 1;
    }
    // MODIFIED: Updated save_to_disk_path to use image_dir and removed extra username argument
    int snprintf_result = snprintf(data, 4096,
                                  "{"
                                  "\"prompt\": \"%s\", "
                                  "\"seed\": %u, "
                                  "\"used_random_seed\": true, "
                                  "\"negative_prompt\": \"ugly, deformed, bad anatomy, blurry\", " //ADD a negative prompt
                                  "\"num_outputs\": 1, "
                                  "\"num_inference_steps\": 15, "
                                  "\"guidance_scale\": 7.5, "
                                  "\"width\": 512, "
                                  "\"height\": 512, " // MODIFIED: Changed height to 512
                                  "\"vram_usage_level\": \"balanced\", "
                                  "\"sampler_name\": \"dpmpp_3m_sde\", "
                                  "\"use_stable_diffusion_model\": \"absolutereality_v181\", "
                                  "\"clip_skip\": true, "
                                  "\"use_vae_model\": \"\", "
                                  "\"stream_progress_updates\": true, " // Set to true to get updates
                                  "\"stream_image_progress\": false, "
                                  "\"show_only_filtered_image\": true, "
                                  "\"block_nsfw\": false, "
                                  "\"output_format\": \"png\", "
                                  "\"output_quality\": 75, "
                                  "\"output_lossless\": false, "
                                  "\"metadata_output_format\": \"embed,json\", "
                                  "\"original_prompt\": \"%s\", "
                                  "\"active_tags\": [], "
                                  "\"inactive_tags\": [], "
                                  "\"save_to_disk_path\": \"%s/\", " // Use image_dir here
                                  "\"use_lora_model\": [], "
                                  "\"lora_alpha\": [], "
                                  "\"enable_vae_tiling\": false, "
                                  "\"scheduler_name\": \"automatic\", "
                                  "\"session_id\": \"1337\""
                                  "} ",
                                  prompt, get_random_seed(), prompt, image_dir); // Removed username argument
    if (snprintf_result < 0 || snprintf_result >= 4096) {
        fprintf(stderr, "Error creating data string (truncation detected) %d.\n", snprintf_result);
        free(data);
        return 1;
    }

    char* response = make_http_post(render_url, data);
    if (!response) {
        fprintf(stderr, "Failed to get response from server.\n");
        free(data);
        return 1;
    }

    // Parse the JSON response to get the task ID
    json_error_t error;
    json_t* root = json_loads(response, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        free(response);
        return 1;
    }

    json_t* task_json = json_object_get(root, "task");
    const char* task = NULL;
    char* task_str = NULL; // Declare task_str here
    if (!task_json) {
        fprintf(stderr, "Task ID not found in JSON response.\n");
        json_decref(root);
        free(response);
        free(data);
        return 1;
    }

    if (json_is_integer(task_json)) {
        // Convert the integer task ID to a string
        long long task_id = json_integer_value(task_json);
        task_str = malloc(32); // Allocate memory for the string
        if (!task_str) {
            fprintf(stderr, "Failed to allocate memory for task ID.\n");
            json_decref(root);
            free(response);
            free(data);
            return 1;
        }
        snprintf(task_str, 32, "%lld", task_id);
        task = task_str; // Assign the allocated string to task
    } else if (json_is_string(task_json)) {
        task = json_string_value(task_json);
        if (!task) {
            fprintf(stderr, "Task ID is not a string.\n");
            json_decref(root);
            free(response);
            free(data);
            return 1;
        }
    } else {
        fprintf(stderr, "Task ID is not a string or integer.\n");
        json_decref(root);
        free(response);
        free(data);
        return 1;
    }

    printf("Task ID: %s\n", task);
    json_decref(root);
    free(response);
    free(data);

    // Poll for the image
    char image_url[256];
    char ping_url[256];
    snprintf(image_url, sizeof(image_url), "http://%s/image/stream/%s", server_url, task);
    snprintf(ping_url, sizeof(ping_url), "http://%s/ping?session_id=1337", server_url);

    char* server_status = strdup("unknown"); // Initialize server status
    int ret_val = 0; // Default to success

    while (strcmp(server_status, "Online") != 0 && !WindowShouldClose()) {
        sleep(5); // Increased sleep duration to 2 seconds

        // --- Get status from ping_url ---
        char* ping_response = make_http_get(ping_url);
        if (!ping_response) {
            fprintf(stderr, "Failed to get ping response from server (ping_url: %s).\n", ping_url);
            // Continue loop, maybe stream data will provide progress or it will eventually finish
        } else {
            json_error_t error;
            json_t* root_ping = json_loads(ping_response, 0, &error);
            if (!root_ping) {
                fprintf(stderr, "Error parsing ping JSON: %s\n", error.text);
            } else {
                json_t* status_json = json_object_get(root_ping, "status");
                if (status_json && json_is_string(status_json)) {
                    free(server_status); // Free previous status string
                    server_status = strdup(json_string_value(status_json));
                    printf("Server Status: %s\n", server_status);
                } else {
                    fprintf(stderr, "Status field not found or not a string in ping response. Assuming still generating.\n");
                }
                json_decref(root_ping);
            }
            free(ping_response);
        }

        // --- Get stream data from image_url for percentage updates ---
        char* stream_response = make_http_get(image_url);
        if (!stream_response) {
            fprintf(stderr, "Failed to get stream response from server (image_url: %s).\n", image_url);
            // Continue loop, maybe ping will eventually tell us it's done
        } else {
            // Process the stream response using the new robust parser
            process_json_stream_response(stream_response);
            free(stream_response); // Free the original response
        }

        printf("Task: %s, Prompt: %s, Percent Done: %s\n", task, prompt, current_percent);
    }

    // If we broke out of the loop due to an error (e.g., memory allocation failure), return early
    if (ret_val != 0) {
        if (task_str) free(task_str);
        free(server_status);
        return ret_val;
    }

    // Get the final image
    char* final_stream_response = make_http_get(image_url);
    if (!final_stream_response) {
        fprintf(stderr, "Failed to get final stream response from server.\n");
        if (task_str) free(task_str);
        free(server_status);
        return 1;
    }

    // Find the start of the base64 image data
    char* data_start = strstr(final_stream_response, "\"data\":\"data:image/png;base64,");
    if (!data_start) {
        fprintf(stderr, "Image data not found in JSON response.\n");
        free(final_stream_response);
        if (task_str) free(task_str);
        free(server_status);
        return 1;
    }

    // Move the pointer to the beginning of the base64 data
    data_start += strlen("\"data\":\"data:image/png;base64,");

    // Find the end of the base64 data
    char* data_end = strchr(data_start, '"');
    if (!data_end) {
        fprintf(stderr, "End of image data not found in JSON response.\n");
        free(final_stream_response);
        if (task_str) free(task_str);
        free(server_status);
        return 1;
    }

    // Calculate the length of the base64 encoded data
    size_t image_data_len = data_end - data_start;

    // Allocate memory for the base64 encoded data
    char* image_data_base64 = malloc(image_data_len + 1);
    if (!image_data_base64) {
        fprintf(stderr, "Failed to allocate memory for base64 data.\n");
        free(final_stream_response);
        if (task_str) free(task_str);
        free(server_status);
        return 1;
    }

    // Copy the base64 encoded data
    strncpy(image_data_base64, data_start, image_data_len);
    image_data_base64[image_data_len] = '\0';

    // Clean up the original response, as it's no longer needed
    free(final_stream_response);

    // Decode the base64 image data
    size_t decoded_size;
    unsigned char* decoded_data = base64_decode(image_data_base64, image_data_len, &decoded_size);
    if (!decoded_data) {
        fprintf(stderr, "Base64 decoding failed.\n");
        free(image_data_base64);
        if (task_str) free(task_str);
        free(server_status);
        return 1;
    }

    // Free the base64 encoded data
    free(image_data_base64);

    // Save the decoded image data to a file
    char filename[MAX_FILEPATH_BUFFER_SIZE]; // Use MAX_FILEPATH_BUFFER_SIZE
    snprintf(filename, sizeof(filename), "%s/character_%d.png", image_dir, character_number); // Use image_dir here
    FILE* fp = fopen(filename, "wb");
    if (fp) {
        fwrite(decoded_data, 1, decoded_size, fp); // Corrected fwrite arguments
        fclose(fp);
        printf("Image saved to %s\n", filename);
    } else {
        fprintf(stderr, "Failed to save image to file %s: %s\n", filename, strerror(errno));
    }

    free(decoded_data);
    if (task_str) free(task_str);
    free(server_status); // Free server status
    return 0;
}

// Struct to hold data for a single image generation task
typedef struct {
    char* prompt;
    int character_number;
    // const char* image_dir; // Removed from here, now part of BatchImageGenData
} SingleImageGenData;

// Struct to hold data for batch image generation
typedef struct {
    SingleImageGenData* images_data; // Array of prompts and character numbers
    int num_images;
    const char* image_dir; // MODIFIED: Added image_dir to struct
} BatchImageGenData;

// Function to run image generation in a separate thread
void* generateImageThread(void* arg) {
    BatchImageGenData* batch_data = (BatchImageGenData*)arg;

    pthread_mutex_lock(&mutex);
    generating_images = true;
    total_characters_to_generate = batch_data->num_images;
    characters_generated_count = 0;
    pthread_mutex_unlock(&mutex);

    for (int i = 0; i < batch_data->num_images; ++i) {
        pthread_mutex_lock(&mutex);
        characters_generated_count = i + 1; // Update count for display
        snprintf(generation_status_message, sizeof(generation_status_message),
                 "Generating image %d of %d...", characters_generated_count, total_characters_to_generate);
        snprintf(current_percent, sizeof(current_percent), "0%%"); // Initialize percentage for new image
        pthread_mutex_unlock(&mutex);

        SingleImageGenData* current_image_data = &batch_data->images_data[i];
        // MODIFIED: Pass image_dir from batch_data to generate_character_image
        generate_character_image(current_image_data->prompt, current_image_data->character_number, batch_data->image_dir);

        // Free prompt after use
        free(current_image_data->prompt);
    }

    pthread_mutex_lock(&mutex);
    generating_images = false;
    snprintf(generation_status_message, sizeof(generation_status_message), "Image generation complete!");
    current_percent[0] = '\0'; // Clear percentage
    pthread_mutex_unlock(&mutex);

    // Free the array of SingleImageGenData itself, but not the prompts within it, as they were freed above.
    free(batch_data->images_data);
    free(batch_data);
    return NULL;
}

// Function to get the theme from the LLM
char** getThemesFromLLM(int* themeCount) {
    char** themes = NULL;
    *themeCount = 0;

    const char* prompt = "Suggest 10 themes for a 'Guess Who?' game. The theme should be who the characters in the game are, and should be a singular noun. For example: 'clown', 'shih-tzu dog', 'penguin', 'llama', etc. Feel free to be creative and random. Return a JSON list of strings, only the themes and nothing else.";
    double temperature = 1.0;
    char* llmResponse = getLLMResponse(prompt, temperature);

    if (llmResponse != NULL) {
        // Parse the JSON response
        json_error_t error;
        json_t* root = json_loads(llmResponse, 0, &error);

        if (!root) {
            fprintf(stderr, "Error parsing JSON: %s\n", error.text);
            free(llmResponse);
            return NULL;
        }

        json_t* choices = json_object_get(root, "choices");
        if (!json_is_array(choices) || json_array_size(choices) == 0) {
            fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
            fprintf(stderr, "Raw LLM Response: %s\n", llmResponse); // Print the raw response for debugging
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* firstChoice = json_array_get(choices, 0);
        if (!json_is_object(firstChoice)) {
            fprintf(stderr, "Error: First choice is not an object.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* message = json_object_get(firstChoice, "message");
        if (!json_is_object(message)) {
            fprintf(stderr, "Error: 'message' is not an object.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* content = json_object_get(message, "content");
        if (!json_is_string(content)) {
            fprintf(stderr, "Error: 'content' is not a string.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        const char* contentStr_raw = json_string_value(content);
        char* contentStr_processed = filter_think_tags(contentStr_raw); // Filter out <think> tags

        // Strip ```json and ``` from the content string
        char* contentStrStripped = strdup(contentStr_processed);
        free(contentStr_processed); // Free the intermediate filtered string

        if (strncmp(contentStrStripped, "```json", 7) == 0) {
            memmove(contentStrStripped, contentStrStripped + 7, strlen(contentStrStripped) - 6);
        }
        size_t len = strlen(contentStrStripped);
        if (len > 3 && strcmp(contentStrStripped + len - 3, "```") == 0) {
            contentStrStripped[len - 3] = '\0';
        }

        // Parse the content as a JSON array
        json_t* contentArray = json_loads(contentStrStripped, 0, &error);
        if (!json_is_array(contentArray)) {
            fprintf(stderr, "Error parsing content as JSON array: %s\n", error.text);
            json_decref(root);
            free(llmResponse);
            free(contentStrStripped);
            return NULL;
        }

        // Extract the themes from the JSON array
        *themeCount = json_array_size(contentArray);
        themes = (char**)malloc(*themeCount * sizeof(char*));
        if (themes == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            json_decref(root);
            json_decref(contentArray);
            free(llmResponse);
            free(contentStrStripped);
            return NULL;
        }

        for (int i = 0; i < *themeCount; i++) {
            json_t* theme = json_array_get(contentArray, i);
            if (!json_is_string(theme)) {
                fprintf(stderr, "Error: Theme is not a string.\n");
                // Free already allocated themes
                for (int j = 0; j < i; j++) {
                    free(themes[j]);
                }
                free(themes);
                themes = NULL;
                *themeCount = 0;
                json_decref(root);
                json_decref(contentArray);
                free(llmResponse);
                free(contentStrStripped);
                return NULL;
            }
            themes[i] = strdup(json_string_value(theme));
        }

        // Cleanup
        json_decref(root);
        json_decref(contentArray);
        free(llmResponse);
        free(contentStrStripped);
    } else {
        fprintf(stderr, "Failed to get response from LLM\n");
        // If max retries reached, return a default theme
        *themeCount = 1;
        themes = (char**)malloc(sizeof(char*));
        themes[0] = strdup("Default");
    }

    return themes;
}

// Function to get character features from the LLM based on the theme
char** getCharacterFeatures(const char* theme, int* featureCount) {
    char** features = NULL;
    *featureCount = 0;

    // Construct the prompt for the LLM
    char* prompt;
    if (asprintf(&prompt, "Given the theme '%s', suggest 8 distinct features that could be used to differentiate characters in a 'Guess Who?' game. These features should be physical attributes or accessories. Return a JSON list of strings, only the features and nothing else.  For example, if the theme is 'clowns', the features could be: big red nose, blue wig, green hair, top hat, etc.", theme) == -1) {
        fprintf(stderr, "Failed to construct prompt\n");
        return NULL;
    }

    double temperature = 1.0;
    char* llmResponse = getLLMResponse(prompt, temperature);
    free(prompt);

    if (llmResponse != NULL) {
        // Add debug logging: print the raw LLM response
        printf("Raw LLM Response: %s\n", llmResponse);

        // Parse the JSON response
        json_error_t error;
        json_t* root = json_loads(llmResponse, 0, &error);

        if (!root) {
            fprintf(stderr, "Error parsing JSON: %s\n", error.text);
            free(llmResponse);
            return NULL;
        }

        json_t* choices = json_object_get(root, "choices");
        if (!json_is_array(choices) || json_array_size(choices) == 0) {
            fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
            fprintf(stderr, "Raw LLM Response: %s\n", llmResponse); // Print the raw response for debugging
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* firstChoice = json_array_get(choices, 0);
        if (!json_is_object(firstChoice)) {
            fprintf(stderr, "Error: First choice is not an object.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* message = json_object_get(firstChoice, "message");
        if (!json_is_object(message)) {
            fprintf(stderr, "Error: 'message' is not an object.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        json_t* content = json_object_get(message, "content");
        if (!json_is_string(content)) {
            fprintf(stderr, "Error: 'content' is not a string.\n");
            json_decref(root);
            free(llmResponse);
            return NULL;
        }

        const char* contentStr_raw = json_string_value(content);
        char* contentStr_processed = filter_think_tags(contentStr_raw); // Filter out <think> tags

        // Add debug logging: print the content string
        //printf("Content string: %s\n", contentStr_processed);

        // Strip ```json and ``` from the content string
        char* contentStrStripped = strdup(contentStr_processed);
        free(contentStr_processed); // Free the intermediate filtered string

        if (strncmp(contentStrStripped, "```json", 7) == 0) {
            memmove(contentStrStripped, contentStrStripped + 7, strlen(contentStrStripped) - 6);
        }
        size_t len = strlen(contentStrStripped);
        if (len > 3 && strcmp(contentStrStripped + len - 3, "```") == 0) {
            contentStrStripped[len - 3] = '\0';
        }

        // Parse the content as a JSON array
        json_t* contentArray = json_loads(contentStrStripped, 0, &error);
        if (!json_is_array(contentArray)) {
            fprintf(stderr, "Error parsing content as JSON array: %s\n", error.text);
            json_decref(root);
            free(llmResponse);
            free(contentStrStripped);
            return NULL;
        }

        // Extract the features from the JSON array
        *featureCount = json_array_size(contentArray);
        if (*featureCount != 8) {
            fprintf(stderr, "Warning: Expected 8 features, but got %d\n", *featureCount);
        }

        features = (char**)malloc(*featureCount * sizeof(char*));
        if (features == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            json_decref(root);
            json_decref(contentArray);
            free(llmResponse);
            free(contentStrStripped);
            return NULL;
        }

        for (int i = 0; i < *featureCount; i++) {
            json_t* feature = json_array_get(contentArray, i);
            if (!json_is_string(feature)) {
                fprintf(stderr, "Error: Feature is not a string.\n");
                // Free already allocated features
                for (int j = 0; j < i; j++) {
                    free(features[j]);
                }
                free(features);
                features = NULL;
                *featureCount = 0;
                json_decref(root);
                json_decref(contentArray);
                free(llmResponse);
                free(contentStrStripped);
                return NULL;
            }
            features[i] = strdup(json_string_value(feature));
        }

        // Cleanup
        json_decref(root);
        json_decref(contentArray);
        free(llmResponse);
        free(contentStrStripped);
    } else {
        fprintf(stderr, "Failed to get response from LLM\n");
    }

    return features;
}

// Function to set the yes/no question
void setYesNoInput(const char* question) {
    strncpy(currentQuestion, question, sizeof(currentQuestion) - 1);
    currentQuestion[sizeof(currentQuestion) - 1] = '\0';
    currentAnswer = -1; // Reset answer
}

// Function for the LLM to make a guessing round
// MODIFIED: Added playerCharacter parameter
void llmGuessingRound(char*** characterTraits, int llmCharacter, const char* theme, int numCharacters, int* charactersRemaining, int* remainingCount, Texture2D playerTexture, int playerCharacter) {
    printf("LLM is thinking...\n");

    // Construct the prompt for the LLM to formulate a question
    char* questionPrompt;
    char* characterList = NULL;

    // Build a string containing the character traits, excluding the LLM's own character
    for (int i = 0; i < numCharacters; ++i) {
        if (i == llmCharacter) continue; // Skip the LLM's own character
        // Check if the character is still in the game
        int stillInGame = 0;
        for (int j = 0; j < *remainingCount; j++) {
            if (charactersRemaining[j] == i) {
                stillInGame = 1;
                break;
            }
        }
        if (!stillInGame) continue;

        char* characterString = NULL;
        if (asprintf(&characterString, "Character %d: ", i + 1) == -1) {
            fprintf(stderr, "Failed to construct character string\n");
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        for (int j = 0; j < 2; ++j) { // Assuming each character has 2 features
            if (characterTraits[i] != NULL && characterTraits[i][j] != NULL) {
                char* temp = NULL;
                if (asprintf(&temp, "%s%s%s", characterString, characterTraits[i][j], (j < 1) ? ", " : "") == -1) {
                    fprintf(stderr, "Failed to append feature to character string\n");
                    free(characterString);
                    if (characterList) free(characterList); // Free characterList on error
                    return;
                }
                free(characterString);
                characterString = temp;
            }
        }

        // Append the character string to the character list
        char* temp2 = NULL;
        if (characterList == NULL) {
            if (asprintf(&temp2, "%s\\n", characterString) == -1) {
                fprintf(stderr, "Failed to construct character list\n");
                free(characterString);
                return;
            }
            characterList = temp2;
        } else {
            if (asprintf(&temp2, "%s%s\\n", characterList, characterString) == -1) {
                fprintf(stderr, "Failed to append character to character list\n");
                free(characterString);
                free(characterList);
                return;
            }
            free(characterList);
            characterList = temp2;
        }
        free(characterString);
    }

    // Construct the question prompt
    if (asprintf(&questionPrompt, "Given the theme '%s' and the following list of characters and their traits: %s Your goal is to guess the player's character, which is one of the characters in this list. Formulate a yes/no question that will help you narrow down the possibilities. The question should be about a single trait from the list of character features. Return the question as a string, only the question and nothing else.", theme, characterList ? characterList : "") == -1) {
        fprintf(stderr, "Failed to construct question prompt\n");
        if (characterList) free(characterList); // Free characterList on error
        return;
    }
    // characterList is NOT freed here, as it's needed for eliminationPrompt

    // Print the prompt before sending it to the LLM
    printf("Prompt sent to LLM:\n%s\n", questionPrompt);

    double temperature = 0.7;
    char* llmQuestionResponse = getLLMResponse(questionPrompt, temperature);
    free(questionPrompt);

    char* question_filtered = NULL; // Declare here to ensure scope for freeing

    if (llmQuestionResponse != NULL) {
        // Parse the JSON response
        json_error_t error;
        json_t* root = json_loads(llmQuestionResponse, 0, &error);

        if (!root) {
            fprintf(stderr, "Error parsing JSON: %s\n", error.text);
            free(llmQuestionResponse);
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        json_t* choices = json_object_get(root, "choices");
        if (!json_is_array(choices) || json_array_size(choices) == 0) {
            fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
            fprintf(stderr, "Raw LLM Response: %s\n", llmQuestionResponse); // Print the raw response for debugging
            json_decref(root);
            free(llmQuestionResponse);
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        json_t* firstChoice = json_array_get(choices, 0);
        if (!json_is_object(firstChoice)) {
            fprintf(stderr, "Error: First choice is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        json_t* message = json_object_get(firstChoice, "message");
        if (!json_is_object(message)) {
            fprintf(stderr, "Error: 'message' is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        json_t* content = json_object_get(message, "content");
        if (!json_is_string(content)) {
            fprintf(stderr, "Error: 'content' is not a string.\n");
            json_decref(root);
            free(llmQuestionResponse);
            if (characterList) free(characterList); // Free characterList on error
            return;
        }

        const char* question_raw = json_string_value(content);
        question_filtered = filter_think_tags(question_raw); // Filter out <think> tags

        // Ask the question to the user (using Raylib GUI)
        printf("LLM asks: %s\n", question_filtered);
        setYesNoInput(question_filtered); // Set the question for the GUI

        // Wait for the user to answer
        while (currentAnswer == -1 && !WindowShouldClose()) {
            BeginDrawing();
            ClearBackground(RAYWHITE);

            // Draw player character string
            DrawText(playerCharacterString, 10, 10, 20, BLACK);

            // Draw the player's character image
            if (playerTexture.id != 0) {
                // MODIFIED: Draw at (195.2, 170) scaled to 0.8
                DrawTextureEx(playerTexture, (Vector2){195.2f, 170.0f}, 0.8f, 0.8f, WHITE);
            }

            // Draw the question
            DrawText(currentQuestion, 100, 70, 20, GRAY);

            // Draw "Yes" and "No" buttons
            Rectangle yesButton = {100, 120, 80, 30};
            Rectangle noButton = {200, 120, 80, 30};
            DrawRectangleRec(yesButton, GREEN);
            DrawText("Yes", yesButton.x + 20, yesButton.y + 5, 20, WHITE);
            DrawRectangleRec(noButton, RED);
            DrawText("No", noButton.x + 20, noButton.y + 5, 20, WHITE);

            EndDrawing();

            // Check for button presses
            if (CheckCollisionPointRec(GetMousePosition(), yesButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 1;
            }
            if (CheckCollisionPointRec(GetMousePosition(), noButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                currentAnswer = 0;
            }
        }

        if (WindowShouldClose()) {
            // Handle window close event
            json_decref(root);
            free(llmQuestionResponse);
            if (question_filtered) free(question_filtered); // Free filtered string on exit
            if (characterList) free(characterList); // Free characterList on exit
            return;
        }

        // Construct the prompt for the LLM to determine which characters to eliminate
        char* eliminationPrompt;
        const char* answerString = currentAnswer ? "yes" : "no";

        char* eliminationInstruction = NULL;
        if (currentAnswer == 0) { // User said NO
            if (asprintf(&eliminationInstruction, "This means the player's character does NOT have the feature asked about. Therefore, eliminate all characters from the list that *do* have the feature asked about.") == -1) {
                fprintf(stderr, "Failed to construct elimination instruction for NO\n");
            }
        } else { // User said YES
            if (asprintf(&eliminationInstruction, "This means the player's character DOES have the feature asked about. Therefore, eliminate all characters from the list that *do not* have the feature asked about.") == -1) {
                fprintf(stderr, "Failed to construct elimination instruction for YES\n");
            }
        }

        // Use question_filtered here
        if (asprintf(&eliminationPrompt, "Given the theme '%s', the question '%s' was asked, and the answer was '%s'. Given the following list of characters and their traits: %s %s Return a JSON list of integers, only the character numbers and nothing else.", theme, question_filtered, answerString, characterList ? characterList : "", eliminationInstruction ? eliminationInstruction : "") == -1) {
            fprintf(stderr, "Failed to construct elimination prompt\n");
            json_decref(root);
            free(llmQuestionResponse);
            if (eliminationInstruction) free(eliminationInstruction);
            if (question_filtered) free(question_filtered); // Free filtered string on error
            if (characterList) free(characterList); // Free characterList on error
            return;
        }
        if (eliminationInstruction) free(eliminationInstruction); // Free the instruction string

        printf("Elimination Prompt sent to LLM:\n%s\n", eliminationPrompt);

        char* llmEliminationResponse = getLLMResponse(eliminationPrompt, temperature);
        free(eliminationPrompt);

        if (llmEliminationResponse != NULL) {
            // Add debug logging: Print the raw elimination response
            printf("Raw LLM Elimination Response: %s\n", llmEliminationResponse);

            // Parse the JSON response for the elimination
            json_error_t error;
            json_t* rootElimination = json_loads(llmEliminationResponse, 0, &error);
            if (!rootElimination) {
                fprintf(stderr, "Error parsing JSON for elimination: %s\n", error.text);
                free(llmEliminationResponse);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            json_t* choicesElimination = json_object_get(rootElimination, "choices");
            if (!json_is_array(choicesElimination) || json_array_size(choicesElimination) == 0) {
                fprintf(stderr, "Error: 'choices' is not a non-empty array for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            json_t* firstChoiceElimination = json_array_get(choicesElimination, 0);
            if (!json_is_object(firstChoiceElimination)) {
                fprintf(stderr, "Error: First choice is not an object for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            json_t* messageElimination = json_object_get(firstChoiceElimination, "message");
            if (!json_is_object(messageElimination)) {
                fprintf(stderr, "Error: 'message' is not an object for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            json_t* contentElimination = json_object_get(messageElimination, "content");
            if (!json_is_string(contentElimination)) {
                fprintf(stderr, "Error: 'content' is not a string for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            const char* contentStrElimination_raw = json_string_value(contentElimination);
            char* contentStrStrippedElimination = filter_think_tags(contentStrElimination_raw); // Filter out <think> tags

            // Strip ```json and ``` from the content string
            if (strncmp(contentStrStrippedElimination, "```json", 7) == 0) {
                memmove(contentStrStrippedElimination, contentStrStrippedElimination + 7, strlen(contentStrStrippedElimination) - 6);
            }
            size_t lenElimination = strlen(contentStrStrippedElimination);
            if (lenElimination > 3 && strcmp(contentStrStrippedElimination + lenElimination - 3, "```") == 0) {
                contentStrStrippedElimination[lenElimination - 3] = '\0';
            }

            // Parse the content as a JSON array
            json_t* contentArrayElimination = json_loads(contentStrStrippedElimination, 0, &error);
            if (!json_is_array(contentArrayElimination)) {
                fprintf(stderr, "Error parsing content as JSON array for elimination: %s\n", error.text);
                json_decref(rootElimination);
                free(llmEliminationResponse);
                free(contentStrStrippedElimination);
                json_decref(root); // Free root from question response
                if (question_filtered) free(question_filtered); // Free filtered string
                if (characterList) free(characterList); // Free characterList on error
                return;
            }

            int numEliminate = json_array_size(contentArrayElimination);
            printf("LLM suggests eliminating %d characters.\n", numEliminate);

            // Iterate through the array and eliminate the characters
            for (int i = 0; i < numEliminate; i++) {
                json_t* charIndex = json_array_get(contentArrayElimination, i);
                if (!json_is_integer(charIndex)) {
                    fprintf(stderr, "Error: Character index is not an integer.\n");
                    continue;
                }
                int characterToEliminate = json_integer_value(charIndex) - 1; // Subtract 1 to get 0-based index

                // Check if the character to eliminate is the player's character
                if (characterToEliminate == playerCharacter) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYER_WINS;
                    pthread_mutex_unlock(&mutex);
                    printf("LLM eliminated player's character! Player wins!\n");
                    // No need to continue processing eliminations if player wins
                    break;
                }

                // Find the character in the remaining characters array and remove it
                int found = 0;
                for (int j = 0; j < *remainingCount; j++) {
                    if (charactersRemaining[j] == characterToEliminate) {
                        // Remove the character by shifting the elements
                        for (int k = j; k < *remainingCount - 1; k++) {
                            charactersRemaining[k] = charactersRemaining[k + 1];
                        }
                        (*remainingCount)--;
                        printf("Eliminating character %d\n", characterToEliminate + 1);
                        found = 1;
                        break;
                    }
                }
                if (!found) {
                    fprintf(stderr, "Warning: Character %d not found in remaining characters.\n", characterToEliminate + 1);
                }
            }

            // Cleanup
            json_decref(contentArrayElimination);
            free(contentStrStrippedElimination);
            json_decref(rootElimination);
            free(llmEliminationResponse);
        } else {
            fprintf(stderr, "Failed to get elimination response from LLM\n");
        }

        // Cleanup for question response
        json_decref(root);
        free(llmQuestionResponse);
    } else {
        fprintf(stderr, "Failed to get response from LLM\n");
    }
    if (question_filtered) free(question_filtered); // Free the filtered question string here
    if (characterList) free(characterList); // Free characterList here, after all uses
}

// Function to clear the screen and redraw the background
void clearScreen() {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    EndDrawing();
}

// Function to draw the image generation progress screen
void DrawImageGenerationProgressScreen(void) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    pthread_mutex_lock(&mutex);
    DrawText(generation_status_message, 10, 10, 20, BLACK);
    DrawText(current_percent, 10, 40, 20, BLACK);
    pthread_mutex_unlock(&mutex);
    EndDrawing();
}

// Struct to pass arguments to gameSetupThread
typedef struct {
    char theme_input[100];
    bool llm_selected;
} SetupThreadArgs;

// Function to run initial game setup in a separate thread
void* gameSetupThread(void* arg) {
    SetupThreadArgs* args = (SetupThreadArgs*)arg;

    pthread_mutex_lock(&mutex);
    setup_in_progress = true;
    snprintf(generation_status_message, sizeof(generation_status_message), "Preparing game data...");
    pthread_mutex_unlock(&mutex);

    if (args->llm_selected) {
        int themeCount;
        char** themes = getThemesFromLLM(&themeCount);
        if (themes != NULL && themeCount > 0) {
            int randomIndex = rand() % themeCount;
            selectedTheme = strdup(themes[randomIndex]);
            printf("LLM selected theme: %s\n", selectedTheme);
            for (int i = 0; i < themeCount; i++) {
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

    // MODIFIED: Format theme name and check/create directory
    char* temp_formatted_name = format_theme_name(selectedTheme);
    strncpy(formattedThemeName, temp_formatted_name, sizeof(formattedThemeName) - 1);
    formattedThemeName[sizeof(formattedThemeName) - 1] = '\0';
    free(temp_formatted_name); // Free the dynamically allocated string

    // Use a temporary buffer to construct the path, then copy to global
    char temp_image_dir_path[512]; // Sufficient for "images/" + 255 chars + null
    snprintf(temp_image_dir_path, sizeof(temp_image_dir_path), "images/%s", formattedThemeName);
    strncpy(imageDirectoryPath, temp_image_dir_path, sizeof(imageDirectoryPath) - 1);
    imageDirectoryPath[sizeof(imageDirectoryPath) - 1] = '\0'; // Ensure null termination

    // Construct the path for game_data.json
    char gameDataFilePath[MAX_FILEPATH_BUFFER_SIZE];
    snprintf(gameDataFilePath, sizeof(gameDataFilePath), "%s/game_data.json", imageDirectoryPath);

    if (directory_exists(imageDirectoryPath)) {
        pthread_mutex_lock(&mutex);
        confirm_regen_prompt_active = true;
        regen_choice = -1; // Reset choice
        pthread_mutex_unlock(&mutex);

        // Wait for user input from main thread
        pthread_mutex_lock(&mutex);
        while (confirm_regen_prompt_active && !WindowShouldClose()) { // Wait until main thread processes the prompt
            pthread_cond_wait(&regen_cond, &mutex);
        }
        pthread_mutex_unlock(&mutex);

        if (WindowShouldClose()) { // If window closed while waiting
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            free(args);
            return NULL;
        }

        if (regen_choice == 1) { // User chose Yes to re-create
            pthread_mutex_lock(&mutex);
            snprintf(generation_status_message, sizeof(generation_status_message), "Deleting old images...");
            pthread_mutex_unlock(&mutex);
            if (delete_files_in_directory(imageDirectoryPath) != 0) {
                fprintf(stderr, "Failed to delete old images in %s.\n", imageDirectoryPath);
                pthread_mutex_lock(&mutex);
                currentGameState = GAME_STATE_EXIT;
                setup_in_progress = false;
                pthread_mutex_unlock(&mutex);
                free(args);
                return NULL;
            }
            // Proceed to create directory and generate images (fall through to common generation path)
        } else { // User chose No, skip image generation
            pthread_mutex_lock(&mutex);
            snprintf(generation_status_message, sizeof(generation_status_message), "Skipping image generation. Loading existing game data...");
            pthread_mutex_unlock(&mutex);

            // Attempt to load game data
            if (!load_game_data(gameDataFilePath, &selectedTheme, &characterFeatures, &featureCount, &characterTraits)) {
                fprintf(stderr, "Failed to load existing game data. Cannot proceed without re-generating images. Exiting.\n");
                pthread_mutex_lock(&mutex);
                currentGameState = GAME_STATE_EXIT; // Critical error, exit game
                setup_in_progress = false;
                pthread_mutex_unlock(&mutex);
                free(args);
                return NULL;
            }

            // Assign random character to player
            playerCharacter = rand() % NUM_CHARACTERS; // Use the global NUM_CHARACTERS constant
            printf("\nYou are character number %d\n", playerCharacter + 1);

            // Construct the player character string
            pthread_mutex_lock(&mutex);
            snprintf(playerCharacterString, sizeof(playerCharacterString), "Character %d: %s, %s",
                     playerCharacter + 1, characterTraits[playerCharacter][0], characterTraits[playerCharacter][1]);
            snprintf(characterSelectionText, sizeof(characterSelectionText), "You are character number %d", playerCharacter + 1);
            pthread_mutex_unlock(&mutex);


            // Assign random character to LLM, making sure it's different from the player's
            do {
                llmCharacter = rand() % NUM_CHARACTERS;
            } while (llmCharacter == playerCharacter);

            // Initialize the array of remaining characters
            charactersRemaining = (int*)malloc(NUM_CHARACTERS * sizeof(int));
            if (charactersRemaining == NULL) {
                fprintf(stderr, "Memory allocation failed\n");
                pthread_mutex_lock(&mutex);
                currentGameState = GAME_STATE_EXIT;
                pthread_mutex_unlock(&mutex);
                pthread_mutex_lock(&mutex);
                setup_in_progress = false;
                pthread_mutex_unlock(&mutex);
                free(args);
                return NULL;
            }
            remainingCount = NUM_CHARACTERS;
            for (int i = 0; i < NUM_CHARACTERS; i++) {
                charactersRemaining[i] = i;
            }

            pthread_mutex_lock(&mutex);
            setup_in_progress = false; // Indicate setup is complete
            // currentGameState = GAME_STATE_PLAYING; // REMOVED: Main thread will handle state transition
            pthread_mutex_unlock(&mutex);
            free(args);
            return NULL; // Exit this thread early
        }
    }

    // If directory didn't exist or user chose to re-create, ensure it's created
    pthread_mutex_lock(&mutex);
    snprintf(generation_status_message, sizeof(generation_status_message), "Creating image directory...");
    pthread_mutex_unlock(&mutex);
    if (create_directory_recursive(imageDirectoryPath) != 0) {
        fprintf(stderr, "Failed to create image directory %s.\n", imageDirectoryPath);
        pthread_mutex_lock(&mutex);
        currentGameState = GAME_STATE_EXIT;
        setup_in_progress = false;
        pthread_mutex_unlock(&mutex);
        free(args);
        return NULL;
    }

    free(args); // Free the arguments struct (only if we didn't return early)

    pthread_mutex_lock(&mutex);
    snprintf(generation_status_message, sizeof(generation_status_message), "Getting character features...");
    pthread_mutex_unlock(&mutex);

    characterFeatures = getCharacterFeatures(selectedTheme, &featureCount);

    if (characterFeatures != NULL && featureCount > 0) {
        characterTraits = (char***)malloc(NUM_CHARACTERS * sizeof(char**));
        if (characterTraits == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            pthread_mutex_unlock(&mutex);
            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            return NULL;
        }

        for (int i = 0; i < NUM_CHARACTERS; ++i) {
            int numCharFeatures = 2;
            characterTraits[i] = (char**)malloc(numCharFeatures * sizeof(char*));
            if (characterTraits[i] == NULL) {
                fprintf(stderr, "Memory allocation failed\n");
                pthread_mutex_lock(&mutex);
                currentGameState = GAME_STATE_EXIT;
                pthread_mutex_unlock(&mutex);
                // Clean up already allocated characterTraits
                for (int k = 0; k < i; ++k) {
                    for (int l = 0; l < 2; ++l) {
                        if (characterTraits[k][l]) free(characterTraits[k][l]);
                    }
                    free(characterTraits[k]);
                }
                free(characterTraits);
                characterTraits = NULL;
                pthread_mutex_lock(&mutex);
                setup_in_progress = false;
                pthread_mutex_unlock(&mutex);
                return NULL;
            }
            for (int j = 0; j < numCharFeatures; ++j) {
                int featureIndex = rand() % featureCount;
                characterTraits[i][j] = strdup(characterFeatures[featureIndex]);
            }
        }

        printf("\nCharacter Traits:\n");
        for (int i = 0; i < NUM_CHARACTERS; ++i) {
            printf("Character %d: ", i + 1);
            int numCharFeatures = 2;
            for (int j = 0; j < numCharFeatures; ++j) {
                printf("%s", characterTraits[i][j]);
                if (j < numCharFeatures - 1) {
                    printf(", ");
                }
            }
            printf("\n");
        }

        // Save game data AFTER characterTraits are fully populated and BEFORE image generation starts
        if (!save_game_data(gameDataFilePath, selectedTheme, characterFeatures, featureCount, characterTraits, NUM_CHARACTERS)) {
            fprintf(stderr, "Failed to save game data after generation setup. This game's data will not be reusable.\n");
            // This is a non-fatal error for the current game, but means data won't be reusable.
            // For now, let's just log it.
        }

        BatchImageGenData* batch_data = (BatchImageGenData*)malloc(sizeof(BatchImageGenData));
        if (!batch_data) {
            fprintf(stderr, "Failed to allocate memory for batch image generation data.\n");
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            pthread_mutex_unlock(&mutex);
            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        batch_data->num_images = NUM_CHARACTERS;
        batch_data->images_data = (SingleImageGenData*)malloc(NUM_CHARACTERS * sizeof(SingleImageGenData));
        if (!batch_data->images_data) {
            fprintf(stderr, "Failed to allocate memory for images_data array.\n");
            free(batch_data);
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            pthread_mutex_unlock(&mutex);
            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        batch_data->image_dir = imageDirectoryPath; // MODIFIED: Pass image_dir to batch_data

        for (int i = 0; i < NUM_CHARACTERS; ++i) {
            char* prompt;
            if (asprintf(&prompt, "A character that is a %s, %s, %s. Cartoon style.", selectedTheme, characterTraits[i][0], characterTraits[i][1]) == -1) {
                fprintf(stderr, "Failed to construct prompt for character %d\n", i + 1);
                // Clean up already allocated prompts
                for (int k = 0; k < i; ++k) {
                    free(batch_data->images_data[k].prompt);
                }
                free(batch_data->images_data);
                free(batch_data);
                pthread_mutex_lock(&mutex);
                currentGameState = GAME_STATE_EXIT;
                pthread_mutex_unlock(&mutex);
                pthread_mutex_lock(&mutex);
                setup_in_progress = false;
                pthread_mutex_unlock(&mutex);
                return NULL;
            }
            batch_data->images_data[i].prompt = prompt;
            batch_data->images_data[i].character_number = i + 1;
            // batch_data->images_data[i].image_dir is set via batch_data->image_dir
        }

        // Launch the image generation thread
        pthread_mutex_lock(&mutex);
        snprintf(generation_status_message, sizeof(generation_status_message), "Starting image generation...");
        pthread_mutex_unlock(&mutex);
        if (pthread_create(&image_gen_master_thread, NULL, generateImageThread, (void*)batch_data) != 0) {
            fprintf(stderr, "Failed to create master image generation thread.\n");
            // Clean up batch_data and its contents if thread creation fails
            for (int i = 0; i < batch_data->num_images; ++i) {
                free(batch_data->images_data[i].prompt);
            }
            free(batch_data->images_data);
            free(batch_data);
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            pthread_mutex_unlock(&mutex);
            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            return NULL;
        }

        // Assign random character to player
        playerCharacter = rand() % NUM_CHARACTERS;
        printf("\nYou are character number %d\n", playerCharacter + 1);

        // Construct the player character string
        pthread_mutex_lock(&mutex);
        snprintf(playerCharacterString, sizeof(playerCharacterString), "Character %d: %s, %s",
                 playerCharacter + 1, characterTraits[playerCharacter][0], characterTraits[playerCharacter][1]);
        snprintf(characterSelectionText, sizeof(characterSelectionText), "You are character number %d", playerCharacter + 1);
        pthread_mutex_unlock(&mutex);


        // Assign random character to LLM, making sure it's different from the player's
        do {
            llmCharacter = rand() % NUM_CHARACTERS;
        } while (llmCharacter == playerCharacter);

        // Initialize the array of remaining characters
        charactersRemaining = (int*)malloc(NUM_CHARACTERS * sizeof(int));
        if (charactersRemaining == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_EXIT;
            pthread_mutex_unlock(&mutex);
            pthread_mutex_lock(&mutex);
            setup_in_progress = false;
            pthread_mutex_unlock(&mutex);
            return NULL;
        }
        remainingCount = NUM_CHARACTERS;
        for (int i = 0; i < NUM_CHARACTERS; i++) {
            charactersRemaining[i] = i;
        }
    } else {
        printf("No character features found.\n");
        pthread_mutex_lock(&mutex);
        currentGameState = GAME_STATE_EXIT;
        pthread_mutex_unlock(&mutex);
    }

    pthread_mutex_lock(&mutex);
    setup_in_progress = false; // Indicate setup is complete (image gen thread is now running independently)
    pthread_mutex_unlock(&mutex);

    return NULL;
}


int main() {
    // Seed the random number generator
    srand(time(NULL));

    // Initialize Raylib window
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Guess Llama");
    SetTargetFPS(60);

    // Initialize mutex and condition variable
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed.\n");
        return 1;
    }
    if (pthread_cond_init(&regen_cond, NULL) != 0) {
        fprintf(stderr, "Condition variable initialization failed.\n");
        pthread_mutex_destroy(&mutex);
        return 1;
    }

    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Theme input variables
    char theme_input_buffer[100] = {0}; // Buffer for user input theme
    bool themeInputSelected = false;
    Rectangle themeInputBox = {100, 100, 200, 30};
    Rectangle llmThemeButton = {320, 100, 200, 30};
    bool llmThemeSelected = false; // Flag to indicate if LLM theme was chosen

    // Main game loop
    while (!WindowShouldClose() && currentGameState != GAME_STATE_EXIT) {
        pthread_mutex_lock(&mutex);
        GameState state = currentGameState;
        bool current_confirm_regen_prompt_active = confirm_regen_prompt_active;
        pthread_mutex_unlock(&mutex);

        // Handle state transitions for image regeneration prompt
        if (state == GAME_STATE_IMAGE_GENERATION && current_confirm_regen_prompt_active) {
            pthread_mutex_lock(&mutex);
            currentGameState = GAME_STATE_CONFIRM_REGENERATE_IMAGES;
            pthread_mutex_unlock(&mutex);
            state = GAME_STATE_CONFIRM_REGENERATE_IMAGES; // Update local state for current frame
        }

        switch (state) {
            case GAME_STATE_THEME_SELECTION: {
                // Handle input
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    if (CheckCollisionPointRec(GetMousePosition(), themeInputBox)) {
                        themeInputSelected = true;
                    } else {
                        themeInputSelected = false;
                    }
                }

                int key = GetCharPressed();
                while (key > 0) {
                    if (themeInputSelected) {
                        int len = strlen(theme_input_buffer);
                        if (key >= 32 && key <= 125 && len < 99) {
                            theme_input_buffer[len] = (char)key;
                            theme_input_buffer[len + 1] = '\0';
                        }
                    }
                    key = GetCharPressed();  // Check next character in the queue
                }

                if (IsKeyPressed(KEY_BACKSPACE) && themeInputSelected) {
                    int len = strlen(theme_input_buffer);
                    if (len > 0) {
                        theme_input_buffer[len - 1] = '\0';
                    }
                }

                // Transition to THEME_READY state on ENTER or LLM button click
                if (IsKeyPressed(KEY_ENTER) && themeInputSelected) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_THEME_READY;
                    pthread_mutex_unlock(&mutex);
                }

                if (CheckCollisionPointRec(GetMousePosition(), llmThemeButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    llmThemeSelected = true;
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_THEME_READY;
                    pthread_mutex_unlock(&mutex);
                }

                // Drawing
                BeginDrawing();
                ClearBackground(RAYWHITE);

                DrawText("Enter a theme:", 100, 70, 20, GRAY);
                DrawRectangleRec(themeInputBox, LIGHTGRAY);
                DrawText(theme_input_buffer, themeInputBox.x + 5, themeInputBox.y + 8, 20, BLACK);
                if (themeInputSelected) {
                    DrawRectangleLines(themeInputBox.x, themeInputBox.y, themeInputBox.width, themeInputBox.height, BLUE);
                }

                // LLM Theme Button
                DrawRectangleRec(llmThemeButton, ORANGE);
                DrawText("LLM Random Theme", llmThemeButton.x + 5, llmThemeButton.y + 8, 20, BLACK);

                EndDrawing();
                break;
            }
            case GAME_STATE_THEME_READY: {
                // Drawing for theme ready screen
                BeginDrawing();
                ClearBackground(RAYWHITE);
                DrawText("Theme selected!", SCREEN_WIDTH / 2 - MeasureText("Theme selected!", 30) / 2, SCREEN_HEIGHT / 2 - 50, 30, BLACK);
                // Display the chosen theme (either user input or LLM selected)
                // Note: selectedTheme is populated by gameSetupThread, which starts after SPACE is pressed.
                // So, at this exact moment, selectedTheme might still be NULL if LLM theme was chosen.
                // We display theme_input_buffer for user-entered, or a generic message for LLM.
                if (llmThemeSelected) {
                    DrawText("LLM will choose a theme...", SCREEN_WIDTH / 2 - MeasureText("LLM will choose a theme...", 25) / 2, SCREEN_HEIGHT / 2, 25, DARKGRAY);
                } else {
                    DrawText(theme_input_buffer, SCREEN_WIDTH / 2 - MeasureText(theme_input_buffer, 25) / 2, SCREEN_HEIGHT / 2, 25, DARKGRAY);
                }
                DrawText("Press SPACE to continue...", SCREEN_WIDTH / 2 - MeasureText("Press SPACE to continue...", 20) / 2, SCREEN_HEIGHT / 2 + 50, 20, GRAY);
                EndDrawing();

                // Logic to start setup thread
                if (IsKeyPressed(KEY_SPACE) && !setup_in_progress) {
                    // Allocate args for the setup thread
                    SetupThreadArgs* args = (SetupThreadArgs*)malloc(sizeof(SetupThreadArgs));
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

                    if (pthread_create(&gameSetupThreadId, NULL, gameSetupThread, (void*)args) != 0) {
                        fprintf(stderr, "Failed to create game setup thread.\n");
                        free(args);
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_EXIT;
                        setup_in_progress = false;
                        pthread_mutex_unlock(&mutex);
                    } else {
                        pthread_mutex_lock(&mutex);
                        currentGameState = GAME_STATE_IMAGE_GENERATION; // Transition to image generation state
                        pthread_mutex_unlock(&mutex);
                    }
                }
                break;
            }
            case GAME_STATE_IMAGE_GENERATION: {
                pthread_mutex_lock(&mutex);
                bool current_generating_status = generating_images;
                bool current_setup_in_progress = setup_in_progress;
                bool current_confirm_regen_prompt_active_local = confirm_regen_prompt_active; // Local copy
                pthread_mutex_unlock(&mutex);

                // Only transition to playing when both setup and image generation are complete
                // and no regeneration prompt is active (meaning it was handled or not needed)
                if (!current_generating_status && !current_setup_in_progress && !current_confirm_regen_prompt_active_local) {
                    // If gameSetupThread exited early (regen_choice == 0), image_gen_master_thread might not have been created.
                    // Only join if it was created.
                    if (pthread_equal(image_gen_master_thread, (pthread_t)0) == 0) { // Check if thread was created
                        pthread_join(image_gen_master_thread, NULL); // Ensure image gen thread is joined
                    }
                    pthread_join(gameSetupThreadId, NULL); // Ensure setup thread is joined

                    // Load player image from the new directory
                    char player_image_filename[MAX_FILEPATH_BUFFER_SIZE]; // Use MAX_FILEPATH_BUFFER_SIZE
                    snprintf(player_image_filename, sizeof(player_image_filename), "%s/character_%d.png", imageDirectoryPath, playerCharacter + 1);
                    Image playerImage = LoadImage(player_image_filename);
                    if (playerImage.data != NULL) {
                        playerCharacterTexture = LoadTextureFromImage(playerImage);
                        UnloadImage(playerImage);
                        printf("Player character image loaded: %s\n", player_image_filename);
                    } else {
                        fprintf(stderr, "Failed to load player character image: %s\n", player_image_filename);
                    }

                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_PLAYING;
                    pthread_mutex_unlock(&mutex);
                }

                DrawImageGenerationProgressScreen();
                break;
            }
            case GAME_STATE_CONFIRM_REGENERATE_IMAGES: {
                BeginDrawing();
                ClearBackground(RAYWHITE);

                DrawText("Theme directory already exists.", SCREEN_WIDTH / 2 - MeasureText("Theme directory already exists.", 25) / 2, SCREEN_HEIGHT / 2 - 50, 25, BLACK);
                DrawText("Re-create images for this theme?", SCREEN_WIDTH / 2 - MeasureText("Re-create images for this theme?", 25) / 2, SCREEN_HEIGHT / 2 - 20, 25, BLACK);

                Rectangle yesButton = {SCREEN_WIDTH / 2 - 100, SCREEN_HEIGHT / 2 + 30, 80, 30};
                Rectangle noButton = {SCREEN_WIDTH / 2 + 20, SCREEN_HEIGHT / 2 + 30, 80, 30};

                DrawRectangleRec(yesButton, GREEN);
                DrawText("Yes", yesButton.x + 20, yesButton.y + 5, 20, WHITE);
                DrawRectangleRec(noButton, RED);
                DrawText("No", noButton.x + 20, noButton.y + 5, 20, WHITE);

                EndDrawing();

                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    pthread_mutex_lock(&mutex);
                    if (CheckCollisionPointRec(GetMousePosition(), yesButton)) {
                        regen_choice = 1;
                        confirm_regen_prompt_active = false;
                        pthread_cond_signal(&regen_cond); // Signal the setup thread
                        currentGameState = GAME_STATE_IMAGE_GENERATION; // Go back to image generation state
                    } else if (CheckCollisionPointRec(GetMousePosition(), noButton)) {
                        regen_choice = 0;
                        confirm_regen_prompt_active = false;
                        pthread_cond_signal(&regen_cond); // Signal the setup thread
                        currentGameState = GAME_STATE_IMAGE_GENERATION; // Go back to image generation state
                    }
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }
            case GAME_STATE_PLAYING: {
                // Drawing
                BeginDrawing();
                ClearBackground(RAYWHITE);

                DrawText(playerCharacterString, 10, 10, 20, BLACK);
                Rectangle startGuessingButton = {10, 70, 250, 30};
                DrawRectangleRec(startGuessingButton, BLUE);
                DrawText("Start Guessing Round", startGuessingButton.x + 5, startGuessingButton.y + 8, 20, WHITE);

                if (CheckCollisionPointRec(GetMousePosition(), startGuessingButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    // MODIFIED: Pass playerCharacterTexture and playerCharacter to llmGuessingRound
                    llmGuessingRound(characterTraits, llmCharacter, selectedTheme, NUM_CHARACTERS, charactersRemaining, &remainingCount, playerCharacterTexture, playerCharacter);
                }

                EndDrawing();
                break;
            }
            case GAME_STATE_PLAYER_WINS: {
                BeginDrawing();
                ClearBackground(RAYWHITE);
                DrawText("YOU WIN!", SCREEN_WIDTH / 2 - MeasureText("YOU WIN!", 40) / 2, SCREEN_HEIGHT / 2 - 20, 40, GREEN);
                DrawText("The LLM eliminated your character!", SCREEN_WIDTH / 2 - MeasureText("The LLM eliminated your character!", 20) / 2, SCREEN_HEIGHT / 2 + 20, 20, DARKGRAY);
                DrawText("Press ESC to exit", SCREEN_WIDTH / 2 - MeasureText("Press ESC to exit", 20) / 2, SCREEN_HEIGHT / 2 + 60, 20, GRAY);
                EndDrawing();

                if (IsKeyPressed(KEY_ESCAPE)) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_EXIT;
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }
            case GAME_STATE_LLM_WINS: {
                // Not implemented yet, but good to have the state
                BeginDrawing();
                ClearBackground(RAYWHITE);
                DrawText("LLM WINS!", SCREEN_WIDTH / 2 - MeasureText("LLM WINS!", 40) / 2, SCREEN_HEIGHT / 2 - 20, 40, RED);
                DrawText("Press ESC to exit", SCREEN_WIDTH / 2 - MeasureText("Press ESC to exit", 20) / 2, SCREEN_HEIGHT / 2 + 30, 20, GRAY);
                EndDrawing();

                if (IsKeyPressed(KEY_ESCAPE)) {
                    pthread_mutex_lock(&mutex);
                    currentGameState = GAME_STATE_EXIT;
                    pthread_mutex_unlock(&mutex);
                }
                break;
            }
            case GAME_STATE_EXIT: {
                // Clean up and exit
                break;
            }
        }
    }

    // Free all allocated memory before closing
    if (selectedTheme) free(selectedTheme);
    if (characterFeatures) {
        for (int i = 0; i < featureCount; i++) {
            free(characterFeatures[i]);
        }
        free(characterFeatures);
    }
    if (characterTraits) {
        for (int i = 0; i < NUM_CHARACTERS; ++i) {
            if (characterTraits[i]) {
                for (int j = 0; j < 2; ++j) { // Each character has 2 features
                    if (characterTraits[i][j]) free(characterTraits[i][j]);
                }
                free(characterTraits[i]);
            }
        }
        free(characterTraits);
    }
    if (charactersRemaining) free(charactersRemaining);

    if (playerCharacterTexture.id != 0) {
        UnloadTexture(playerCharacterTexture);
    }

    CloseWindow(); // Close window and OpenGL context
    pthread_mutex_destroy(&mutex); // Destroy mutex
    pthread_cond_destroy(&regen_cond); // Destroy condition variable
    curl_global_cleanup(); // Cleanup curl globally

    return 0;
}
