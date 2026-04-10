#include "guess_llama.h"

typedef struct {
    char* data;
    size_t size;
} ResponseData;

typedef struct {
    char* filepath;
    char* base64Png;
} VisionImageCacheEntry;

static VisionImageCacheEntry* visionImageCache = NULL;
static size_t visionImageCacheCount = 0;
static pthread_mutex_t visionImageCacheMutex = PTHREAD_MUTEX_INITIALIZER;

static void trim_response_whitespace(char* text) {
    char* start;
    size_t len;

    if (!text) {
        return;
    }

    start = text;
    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        len--;
    }
}

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

static char* perform_json_post(const char* url, const char* data, long timeout_seconds) {
    CURL* curl = curl_easy_init();
    CURLcode res;
    ResponseData response = {NULL, 0};
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

    if (llmApiKey && llmApiKey[0] != '\0') {
        struct curl_slist* updatedHeaders;

        if (asprintf(&authHeader, "Authorization: Bearer %s", llmApiKey) == -1) {
            fprintf(stderr, "Failed to construct authorization header\n");
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return NULL;
        }

        updatedHeaders = curl_slist_append(headers, authHeader);
        if (!updatedHeaders) {
            fprintf(stderr, "Failed to allocate curl authorization header\n");
            curl_slist_free_all(headers);
            free(authHeader);
            curl_easy_cleanup(curl);
            return NULL;
        }

        headers = updatedHeaders;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(data));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        free(response.data);
        response.data = NULL;
    } else {
        trim_response_whitespace(response.data);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(authHeader);
    return response.data;
}

static char* encode_buffer_as_base64(const unsigned char* raw_data, size_t raw_size) {
    static const char b64_chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char* b64_output;
    size_t b64_size;
    size_t out_index = 0;

    if (!raw_data || raw_size == 0) {
        return NULL;
    }

    b64_size = ((raw_size + 2) / 3) * 4 + 1;
    b64_output = malloc(b64_size);
    if (!b64_output) {
        fprintf(stderr, "Failed to allocate memory for base64 data\n");
        return NULL;
    }

    for (size_t i = 0; i < raw_size; i += 3) {
        unsigned int chunk = (unsigned int)raw_data[i] << 16;
        if (i + 1 < raw_size) {
            chunk |= (unsigned int)raw_data[i + 1] << 8;
        }
        if (i + 2 < raw_size) {
            chunk |= (unsigned int)raw_data[i + 2];
        }

        b64_output[out_index++] = b64_chars[(chunk >> 18) & 0x3F];
        b64_output[out_index++] = b64_chars[(chunk >> 12) & 0x3F];
        b64_output[out_index++] = (i + 1 < raw_size) ? b64_chars[(chunk >> 6) & 0x3F] : '=';
        b64_output[out_index++] = (i + 2 < raw_size) ? b64_chars[chunk & 0x3F] : '=';
    }
    b64_output[out_index] = '\0';

    return b64_output;
}

static char* read_image_as_base64(const char* filepath) {
    const int maxDimension = 512;
    Image image;
    unsigned char* encoded_image_data;
    int encoded_image_size = 0;
    char* b64_output;

    image = LoadImage(filepath);
    if (image.data == NULL) {
        fprintf(stderr, "Failed to load image file: %s\n", filepath);
        return NULL;
    }

    if (image.width != maxDimension || image.height != maxDimension) {
        ImageResize(&image, maxDimension, maxDimension);
    }

    encoded_image_data = ExportImageToMemory(image, ".png", &encoded_image_size);
    UnloadImage(image);

    if (!encoded_image_data || encoded_image_size <= 0) {
        fprintf(stderr, "Failed to encode image data: %s\n", filepath);
        if (encoded_image_data) {
            MemFree(encoded_image_data);
        }
        return NULL;
    }

    b64_output = encode_buffer_as_base64(encoded_image_data, (size_t)encoded_image_size);
    MemFree(encoded_image_data);

    return b64_output;
}

static char* encode_loaded_image_as_base64(const Image* loadedImage) {
    const int maxDimension = 512;
    Image workingImage;
    unsigned char* encodedImageData;
    int encodedImageSize = 0;
    char* b64Output;

    if (!loadedImage || !loadedImage->data) {
        return NULL;
    }

    workingImage = ImageCopy(*loadedImage);
    if (!workingImage.data) {
        fprintf(stderr, "Failed to copy loaded image for base64 encoding\n");
        return NULL;
    }

    if (workingImage.width != maxDimension || workingImage.height != maxDimension) {
        ImageResize(&workingImage, maxDimension, maxDimension);
    }

    encodedImageData = ExportImageToMemory(workingImage, ".png", &encodedImageSize);
    UnloadImage(workingImage);

    if (!encodedImageData || encodedImageSize <= 0) {
        fprintf(stderr, "Failed to encode loaded image data\n");
        if (encodedImageData) {
            MemFree(encodedImageData);
        }
        return NULL;
    }

    b64Output = encode_buffer_as_base64(encodedImageData, (size_t)encodedImageSize);
    MemFree(encodedImageData);
    return b64Output;
}

static const char* get_cached_resized_image_base64(const char* filepath) {
    char* loadedBase64 = NULL;
    const char* cachedBase64 = NULL;

    if (!filepath || filepath[0] == '\0') {
        return NULL;
    }

    pthread_mutex_lock(&visionImageCacheMutex);
    for (size_t i = 0; i < visionImageCacheCount; ++i) {
        if (strcmp(visionImageCache[i].filepath, filepath) == 0) {
            cachedBase64 = visionImageCache[i].base64Png;
            pthread_mutex_unlock(&visionImageCacheMutex);
            return cachedBase64;
        }
    }
    pthread_mutex_unlock(&visionImageCacheMutex);

    loadedBase64 = read_image_as_base64(filepath);
    if (!loadedBase64) {
        return NULL;
    }

    pthread_mutex_lock(&visionImageCacheMutex);
    for (size_t i = 0; i < visionImageCacheCount; ++i) {
        if (strcmp(visionImageCache[i].filepath, filepath) == 0) {
            cachedBase64 = visionImageCache[i].base64Png;
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            return cachedBase64;
        }
    }

    {
        VisionImageCacheEntry* resizedCache = realloc(
            visionImageCache,
            (visionImageCacheCount + 1) * sizeof(VisionImageCacheEntry)
        );
        if (!resizedCache) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            fprintf(stderr, "Failed to grow vision image cache\n");
            return NULL;
        }

        visionImageCache = resizedCache;
        visionImageCache[visionImageCacheCount].filepath = strdup(filepath);
        if (!visionImageCache[visionImageCacheCount].filepath) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            fprintf(stderr, "Failed to cache vision image path\n");
            return NULL;
        }

        visionImageCache[visionImageCacheCount].base64Png = loadedBase64;
        cachedBase64 = visionImageCache[visionImageCacheCount].base64Png;
        visionImageCacheCount++;
    }

    pthread_mutex_unlock(&visionImageCacheMutex);
    return cachedBase64;
}

void clear_llm_vision_image_cache(void) {
    pthread_mutex_lock(&visionImageCacheMutex);
    for (size_t i = 0; i < visionImageCacheCount; ++i) {
        free(visionImageCache[i].filepath);
        free(visionImageCache[i].base64Png);
    }
    free(visionImageCache);
    visionImageCache = NULL;
    visionImageCacheCount = 0;
    pthread_mutex_unlock(&visionImageCacheMutex);
}

bool cache_llm_vision_image_from_loaded_image(const char* filepath, const Image* loadedImage) {
    char* loadedBase64 = NULL;

    if (!filepath || filepath[0] == '\0' || !loadedImage || !loadedImage->data) {
        return false;
    }

    pthread_mutex_lock(&visionImageCacheMutex);
    for (size_t i = 0; i < visionImageCacheCount; ++i) {
        if (strcmp(visionImageCache[i].filepath, filepath) == 0) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            return true;
        }
    }
    pthread_mutex_unlock(&visionImageCacheMutex);

    loadedBase64 = encode_loaded_image_as_base64(loadedImage);
    if (!loadedBase64) {
        return false;
    }

    pthread_mutex_lock(&visionImageCacheMutex);
    for (size_t i = 0; i < visionImageCacheCount; ++i) {
        if (strcmp(visionImageCache[i].filepath, filepath) == 0) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            return true;
        }
    }

    {
        VisionImageCacheEntry* resizedCache = realloc(
            visionImageCache,
            (visionImageCacheCount + 1) * sizeof(VisionImageCacheEntry)
        );
        if (!resizedCache) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            fprintf(stderr, "Failed to grow vision image cache\n");
            return false;
        }

        visionImageCache = resizedCache;
        visionImageCache[visionImageCacheCount].filepath = strdup(filepath);
        if (!visionImageCache[visionImageCacheCount].filepath) {
            pthread_mutex_unlock(&visionImageCacheMutex);
            free(loadedBase64);
            fprintf(stderr, "Failed to cache vision image path\n");
            return false;
        }

        visionImageCache[visionImageCacheCount].base64Png = loadedBase64;
        visionImageCacheCount++;
    }

    pthread_mutex_unlock(&visionImageCacheMutex);
    return true;
}

static char* dump_json_payload(json_t* payload) {
    char* serialized = json_dumps(payload, JSON_COMPACT);

    if (!serialized) {
        fprintf(stderr, "Failed to serialize JSON payload\n");
    }
    return serialized;
}

static char* strip_base64_for_logging(const char* json_str) {
    const char* src = json_str;
    size_t out_size = 0;
    char* result;
    char* out;

    while (*src) {
        if (strncmp(src, "\"url\":\"data:image/png;base64,", 27) == 0) {
            out_size += 13;
            src += 27;
            while (*src && *src != '"') src++;
            if (*src == '"') src++;
        } else {
            out_size++;
            src++;
        }
    }

    result = malloc(out_size + 1);
    if (!result) return NULL;

    src = json_str;
    out = result;
    while (*src) {
        if (strncmp(src, "\"url\":\"data:image/png;base64,", 27) == 0) {
            memcpy(out, "[base64_data]", 13);
            out += 13;
            src += 27;
            while (*src && *src != '"') src++;
            if (*src == '"') src++;
        } else {
            *out++ = *src++;
        }
    }
    *out = '\0';

    return result;
}

static int append_text_content_item(json_t* content_array, const char* text) {
    json_t* item = json_pack("{s:s, s:s}", "type", "text", "text", text);

    if (!item) {
        fprintf(stderr, "Failed to construct text content item\n");
        return -1;
    }

    if (json_array_append_new(content_array, item) < 0) {
        fprintf(stderr, "Failed to append text content item\n");
        json_decref(item);
        return -1;
    }

    return 0;
}

static int append_image_content_item(json_t* content_array, const char* data_url) {
    json_t* image_url = NULL;
    json_t* item = NULL;
    json_t* type = NULL;

    image_url = json_pack("{s:s}", "url", data_url);
    item = json_object();
    type = json_string("image_url");
    if (!image_url || !item || !type) {
        fprintf(stderr, "Failed to construct image content item\n");
        json_decref(image_url);
        json_decref(item);
        json_decref(type);
        return -1;
    }

    if (json_object_set_new(item, "type", type) < 0) {
        fprintf(stderr, "Failed to append image content item\n");
        json_decref(image_url);
        json_decref(item);
        json_decref(type);
        return -1;
    }
    type = NULL;

    if (json_object_set_new(item, "image_url", image_url) < 0) {
        fprintf(stderr, "Failed to append image content item\n");
        json_decref(image_url);
        json_decref(item);
        return -1;
    }
    image_url = NULL;

    if (json_array_append_new(content_array, item) < 0) {
        fprintf(stderr, "Failed to append image content item\n");
        json_decref(item);
        return -1;
    }
    item = NULL;

    return 0;
}

static char* extract_first_choice_content(const char* llmResponse) {
    json_error_t error;
    json_t* root = json_loads(llmResponse, 0, &error);
    json_t* choices;
    json_t* firstChoice;
    json_t* message;
    json_t* content;
    char* result;

    if (!root) {
        fprintf(stderr, "Error parsing JSON: %s\n", error.text);
        return NULL;
    }

    choices = json_object_get(root, "choices");
    if (!json_is_array(choices) || json_array_size(choices) == 0) {
        fprintf(stderr, "Error: 'choices' is not a non-empty array.\n");
        fprintf(stderr, "Raw LLM Response: %s\n", llmResponse);
        json_decref(root);
        return NULL;
    }

    firstChoice = json_array_get(choices, 0);
    if (!json_is_object(firstChoice)) {
        fprintf(stderr, "Error: First choice is not an object.\n");
        json_decref(root);
        return NULL;
    }

    message = json_object_get(firstChoice, "message");
    if (!json_is_object(message)) {
        fprintf(stderr, "Error: 'message' is not an object.\n");
        json_decref(root);
        return NULL;
    }

    content = json_object_get(message, "content");
    if (!json_is_string(content)) {
        fprintf(stderr, "Error: 'content' is not a string.\n");
        json_decref(root);
        return NULL;
    }

    result = strdup(json_string_value(content));
    json_decref(root);

    if (!result) {
        fprintf(stderr, "Failed to duplicate LLM content string.\n");
    }
    return result;
}

static char** parse_string_array_from_content(const char* content, int* itemCount) {
    json_error_t error;
    json_t* array;
    char* filtered_content = filter_think_tags(content);
    fprintf(stderr, "[LLM Response Raw]: %s\n", content);
    fprintf(stderr, "[LLM Response Filtered]: %s\n", filtered_content);
    char* json_content = extract_json_from_markdown(filtered_content);
    char** items = NULL;

    free(filtered_content);

    array = json_loads(json_content, 0, &error);
    free(json_content);
    if (!json_is_array(array)) {
        fprintf(stderr, "Error parsing content as JSON array: %s\n", error.text);
        return NULL;
    }

    *itemCount = (int)json_array_size(array);
    items = calloc((size_t)*itemCount, sizeof(char*));
    if (!items) {
        fprintf(stderr, "Memory allocation failed\n");
        json_decref(array);
        return NULL;
    }

    for (int i = 0; i < *itemCount; ++i) {
        json_t* item = json_array_get(array, (size_t)i);
        if (!json_is_string(item)) {
            fprintf(stderr, "Error: Array item is not a string.\n");
            for (int j = 0; j < i; ++j) {
                free(items[j]);
            }
            free(items);
            json_decref(array);
            return NULL;
        }

        items[i] = strdup(json_string_value(item));
        if (!items[i]) {
            fprintf(stderr, "Memory allocation failed\n");
            for (int j = 0; j <= i; ++j) {
                free(items[j]);
            }
            free(items);
            json_decref(array);
            return NULL;
        }
    }

    json_decref(array);
    return items;
}

char* getLLMResponse(const char* prompt, double temperature) {
    char* url = NULL;
    char* data = NULL;
    char* response = NULL;
    json_t* payload = NULL;
    json_t* messages = NULL;
    json_t* system_message = NULL;
    json_t* user_message = NULL;

    if (asprintf(&url, "%s/v1/chat/completions", llmServerAddress) == -1) {
        fprintf(stderr, "Failed to construct URL\n");
        return NULL;
    }

    payload = json_object();
    messages = json_array();
    system_message = json_pack(
        "{s:s, s:s}",
        "role", "system",
        "content", "You are a helpful assistant."
    );
    user_message = json_pack(
        "{s:s, s:s}",
        "role", "user",
        "content", prompt
    );

    if (!payload || !messages || !system_message || !user_message) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }

    if (json_array_append_new(messages, system_message) < 0) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }
    system_message = NULL;

    if (json_array_append_new(messages, user_message) < 0) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }
    user_message = NULL;

    if (json_object_set_new(payload, "model", json_string(llmModel)) < 0) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }

    if (json_object_set_new(payload, "messages", messages) < 0) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }
    messages = NULL;

    if (json_object_set_new(payload, "temperature", json_real(temperature)) < 0) {
        fprintf(stderr, "Failed to construct JSON payload\n");
        goto cleanup;
    }

    data = dump_json_payload(payload);
    if (!data) {
        goto cleanup;
    }

    printf("[LLM] Sending request to: %s\n", url);
    printf("[LLM] Payload: %s\n", data);

    response = perform_json_post(url, data, 14400L);

    if (response) {
        printf("[LLM] Response: %s\n", response);
    }

cleanup:
    free(url);
    free(data);
    json_decref(payload);
    json_decref(messages);
    json_decref(system_message);
    json_decref(user_message);
    return response;
}

char* getLLMResponseWithVision(
    const char* initial_prompt,
    const char** image_paths,
    int image_count,
    const int* image_labels,
    const char* final_prompt,
    double temperature
) {
    char* url = NULL;
    char* response = NULL;
    char* data = NULL;
    json_t* payload = NULL;
    json_t* messages = NULL;
    json_t* user_content = NULL;
    json_t* system_message = NULL;
    json_t* user_message = NULL;

    if (asprintf(&url, "%s/v1/chat/completions", llmServerAddress) == -1) {
        fprintf(stderr, "Failed to construct URL\n");
        return NULL;
    }

    payload = json_object();
    messages = json_array();
    user_content = json_array();
    system_message = json_pack(
        "{s:s, s:s}",
        "role", "system",
        "content", "You are a helpful assistant."
    );
    if (!payload || !messages || !user_content || !system_message) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }

    if (append_text_content_item(user_content, initial_prompt) < 0) {
        goto cleanup;
    }

    for (int i = 0; i < image_count; ++i) {
        const char* base64_image = get_cached_resized_image_base64(image_paths[i]);
        char* data_url = NULL;
        char character_label[32];
        int character_number = (image_labels && image_labels[i] > 0) ? image_labels[i] : (i + 1);

        if (!base64_image) {
            fprintf(stderr, "Failed to encode image %d: %s\n", i + 1, image_paths[i]);
            goto cleanup;
        }

        snprintf(character_label, sizeof(character_label), "Character %d:", character_number);
        if (append_text_content_item(user_content, character_label) < 0) {
            goto cleanup;
        }

        if (asprintf(&data_url, "data:image/png;base64,%s", base64_image) == -1) {
            fprintf(stderr, "Failed to construct image data URL\n");
            goto cleanup;
        }

        if (append_image_content_item(user_content, data_url) < 0) {
            free(data_url);
            goto cleanup;
        }

        free(data_url);
    }

    if (append_text_content_item(user_content, final_prompt) < 0) {
        goto cleanup;
    }

    user_message = json_object();
    if (!user_message) {
        fprintf(stderr, "Failed to construct vision user message\n");
        goto cleanup;
    }

    if (json_object_set_new(user_message, "role", json_string("user")) < 0) {
        fprintf(stderr, "Failed to construct vision user message\n");
        goto cleanup;
    }

    if (json_object_set_new(user_message, "content", user_content) < 0) {
        fprintf(stderr, "Failed to construct vision user message\n");
        goto cleanup;
    }
    user_content = NULL;

    if (json_array_append_new(messages, system_message) < 0) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }
    system_message = NULL;

    if (json_array_append_new(messages, user_message) < 0) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }
    user_message = NULL;

    if (json_object_set_new(payload, "model", json_string(llmModel)) < 0) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }

    if (json_object_set_new(payload, "messages", messages) < 0) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }
    messages = NULL;

    if (json_object_set_new(payload, "temperature", json_real(temperature)) < 0) {
        fprintf(stderr, "Failed to construct vision payload\n");
        goto cleanup;
    }

    data = dump_json_payload(payload);
    if (!data) {
        goto cleanup;
    }

    {
        char* clean_data = strip_base64_for_logging(data);
        printf("[LLM/Vision] Sending request to: %s\n", url);
        printf("[LLM/Vision] Payload:\n%s\n", clean_data ? clean_data : data);
        free(clean_data);
    }

    response = perform_json_post(url, data, 14400L);

    if (response) {
        printf("[LLM/Vision] Response: %s\n", response);
    }

cleanup:
    free(url);
    free(data);
    json_decref(payload);
    json_decref(messages);
    json_decref(user_content);
    json_decref(system_message);
    json_decref(user_message);
    return response;
}

char* filter_think_tags(const char* input_str) {
    const char* think_start_tag = "<think>";
    const char* think_end_tag = "</think>";
    size_t think_start_len = strlen(think_start_tag);
    size_t think_end_len = strlen(think_end_tag);
    char* temp_str;
    char* current_pos;
    char* think_start_found;
    char* think_end_found;
    char* result;
    size_t result_len;

    if (!input_str) {
        return strdup("");
    }

    temp_str = strdup(input_str);
    if (!temp_str) {
        fprintf(stderr, "Failed to allocate memory for temporary string.\n");
        return strdup("");
    }

    current_pos = temp_str;
    while ((think_start_found = strstr(current_pos, think_start_tag)) != NULL) {
        think_end_found = strstr(think_start_found + think_start_len, think_end_tag);
        if (think_end_found) {
            memmove(think_start_found, think_end_found + think_end_len,
                    strlen(think_end_found + think_end_len) + 1);
        } else {
            break;
        }
        current_pos = think_start_found;
    }

    while (*current_pos != '\0' &&
           isspace((unsigned char)*current_pos)) {
        current_pos++;
    }

    result_len = strlen(current_pos);
    while (result_len > 0 &&
           isspace((unsigned char)current_pos[result_len - 1])) {
        result_len--;
    }
    current_pos[result_len] = '\0';

    result = strdup(current_pos);
    free(temp_str);
    return result;
}

char* extract_json_from_markdown(const char* input_str) {
    const char* json_start_tag = "```json";
    const char* json_end_tag = "```";
    const char* start_ptr;
    const char* end_ptr;
    char* result;
    size_t len;

    if (!input_str) {
        return strdup("");
    }

    start_ptr = strstr(input_str, json_start_tag);
    if (!start_ptr) {
        return strdup(input_str);
    }

    start_ptr += strlen(json_start_tag);
    end_ptr = strstr(start_ptr, json_end_tag);
    if (!end_ptr) {
        return strdup(input_str);
    }

    len = (size_t)(end_ptr - start_ptr);
    result = malloc(len + 1);
    if (!result) {
        fprintf(stderr, "Failed to allocate memory for extracted JSON.\n");
        return strdup("");
    }

    strncpy(result, start_ptr, len);
    result[len] = '\0';
    return result;
}

char** getThemesFromLLM(int* themeCount) {
    const char* prompt =
        "Suggest 10 themes for a 'Guess Who?' game. The theme should be who the "
        "characters in the game are, and should be a singular noun. For example: "
        "'clown', 'shih-tzu dog', 'penguin', 'llama', etc. Feel free to be creative "
        "and random. Return a JSON list of strings, only the themes and nothing else.";
    char* llmResponse = getLLMResponse(prompt, 1.0);
    char* content;
    char** themes;

    *themeCount = 0;

    if (!llmResponse) {
        fprintf(stderr, "Failed to get response from LLM\n");
        themes = malloc(sizeof(char*));
        if (!themes) {
            return NULL;
        }
        themes[0] = strdup("Default");
        if (!themes[0]) {
            free(themes);
            return NULL;
        }
        *themeCount = 1;
        return themes;
    }

    content = extract_first_choice_content(llmResponse);
    free(llmResponse);
    if (!content) {
        return NULL;
    }

    themes = parse_string_array_from_content(content, themeCount);
    free(content);
    return themes;
}

char** getCharacterFeatures(const char* theme, int* featureCount) {
    char* prompt = NULL;
    char* llmResponse;
    char* content;
    char** features;

    *featureCount = 0;

    if (asprintf(
            &prompt,
            "Given the theme '%s', suggest 8 distinct features that could be used "
            "to differentiate characters in a 'Guess Who?' game. These features "
            "should be physical attributes or accessories that the %s does NOT "
            "normally have. For example, if the theme is 'cat', good features "
            "would be 'a hat', 'glasses', 'a bow tie'. Avoid verbs like 'wearing', "
            "'has', 'is holding', etc. Bad features would be 'whiskers', 'a tail', "
            "'fur', because cats normally have these. Return a JSON list of strings, "
            "only the features and nothing else.",
            theme,
            theme
        ) == -1) {
        fprintf(stderr, "Failed to construct prompt\n");
        return NULL;
    }

    llmResponse = getLLMResponse(prompt, 1.0);
    free(prompt);

    if (!llmResponse) {
        fprintf(stderr, "Failed to get response from LLM\n");
        return NULL;
    }

    printf("Raw LLM Response: %s\n", llmResponse);
    content = extract_first_choice_content(llmResponse);
    free(llmResponse);
    if (!content) {
        return NULL;
    }

    features = parse_string_array_from_content(content, featureCount);
    free(content);
    if (features && *featureCount != 8) {
        fprintf(stderr, "Warning: Expected 8 features, but got %d\n", *featureCount);
    }
    return features;
}
