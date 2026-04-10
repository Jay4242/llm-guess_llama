#include "guess_llama.h"

typedef struct {
    char* data;
    size_t size;
} ResponseData;

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    ResponseData* data = (ResponseData*)userdata;
    size_t chunk_size = size * nmemb;
    size_t new_size = data->size + chunk_size;
    char* resized_data = realloc(data->data, new_size + 1);

    if (!resized_data) {
        fprintf(stderr, "realloc() failed\n");
        return 0;
    }

    data->data = resized_data;
    memcpy(data->data + data->size, ptr, chunk_size);
    data->data[new_size] = '\0';
    data->size = new_size;
    return chunk_size;
}

static char* make_http_post(const char* url, const char* data) {
    CURL* curl = curl_easy_init();
    CURLcode res;
    ResponseData response_data = {NULL, 0};
    struct curl_slist* headers = NULL;

    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 14400L);

    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = curl_slist_append(headers, "Cache-Control: no-cache");
    headers = curl_slist_append(headers, "Connection: keep-alive");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Pragma: no-cache");
    headers = curl_slist_append(
        headers,
        "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36"
    );
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(response_data.data);
        response_data.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response_data.data;
}

static unsigned char* base64_decode(const char* data, size_t data_len, size_t* output_len) {
    size_t padding = 0;
    unsigned char* decoded_data;
    int val = 0;
    int valb = -8;
    size_t out_index = 0;

    if (data_len == 0) {
        *output_len = 0;
        return NULL;
    }

    if (data_len > 0 && data[data_len - 1] == '=') {
        padding++;
    }
    if (data_len > 1 && data[data_len - 2] == '=') {
        padding++;
    }
    *output_len = (data_len * 3) / 4 - padding;

    decoded_data = malloc(*output_len);
    if (!decoded_data) {
        fprintf(stderr, "malloc() failed\n");
        return NULL;
    }

    for (size_t i = 0; i < data_len; ++i) {
        const char* table_position;
        unsigned char c = (unsigned char)data[i];

        if (c == '=') {
            break;
        }

        table_position = strchr(b64_table, c);
        if (!table_position) {
            continue;
        }

        val = (val << 6) | (int)(table_position - b64_table);
        valb += 6;
        if (valb >= 0) {
            decoded_data[out_index++] = (unsigned char)((val >> valb) & 0xFF);
            valb -= 8;
        }
    }

    return decoded_data;
}

static int save_image_from_base64(const char* b64_data, size_t b64_len, const char* filename) {
    size_t decoded_size;
    unsigned char* decoded_data = base64_decode(b64_data, b64_len, &decoded_size);
    FILE* file;

    if (!decoded_data) {
        fprintf(stderr, "Base64 decoding failed.\n");
        return 1;
    }

    file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Failed to open file %s for writing.\n", filename);
        free(decoded_data);
        return 1;
    }

    fwrite(decoded_data, 1, decoded_size, file);
    fclose(file);
    free(decoded_data);

    printf("Saved image to %s\n", filename);
    return 0;
}

int generate_character_image(const char* prompt, int character_number, const char* image_dir) {
    char url[256];
    char filename[MAX_FILEPATH_BUFFER_SIZE];
    char* data;
    char* response;
    json_error_t error;
    json_t* root;
    json_t* data_json;
    json_t* item;
    json_t* b64_json;
    const char* b64_data;
    int snprintf_result;
    int result;

    printf("Generating image for character %d with prompt: %s\n", character_number, prompt);
    snprintf(url, sizeof(url), "http://%s/v1/images/generations", server_url);

    data = malloc(4096);
    if (!data) {
        fprintf(stderr, "Failed to allocate memory for data.\n");
        return 1;
    }

    snprintf_result = snprintf(
        data,
        4096,
        "{"
        "\"prompt\": \"%s\", "
        "\"n\": 1, "
        "\"size\": \"512x512\", "
        "\"output_format\": \"png\""
        "}",
        prompt
    );
    if (snprintf_result < 0 || snprintf_result >= 4096) {
        fprintf(stderr, "Error creating data string (truncation detected) %d.\n", snprintf_result);
        free(data);
        return 1;
    }

    pthread_mutex_lock(&mutex);
    {
        int total = total_characters_to_generate > 0 ? total_characters_to_generate : NUM_CHARACTERS;
        int base_percent = (current_image_index * 100) / total;
        snprintf(current_percent, sizeof(current_percent), "%d%%", base_percent);
    }
    pthread_mutex_unlock(&mutex);

    if (is_openrouter_server_url(server_url)) {
        result = generate_character_image_openrouter(prompt, character_number, image_dir);
        free(data);
        if (result == 0) {
            pthread_mutex_lock(&mutex);
            {
                int total = total_characters_to_generate > 0 ? total_characters_to_generate : NUM_CHARACTERS;
                int completed_percent = ((current_image_index + 1) * 100) / total;
                snprintf(current_percent, sizeof(current_percent), "%d%%", completed_percent);
            }
            pthread_mutex_unlock(&mutex);
            printf("Successfully generated image for character %d\n", character_number);
        }
        return result;
    }

    response = make_http_post(url, data);
    free(data);
    if (!response) {
        fprintf(stderr, "Failed to get response from server.\n");
        return 1;
    }

    printf("Response received for character %d\n", character_number);

    root = json_loads(response, 0, &error);
    free(response);
    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return 1;
    }

    data_json = json_object_get(root, "data");
    if (!data_json || !json_is_array(data_json) || json_array_size(data_json) == 0) {
        fprintf(stderr, "Unexpected response format: no usable 'data' array found.\n");
        json_decref(root);
        return 1;
    }

    item = json_array_get(data_json, 0);
    b64_json = json_object_get(item, "b64_json");
    if (!b64_json || !json_is_string(b64_json)) {
        fprintf(stderr, "Missing b64_json in response.\n");
        json_decref(root);
        return 1;
    }

    b64_data = json_string_value(b64_json);
    snprintf(filename, sizeof(filename), "%s/character_%d.png", image_dir, character_number);
    result = save_image_from_base64(b64_data, strlen(b64_data), filename);
    json_decref(root);

    if (result == 0) {
        pthread_mutex_lock(&mutex);
        {
            int total = total_characters_to_generate > 0 ? total_characters_to_generate : NUM_CHARACTERS;
            int completed_percent = ((current_image_index + 1) * 100) / total;
            snprintf(current_percent, sizeof(current_percent), "%d%%", completed_percent);
        }
        pthread_mutex_unlock(&mutex);
        printf("Successfully generated image for character %d\n", character_number);
    }

    return result;
}

void* generateImageThread(void* arg) {
    BatchImageGenData* batch_data = (BatchImageGenData*)arg;

    pthread_mutex_lock(&mutex);
    generating_images = true;
    total_characters_to_generate = batch_data->num_images;
    characters_generated_count = 0;
    pthread_mutex_unlock(&mutex);

    for (int i = 0; i < batch_data->num_images; ++i) {
        SingleImageGenData* current_image_data = &batch_data->images_data[i];

        pthread_mutex_lock(&mutex);
        characters_generated_count = i + 1;
        current_image_index = i;
        snprintf(
            generation_status_message,
            sizeof(generation_status_message),
            "Generating image %d of %d...",
            characters_generated_count,
            total_characters_to_generate
        );
        pthread_mutex_unlock(&mutex);

        generate_character_image(
            current_image_data->prompt,
            current_image_data->character_number,
            batch_data->image_dir
        );

        free(current_image_data->prompt);
    }

    pthread_mutex_lock(&mutex);
    generating_images = false;
    snprintf(generation_status_message, sizeof(generation_status_message), "Image generation complete!");
    snprintf(current_percent, sizeof(current_percent), "100%%");
    current_image_index = 0;
    pthread_mutex_unlock(&mutex);

    free(batch_data->images_data);
    free(batch_data);
    return NULL;
}
