#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <time.h>
#include <pthread.h>
#include <raylib.h>
#include <curl/curl.h>
#include <time.h>
#include <unistd.h>
#include <jansson.h>

// Define screen dimensions
#define SCREEN_WIDTH 800
#define SCREEN_HEIGHT 600

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
pthread_t image_gen_master_thread; // Declare the global thread variable here

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

    curl_global_init(CURL_GLOBAL_DEFAULT);
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
            curl_global_cleanup();
            return NULL;
        }

        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();

    return response_data.data;
}

// Function to make an HTTP POST request using libcurl
char* make_http_post(const char* url, const char* data) {
    CURL* curl;
    CURLcode res;
    ResponseData response_data = {NULL, 0};
    struct curl_slist *headers = NULL;

    curl_global_init(CURL_GLOBAL_DEFAULT);
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
            curl_global_cleanup();
            if (headers) curl_slist_free_all(headers);
            return NULL;
        }

        curl_easy_cleanup(curl);
    }
    curl_global_cleanup();
    if (headers) curl_slist_free_all(headers);

    return response_data.data;
}

// Function to make a request to the LLM API
char* getLLMResponse(const char* prompt, double temperature) {
    CURL* curl;
    CURLcode res;
    ResponseData response = {NULL, 0};

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {
        // Construct the URL
        char* url;
        if (asprintf(&url, "%s/v1/chat/completions", llmServerAddress) == -1) {
            fprintf(stderr, "Failed to construct URL\n");
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            return NULL;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);

        // Prepare the JSON payload
        char* data;
        if (asprintf(&data, "{\"model\": \"llama-3.2-3b-it-q8_0\", \"messages\": [{\"role\": \"system\", \"content\": \"You are a helpful assistant.\"}, {\"role\": \"user\", \"content\": \"%s\"}], \"temperature\": %f}", prompt, temperature) == -1) {
            fprintf(stderr, "Failed to construct JSON payload\n");
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            free(url);
            return NULL;
        }

        // Set the request type to POST
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        // Set the POST data
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);

        // Set the data length
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(data));

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
            curl_global_cleanup();
            free(url);
            return NULL;
        }

        // Always cleanup
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        free(url);
    } else {
        curl_global_cleanup();
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }
    curl_global_cleanup();

    return response.data;
}


// Adapted generate_image function for character generation
int generate_character_image(const char* prompt, int character_number) {
    char render_url[256];
    snprintf(render_url, sizeof(render_url), "http://%s/render", server_url);
    char* data = malloc(4096);
    if (!data) {
        fprintf(stderr, "Failed to allocate memory for data.\n");
        return 1;
    }
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
                                  "\"height\": 768, "
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
                                  "\"save_to_disk_path\": \"/home/%s/Pictures/stable-diffusion/output/\", "
                                  "\"use_lora_model\": [], "
                                  "\"lora_alpha\": [], "
                                  "\"enable_vae_tiling\": false, "
                                  "\"scheduler_name\": \"automatic\", "
                                  "\"session_id\": \"1337\""
                                  "} ",
                                  prompt, get_random_seed(), prompt, username);
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

    char* status = strdup("pending");

    while (strcmp(status, "completed") != 0) {
        if (strcmp(status, "error") == 0) {
            fprintf(stderr, "Error occurred during rendering.\n");
            if (task_str) free(task_str);
            return 1;
        }

        if (strcmp(status, "Online") == 0) {
            printf("Status is Online, generation complete.\n");
            break;
        }

        sleep(5);

        // Get the status
        char* ping_response = make_http_get(ping_url);
        if (!ping_response) {
            fprintf(stderr, "Failed to get ping response from server.\n");
            if (task_str) free(task_str);
            free(status);
            return 1;
        }

        // Debug: Print the raw ping response
        //printf("Raw Ping Response: %s\n", ping_response);

        // Extract the status string
        char* status_start = strstr(ping_response, "\"status\":\"");
        if (status_start != NULL) {
            status_start += strlen("\"status\":\"");
            char* status_end = strchr(status_start, '"');
            if (status_end != NULL) {
                // Calculate the length of the status string
                size_t status_len = status_end - status_start;

                // Allocate memory for the status string
                char* extracted_status = (char*)malloc(status_len + 1);
                if (extracted_status == NULL) {
                    fprintf(stderr, "Failed to allocate memory for status.\n");
                    free(ping_response);
                    if (task_str) free(task_str);
                    free(status);
                    return 1;
                }

                // Copy the status string
                strncpy(extracted_status, status_start, status_len);
                extracted_status[status_len] = '\0';

                // Use the extracted status
                free(status);
                status = extracted_status;
                printf("Extracted Status: %s\n", status);
            } else {
                printf("Could not find end quote for status.\n");
            }
        } else {
            printf("Could not find status field in ping response.\n");
        }
        free(ping_response);

        // Get the stream data
        char* stream_response = make_http_get(image_url);
        if (!stream_response) {
            fprintf(stderr, "Failed to get stream response from server.\n");
            if (task_str) free(task_str);
            free(status);
            return 1;
        }

        // Split the stream response into individual JSON objects
        char* stream_data_str = strtok(stream_response, "}{");
        while (stream_data_str != NULL) {
            // Add back the missing brackets
            char json_string[2048];
            snprintf(json_string, sizeof(json_string), "%s%s%s",
                     stream_data_str[0] == '{' ? "" : "{",
                     stream_data_str,
                     stream_data_str[strlen(stream_data_str) - 1] == '}' ? "" : "}");

            // Process the stream data as a single JSON object
            json_error_t stream_error;
            json_t* stream_data = json_loads(json_string, 0, &stream_error);
            if (!stream_data) {
                stream_data_str = strtok(NULL, "}{");
                continue;
            }

            json_t* steps_json = json_object_get(stream_data, "step");
            json_t* total_steps_json = json_object_get(stream_data, "total_steps");

            if (steps_json && total_steps_json && json_is_number(steps_json) && json_is_number(total_steps_json)) {
                int steps = json_integer_value(steps_json);
                int total_steps = json_integer_value(total_steps_json);
                if (total_steps > 0) { // Prevent division by zero
                    float percentage = (float)steps / total_steps * 100;
                    pthread_mutex_lock(&mutex);
                    snprintf(current_percent, sizeof(current_percent), "%.0f%%", percentage);
                    pthread_mutex_unlock(&mutex);
                }
            }

            json_decref(stream_data);
            stream_data_str = strtok(NULL, "}{");
        }
        free(stream_response);

        printf("Task Status: %s, Task: %s, Prompt: %s, Percent Done: %s\n",
               status, task, prompt, current_percent);
    }

    // Get the final image
    char* final_stream_response = make_http_get(image_url);
    if (!final_stream_response) {
        fprintf(stderr, "Failed to get final stream response from server.\n");
        if (task_str) free(task_str);
        free(status);
        return 1;
    }

    // Find the start of the base64 image data
    char* data_start = strstr(final_stream_response, "\"data\":\"data:image/png;base64,");
    if (!data_start) {
        fprintf(stderr, "Image data not found in JSON response.\n");
        free(final_stream_response);
	if (task_str) free(task_str);
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
        return 1;
    }

    // Free the base64 encoded data
    free(image_data_base64);

    // Save the decoded image data to a file
    char filename[64];
    snprintf(filename, sizeof(filename), "character_%d.png", character_number);
    FILE* fp = fopen(filename, "wb");
    if (fp) {
        fwrite(decoded_data, 1, decoded_size, fp);
        fclose(fp);
        printf("Image saved to %s\n", filename);
    } else {
        fprintf(stderr, "Failed to save image to file.\n");
    }

    free(decoded_data);
    if (task_str) free(task_str);
    free(status);
    return 0;
}

// Struct to hold data for a single image generation task
typedef struct {
    char* prompt;
    int character_number;
} SingleImageGenData;

// Struct to hold data for batch image generation
typedef struct {
    SingleImageGenData* images_data; // Array of prompts and character numbers
    int num_images;
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
        current_percent[0] = '\0'; // Clear percentage
        pthread_mutex_unlock(&mutex);

        SingleImageGenData* current_image_data = &batch_data->images_data[i];
        generate_character_image(current_image_data->prompt, current_image_data->character_number);

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

    const char* prompt = "Suggest 10 themes for a 'Guess Who?' game. The theme should be who the characters in the game are. For example: clowns, shih-tzu dogs, penguins, llamas, etc. Feel free to be creative and random. Return a JSON list of strings, only the themes and nothing else.";
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

        const char* contentStr = json_string_value(content);

        // Strip ```json and ``` from the content string
        char* contentStrStripped = strdup(contentStr);
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

        const char* contentStr = json_string_value(content);

        // Add debug logging: print the content string
        //printf("Content string: %s\n", contentStr);

        // Strip ```json and ``` from the content string
        char* contentStrStripped = strdup(contentStr);
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
void llmGuessingRound(char*** characterTraits, int llmCharacter, const char* theme, int numCharacters, int* charactersRemaining, int* remainingCount) {
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
            return;
        }

        for (int j = 0; j < 2; ++j) { // Assuming each character has 2 features
            if (characterTraits[i] != NULL && characterTraits[i][j] != NULL) {
                char* temp = NULL;
                if (asprintf(&temp, "%s%s%s", characterString, characterTraits[i][j], (j < 1) ? ", " : "") == -1) {
                    fprintf(stderr, "Failed to append feature to character string\n");
                    free(characterString);
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
    if (asprintf(&questionPrompt, "Given the theme '%s' and the following list of characters and their traits: %s Formulate a yes/no question that will help you narrow down the possibilities. The question should be about a single trait from the list of character features. Return the question as a string, only the question and nothing else.", theme, characterList ? characterList : "") == -1) {
        fprintf(stderr, "Failed to construct question prompt\n");
        free(characterList);
        return;
    }

    // Print the prompt before sending it to the LLM
    printf("Prompt sent to LLM:\n%s\n", questionPrompt);

    double temperature = 0.7;
    char* llmQuestionResponse = getLLMResponse(questionPrompt, temperature);
    free(questionPrompt);

    if (llmQuestionResponse != NULL) {
        // Parse the JSON response
        json_error_t error;
        json_t* root = json_loads(llmQuestionResponse, 0, &error);

        if (!root) {
            fprintf(stderr, "Error parsing JSON: %s\n", error.text);
            free(llmQuestionResponse);
            return;
        }

        json_t* choices = json_object_get(root, "choices");
        if (!json_is_array(choices) || json_array_size(choices) == 0) {
            fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
            fprintf(stderr, "Raw LLM Response: %s\n", llmQuestionResponse); // Print the raw response for debugging
            json_decref(root);
            free(llmQuestionResponse);
            return;
        }

        json_t* firstChoice = json_array_get(choices, 0);
        if (!json_is_object(firstChoice)) {
            fprintf(stderr, "Error: First choice is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return;
        }

        json_t* message = json_object_get(firstChoice, "message");
        if (!json_is_object(message)) {
            fprintf(stderr, "Error: 'message' is not an object.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return;
        }

        json_t* content = json_object_get(message, "content");
        if (!json_is_string(content)) {
            fprintf(stderr, "Error: 'content' is not a string.\n");
            json_decref(root);
            free(llmQuestionResponse);
            return;
        }

        const char* question = json_string_value(content);

        // Ask the question to the user (using Raylib GUI)
        printf("LLM asks: %s\n", question);
        setYesNoInput(question); // Set the question for the GUI

        // Wait for the user to answer
        while (currentAnswer == -1 && !WindowShouldClose()) {
            BeginDrawing();
            ClearBackground(RAYWHITE);

            // Draw player character string
            DrawText(playerCharacterString, 10, 10, 20, BLACK);

            // Draw the question
            DrawText(currentQuestion, 100, 70, 20, GRAY);

            // Draw "Yes" and "No" buttons
            Rectangle yesButton = {100, 120, 80, 30};
            Rectangle noButton = {200, 120, 80, 30};
            DrawRectangleRec(yesButton, GREEN);
            DrawRectangleRec(noButton, RED);
            DrawText("Yes", yesButton.x + 20, yesButton.y + 5, 20, WHITE);
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
            return;
        }

        // Construct the prompt for the LLM to determine which characters to eliminate
        char* eliminationPrompt;
        const char* answerString = currentAnswer ? "yes" : "no";
        if (asprintf(&eliminationPrompt, "Given the theme '%s', the question '%s' was asked, and the answer was '%s'.  Given the following list of characters and their traits: %s Which characters should be eliminated? Return a JSON list of integers, only the character numbers and nothing else.", theme, question, answerString, characterList ? characterList : "") == -1) {
            fprintf(stderr, "Failed to construct elimination prompt\n");
            free(characterList);
            json_decref(root);
            free(llmQuestionResponse);
            return;
        }
        free(characterList);

        printf("Elimination Prompt sent to LLM:\n%s\n", eliminationPrompt);

        char* llmEliminationResponse = getLLMResponse(eliminationPrompt, temperature);
        free(eliminationPrompt);

        if (llmEliminationResponse != NULL) {
            // Add debug logging: Print the raw elimination response
            printf("Raw LLM Elimination Response: %s\n", llmEliminationResponse);

            // Parse the JSON response for the elimination
            json_t* rootElimination = json_loads(llmEliminationResponse, 0, &error);
            if (!rootElimination) {
                fprintf(stderr, "Error parsing JSON for elimination: %s\n", error.text);
                free(llmEliminationResponse);
                return;
            }

            json_t* choicesElimination = json_object_get(rootElimination, "choices");
            if (!json_is_array(choicesElimination) || json_array_size(choicesElimination) == 0) {
                fprintf(stderr, "Error: 'choices' is not a non-empty array for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                return;
            }

            json_t* firstChoiceElimination = json_array_get(choicesElimination, 0);
            if (!json_is_object(firstChoiceElimination)) {
                fprintf(stderr, "Error: First choice is not an object for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                return;
            }

            json_t* messageElimination = json_object_get(firstChoiceElimination, "message");
            if (!json_is_object(messageElimination)) {
                fprintf(stderr, "Error: 'message' is not an object for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                return;
            }

            json_t* contentElimination = json_object_get(messageElimination, "content");
            if (!json_is_string(contentElimination)) {
                fprintf(stderr, "Error: 'content' is not a string for elimination.\n");
                json_decref(rootElimination);
                free(llmEliminationResponse);
                return;
            }

            const char* contentStrElimination = json_string_value(contentElimination);

            // Strip ```json and ``` from the content string
            char* contentStrStrippedElimination = strdup(contentStrElimination);
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
            return;
        } else {
            fprintf(stderr, "Failed to get elimination response from LLM\n");
        }

        // Cleanup
        json_decref(root);
        free(llmQuestionResponse);
    } else {
        fprintf(stderr, "Failed to get response from LLM\n");
    }
}

// Function to clear the screen and redraw the background
void clearScreen() {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    EndDrawing();
}

int main() {
    // Seed the random number generator
    srand(time(NULL));

    // Initialize Raylib window
    InitWindow(SCREEN_WIDTH, SCREEN_HEIGHT, "Guess Llama");
    SetTargetFPS(60);

    // Initialize mutex
    if (pthread_mutex_init(&mutex, NULL) != 0) {
        fprintf(stderr, "Mutex initialization failed.\n");
        return 1;
    }

    // Theme input variables
    char theme[100] = {0};
    bool themeEntered = false;
    Rectangle themeInputBox = {100, 100, 200, 30};
    bool themeInputSelected = false;
    Rectangle llmThemeButton = {320, 100, 200, 30};
    bool llmThemeSelected = false;

    // Main game loop for theme selection
    while (!WindowShouldClose() && !themeEntered && !llmThemeSelected) {
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
                int len = strlen(theme);
                if (key >= 32 && key <= 125 && len < 99) {
                    theme[len] = (char)key;
                    theme[len + 1] = '\0';
                }
            }
            key = GetCharPressed();  // Check next character in the queue
        }

        if (IsKeyPressed(KEY_BACKSPACE) && themeInputSelected) {
            int len = strlen(theme);
            if (len > 0) {
                theme[len - 1] = '\0';
            }
        }

        if (IsKeyPressed(KEY_ENTER) && themeInputSelected) {
            themeEntered = true;
        }

        // Check for LLM theme selection
        if (CheckCollisionPointRec(GetMousePosition(), llmThemeButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            llmThemeSelected = true;
        }

        // Drawing
        BeginDrawing();
        ClearBackground(RAYWHITE);

        DrawText("Enter a theme:", 100, 70, 20, GRAY);
        DrawRectangleRec(themeInputBox, LIGHTGRAY);
        DrawText(theme, themeInputBox.x + 5, themeInputBox.y + 8, 20, BLACK);
        if (themeInputSelected) {
            DrawRectangleLines(themeInputBox.x, themeInputBox.y, themeInputBox.width, themeInputBox.height, BLUE);
        }

        // LLM Theme Button
        DrawRectangleRec(llmThemeButton, ORANGE);
        DrawText("LLM Random Theme", llmThemeButton.x + 5, llmThemeButton.y + 8, 20, BLACK);

        if (themeEntered) {
            DrawText("Press SPACE to continue", 100, 150, 20, GRAY);
        }

        EndDrawing();

        if (themeEntered && IsKeyPressed(KEY_SPACE)) {
            break; // Exit the theme input loop
        }
    }

    // Clear the screen before starting the game logic
    clearScreen();

    // Game logic starts here after theme is entered
    char* selectedTheme;
    if (llmThemeSelected) {
        // Get a random theme from the LLM
        int themeCount;
        char** themes = getThemesFromLLM(&themeCount);
        if (themes != NULL && themeCount > 0) {
            // Select a random theme from the list
            int randomIndex = rand() % themeCount;
            selectedTheme = strdup(themes[randomIndex]);
            printf("LLM selected theme: %s\n", selectedTheme);

            // Free the memory allocated for the themes
            for (int i = 0; i < themeCount; i++) {
                free(themes[i]);
            }
            free(themes);
        } else {
            // If no themes are returned, use a default theme
            selectedTheme = strdup("Default");
            printf("Using default theme: Default\n");
        }
    } else {
        selectedTheme = strdup(theme);
        printf("Using theme: %s\n", selectedTheme);
    }

    // Get character features based on the theme
    int featureCount;
    char** characterFeatures = getCharacterFeatures(selectedTheme, &featureCount);

    if (characterFeatures != NULL && featureCount > 0) {
        printf("Character features:\n");
        for (int i = 0; i < featureCount; i++) {
            printf("- %s\n", characterFeatures[i]);
        }

        // Assign 2-3 random features to each of the 24 characters
        int numCharacters = 24;

        // Allocate memory for character traits
        char*** characterTraits = (char***)malloc(numCharacters * sizeof(char**));
        if (characterTraits == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            return 1;
        }

        for (int i = 0; i < numCharacters; ++i) {
            int numCharFeatures = 2; // Each character has 2 features
            characterTraits[i] = (char**)malloc(numCharFeatures * sizeof(char*));

            if (characterTraits[i] == NULL) {
                fprintf(stderr, "Memory allocation failed\n");
                return 1;
            }

            for (int j = 0; j < numCharFeatures; ++j) {
                int featureIndex = rand() % featureCount;
                characterTraits[i][j] = strdup(characterFeatures[featureIndex]); // Assign the feature to the character
            }
        }

        printf("\nCharacter Traits:\n");
        for (int i = 0; i < numCharacters; ++i) {
            printf("Character %d: ", i + 1);
            int numCharFeatures = 2; // Each character has 2 features
            for (int j = 0; j < numCharFeatures; ++j) {
                printf("%s", characterTraits[i][j]);
                if (j < numCharFeatures - 1) {
                    printf(", ");
                }
            }
            printf("\n");
        }

        // Prepare data for the single image generation thread
        BatchImageGenData* batch_data = (BatchImageGenData*)malloc(sizeof(BatchImageGenData));
        if (!batch_data) {
            fprintf(stderr, "Failed to allocate memory for batch image generation data.\n");
            // Free characterTraits and characterFeatures before returning
            for (int i = 0; i < numCharacters; ++i) {
                for (int j = 0; j < 2; ++j) free(characterTraits[i][j]);
                free(characterTraits[i]);
            }
            free(characterTraits);
            for (int i = 0; i < featureCount; i++) free(characterFeatures[i]);
            free(characterFeatures);
            free(selectedTheme);
            return 1;
        }
        batch_data->num_images = numCharacters;
        batch_data->images_data = (SingleImageGenData*)malloc(numCharacters * sizeof(SingleImageGenData));
        if (!batch_data->images_data) {
            fprintf(stderr, "Failed to allocate memory for images_data array.\n");
            free(batch_data);
            // Free characterTraits and characterFeatures before returning
            for (int i = 0; i < numCharacters; ++i) {
                for (int j = 0; j < 2; ++j) free(characterTraits[i][j]);
                free(characterTraits[i]);
            }
            free(characterTraits);
            for (int i = 0; i < featureCount; i++) free(characterFeatures[i]);
            free(characterFeatures);
            free(selectedTheme);
            return 1;
        }

        for (int i = 0; i < numCharacters; ++i) {
            char* prompt;
            if (asprintf(&prompt, "A character that is a %s, %s, %s. Cartoon style.", selectedTheme, characterTraits[i][0], characterTraits[i][1]) == -1) {
                fprintf(stderr, "Failed to construct prompt for character %d\n", i + 1);
                // Free previously allocated prompts in batch_data->images_data
                for (int j = 0; j < i; ++j) free(batch_data->images_data[j].prompt);
                free(batch_data->images_data);
                free(batch_data);
                // Free characterTraits and characterFeatures before returning
                for (int k = 0; k < numCharacters; ++k) {
                    for (int l = 0; l < 2; ++l) free(characterTraits[k][l]);
                    free(characterTraits[k]);
                }
                free(characterTraits);
                for (int k = 0; k < featureCount; k++) free(characterFeatures[k]);
                free(characterFeatures);
                free(selectedTheme);
                return 1;
            }
            batch_data->images_data[i].prompt = prompt;
            batch_data->images_data[i].character_number = i + 1;
        }

        // Launch the single image generation thread
        if (pthread_create(&image_gen_master_thread, NULL, generateImageThread, (void*)batch_data) != 0) {
            fprintf(stderr, "Failed to create master image generation thread.\n");
            // Free all prompts and batch_data
            for (int i = 0; i < numCharacters; ++i) free(batch_data->images_data[i].prompt);
            free(batch_data->images_data);
            free(batch_data);
            // Free characterTraits and characterFeatures before returning
            for (int i = 0; i < numCharacters; ++i) {
                for (int j = 0; j < 2; ++j) free(characterTraits[i][j]);
                free(characterTraits[i]);
            }
            free(characterTraits);
            for (int i = 0; i < featureCount; i++) free(characterFeatures[i]);
            free(characterFeatures);
            free(selectedTheme);
            return 1;
        }

        // Assign random character to player
        int playerCharacter = rand() % numCharacters;
        printf("\nYou are character number %d\n", playerCharacter + 1);

        // Construct the player character string
        snprintf(playerCharacterString, sizeof(playerCharacterString), "Character %d: %s, %s",
                 playerCharacter + 1, characterTraits[playerCharacter][0], characterTraits[playerCharacter][1]);
        snprintf(characterSelectionText, sizeof(characterSelectionText), "You are character number %d", playerCharacter + 1);

        // Assign random character to LLM, making sure it's different from the player's
        int llmCharacter;
        do {
            llmCharacter = rand() % numCharacters;
        } while (llmCharacter == playerCharacter);

        // Initialize the array of remaining characters
        int* charactersRemaining = (int*)malloc(numCharacters * sizeof(int));
        if (charactersRemaining == NULL) {
            fprintf(stderr, "Memory allocation failed\n");
            return 1;
        }
        int remainingCount = numCharacters;
        for (int i = 0; i < numCharacters; i++) {
            charactersRemaining[i] = i;
        }

        // Main game loop (after image generation starts)
        while (!WindowShouldClose()) {
            // Drawing
            BeginDrawing();
            ClearBackground(RAYWHITE);

            pthread_mutex_lock(&mutex);
            if (generating_images) {
                DrawText(generation_status_message, 10, 10, 20, BLACK);
                DrawText(current_percent, 10, 40, 20, BLACK);
            } else {
                // Once generation is complete, show player character info and game elements
                DrawText(playerCharacterString, 10, 10, 20, BLACK);
                // Example: Draw a button to start the LLM guessing round
                Rectangle startGuessingButton = {10, 70, 200, 30};
                DrawRectangleRec(startGuessingButton, BLUE);
                DrawText("Start Guessing Round", startGuessingButton.x + 5, startGuessingButton.y + 8, 20, WHITE);

                if (CheckCollisionPointRec(GetMousePosition(), startGuessingButton) && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    llmGuessingRound(characterTraits, llmCharacter, selectedTheme, numCharacters, charactersRemaining, &remainingCount);
                }
            }
            pthread_mutex_unlock(&mutex);

            EndDrawing();
        }

        // After Raylib loop, wait for the master image generation thread to finish
        pthread_join(image_gen_master_thread, NULL);

        // Free memory
        for (int i = 0; i < numCharacters; ++i) {
            int numCharFeatures = 2; // Each character has 2 features
            for (int j = 0; j < numCharFeatures; ++j) {
                free(characterTraits[i][j]);
            }
            free(characterTraits[i]);
        }
        free(characterTraits);

        // Free character features
        for (int i = 0; i < featureCount; i++) {
            free(characterFeatures[i]);
        }
        free(characterFeatures);
        free(charactersRemaining);
    } else {
        printf("No character features found.\n");
    }

    free(selectedTheme);
    CloseWindow(); // Close window and OpenGL context

    // Destroy mutex
    pthread_mutex_destroy(&mutex);

    return 0;
}
