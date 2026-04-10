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

    if (data[data_len - 1] == '=') {
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

static bool string_contains_openrouter(const char* value) {
    const char* target = "openrouter.ai";
    size_t target_len = strlen(target);

    if (!value) {
        return false;
    }

    for (const char* cursor = value; *cursor != '\0'; ++cursor) {
        size_t match_index = 0;
        while (match_index < target_len &&
               cursor[match_index] != '\0' &&
               tolower((unsigned char)cursor[match_index]) == target[match_index]) {
            ++match_index;
        }
        if (match_index == target_len) {
            return true;
        }
    }

    return false;
}

bool is_openrouter_server_url(const char* url) {
    return string_contains_openrouter(url);
}

static void trim_trailing_slashes(char* text) {
    size_t len;

    if (!text) {
        return;
    }

    len = strlen(text);
    while (len > 0 && text[len - 1] == '/') {
        text[len - 1] = '\0';
        --len;
    }
}

static bool has_suffix(const char* text, const char* suffix) {
    size_t text_len;
    size_t suffix_len;

    if (!text || !suffix) {
        return false;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }

    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

static char* build_openrouter_chat_url(const char* configured_url) {
    char* normalized_base;
    char* result;

    if (!configured_url || configured_url[0] == '\0') {
        return strdup("https://openrouter.ai/api/v1/chat/completions");
    }

    if (strstr(configured_url, "://") != NULL) {
        normalized_base = strdup(configured_url);
    } else if (asprintf(&normalized_base, "https://%s", configured_url) == -1) {
        return NULL;
    }

    if (!normalized_base) {
        return NULL;
    }

    trim_trailing_slashes(normalized_base);

    if (strstr(normalized_base, "/chat/completions") != NULL) {
        return normalized_base;
    }

    if (has_suffix(normalized_base, "/api/v1") || has_suffix(normalized_base, "/v1")) {
        if (asprintf(&result, "%s/chat/completions", normalized_base) == -1) {
            free(normalized_base);
            return NULL;
        }
    } else if (has_suffix(normalized_base, "/api")) {
        if (asprintf(&result, "%s/v1/chat/completions", normalized_base) == -1) {
            free(normalized_base);
            return NULL;
        }
    } else {
        if (asprintf(&result, "%s/api/v1/chat/completions", normalized_base) == -1) {
            free(normalized_base);
            return NULL;
        }
    }

    free(normalized_base);
    return result;
}

static char* make_openrouter_http_post(const char* url, const char* data, const char* api_key) {
    CURL* curl = curl_easy_init();
    CURLcode res;
    ResponseData response_data = {NULL, 0};
    struct curl_slist* headers = NULL;
    char* authHeader = NULL;

    if (!curl) {
        fprintf(stderr, "curl_easy_init() failed\n");
        return NULL;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        fprintf(stderr, "Failed to allocate curl headers\n");
        curl_easy_cleanup(curl);
        return NULL;
    }

    if (api_key && api_key[0] != '\0') {
        struct curl_slist* updatedHeaders;

        if (asprintf(&authHeader, "Authorization: Bearer %s", api_key) == -1) {
            fprintf(stderr, "Failed to construct authorization header\n");
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return NULL;
        }

        updatedHeaders = curl_slist_append(headers, authHeader);
        if (!updatedHeaders) {
            fprintf(stderr, "Failed to allocate curl authorization header\n");
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            free(authHeader);
            return NULL;
        }

        headers = updatedHeaders;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(data));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 14400L);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(response_data.data);
        response_data.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(authHeader);
    return response_data.data;
}

static const char* get_data_url_from_content_item(json_t* item) {
    json_t* type_json;
    json_t* image_url_obj;
    json_t* url_json;
    const char* type_value;

    if (!json_is_object(item)) {
        return NULL;
    }

    type_json = json_object_get(item, "type");
    if (!json_is_string(type_json)) {
        return NULL;
    }

    type_value = json_string_value(type_json);
    if (strcmp(type_value, "image_url") != 0) {
        return NULL;
    }

    image_url_obj = json_object_get(item, "image_url");
    if (!json_is_object(image_url_obj)) {
        return NULL;
    }

    url_json = json_object_get(image_url_obj, "url");
    if (!json_is_string(url_json)) {
        return NULL;
    }

    return json_string_value(url_json);
}

static const char* extract_data_url_from_response(json_t* root) {
    json_t* choices;
    json_t* first_choice;
    json_t* message;
    json_t* images;
    json_t* first_image;
    json_t* image_url_obj;
    json_t* url_json;
    json_t* content;

    choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        return NULL;
    }

    first_choice = json_array_get(choices, 0);
    if (!json_is_object(first_choice)) {
        return NULL;
    }

    message = json_object_get(first_choice, "message");
    if (!json_is_object(message)) {
        return NULL;
    }

    images = json_object_get(message, "images");
    if (json_is_array(images) && json_array_size(images) > 0) {
        first_image = json_array_get(images, 0);
        if (json_is_object(first_image)) {
            image_url_obj = json_object_get(first_image, "image_url");
            if (json_is_object(image_url_obj)) {
                url_json = json_object_get(image_url_obj, "url");
                if (json_is_string(url_json)) {
                    return json_string_value(url_json);
                }
            }
        }
    }

    content = json_object_get(message, "content");
    if (json_is_array(content)) {
        for (size_t i = 0; i < json_array_size(content); ++i) {
            json_t* item = json_array_get(content, i);
            const char* data_url = get_data_url_from_content_item(item);
            if (data_url) {
                return data_url;
            }
        }
    }

    return NULL;
}

static const char* get_base64_payload_from_data_url(const char* data_url) {
    const char* marker = ";base64,";
    const char* marker_pos;

    if (!data_url) {
        return NULL;
    }

    if (strncmp(data_url, "data:image/", 11) != 0) {
        return NULL;
    }

    marker_pos = strstr(data_url, marker);
    if (!marker_pos) {
        return NULL;
    }

    return marker_pos + strlen(marker);
}

int generate_character_image_openrouter(const char* prompt, int character_number, const char* image_dir) {
    char filename[MAX_FILEPATH_BUFFER_SIZE];
    char* url = NULL;
    char* data = NULL;
    char* response = NULL;
    const char* api_key;
    const char* data_url;
    const char* b64_data;
    json_error_t error;
    json_t* payload = NULL;
    json_t* messages = NULL;
    json_t* user_message = NULL;
    json_t* modalities = NULL;
    json_t* imageConfig = NULL;
    json_t* root = NULL;
    int result = 1;

    if (!serverModel || serverModel[0] == '\0') {
        fprintf(stderr, "GUESS_LLAMA_SERVER_MODEL must be set for OpenRouter image generation.\n");
        return 1;
    }

    url = build_openrouter_chat_url(server_url);
    if (!url) {
        fprintf(stderr, "Failed to construct OpenRouter URL.\n");
        return 1;
    }

    payload = json_object();
    messages = json_array();
    user_message = json_pack("{s:s, s:s}", "role", "user", "content", prompt);
    modalities = json_array();
    imageConfig = json_object();

    if (!payload || !messages || !user_message || !modalities || !imageConfig) {
        fprintf(stderr, "Failed to build OpenRouter request payload.\n");
        goto cleanup;
    }

    if (json_array_append_new(messages, user_message) < 0) {
        fprintf(stderr, "Failed to build OpenRouter request payload.\n");
        goto cleanup;
    }
    user_message = NULL;

    if (json_array_append_new(modalities, json_string("image")) < 0) {
        fprintf(stderr, "Failed to build OpenRouter request payload.\n");
        goto cleanup;
    }

    if (json_object_set_new(imageConfig, "aspect_ratio", json_string("1:1")) < 0 ||
        json_object_set_new(imageConfig, "image_size", json_string("1K")) < 0) {
        fprintf(stderr, "Failed to build OpenRouter image config.\n");
        goto cleanup;
    }

    if (json_object_set_new(payload, "model", json_string(serverModel)) < 0 ||
        json_object_set_new(payload, "messages", messages) < 0 ||
        json_object_set_new(payload, "modalities", modalities) < 0 ||
        json_object_set_new(payload, "image_config", imageConfig) < 0) {
        fprintf(stderr, "Failed to build OpenRouter request payload.\n");
        goto cleanup;
    }
    messages = NULL;
    modalities = NULL;
    imageConfig = NULL;

    data = json_dumps(payload, JSON_COMPACT);
    if (!data) {
        fprintf(stderr, "Failed to serialize OpenRouter request payload.\n");
        goto cleanup;
    }

    printf("[OpenRouter/Image] Sending request to: %s\n", url);
    printf("[OpenRouter/Image] Payload: %s\n", data);

    api_key = (serverApiKey && serverApiKey[0] != '\0') ? serverApiKey : llmApiKey;
    if (!api_key || api_key[0] == '\0') {
        fprintf(stderr, "OpenRouter image generation requires GUESS_LLAMA_SERVER_API_KEY or GUESS_LLAMA_LLM_API_KEY.\n");
        goto cleanup;
    }

    response = make_openrouter_http_post(url, data, api_key);
    if (!response) {
        fprintf(stderr, "Failed to get response from OpenRouter.\n");
        goto cleanup;
    }

    printf("[OpenRouter/Image] Response: %s\n", response);

    root = json_loads(response, 0, &error);
    if (!root) {
        fprintf(stderr, "Error parsing OpenRouter response JSON: %s\n", error.text);
        goto cleanup;
    }

    data_url = extract_data_url_from_response(root);
    if (!data_url) {
        fprintf(stderr, "OpenRouter response did not include generated image data.\n");
        goto cleanup;
    }

    b64_data = get_base64_payload_from_data_url(data_url);
    if (!b64_data) {
        fprintf(stderr, "OpenRouter image was not returned as a base64 data URL.\n");
        goto cleanup;
    }

    snprintf(filename, sizeof(filename), "%s/character_%d.png", image_dir, character_number);
    result = save_image_from_base64(b64_data, strlen(b64_data), filename);

cleanup:
    free(url);
    free(data);
    free(response);
    json_decref(payload);
    json_decref(messages);
    json_decref(user_message);
    json_decref(modalities);
    json_decref(imageConfig);
    json_decref(root);
    return result;
}
