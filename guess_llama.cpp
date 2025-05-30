#include <iostream>
#include <string>
#include <curl/curl.h>
#include <sstream>
#include <stdexcept> // Required for std::runtime_error
#include <algorithm> // Required for std::remove
#include <cstring>   // Required for strlen
#include <cassert>   // Required for assertions
#include <vector>
#include <sstream>
#include <fstream>  // Required for file I/O
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include <random>   // Required for random number generation

// Define the LLM server address
std::string llmServerAddress = "http://localhost:9090";

// Callback function to write the response data
size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
    size_t totalSize = size * nmemb;
    output->append((char*)contents, totalSize);
    return totalSize;
}

// Helper function to set curl options and check for errors
CURLcode setCurlOption(CURL* curl, CURLoption option, const void* param) {
    CURLcode res = curl_easy_setopt(curl, option, param);
    if (res != CURLE_OK) {
        std::cerr << "curl_easy_setopt(" << option << ") failed: " << curl_easy_strerror(res) << std::endl;
    }
    return res;
}

// Function to make a request to the LLM API
std::string getLLMResponse(const std::string& prompt, double temperature) {
    CURL* curl;
    CURLcode res;
    std::string response;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (curl) {
        // Set the URL
        res = setCurlOption(curl, CURLOPT_URL, (llmServerAddress + "/v1/chat/completions").c_str());
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set URL");
        }

        // Prepare the JSON payload
        std::string data = R"({"model": "llama-3.2-3b-it-q8_0", "messages": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": ")" + prompt + R"("}], "temperature": )" + std::to_string(temperature) + R"(})";

        // Set the request type to POST
        res = setCurlOption(curl, CURLOPT_POST, (void*)1L);
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set POST");
        }

        // Set the POST data
        res = setCurlOption(curl, CURLOPT_POSTFIELDS, data.c_str());
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set POST fields");
        }

        // Set the data length
        res = setCurlOption(curl, CURLOPT_POSTFIELDSIZE, (void*)((long)data.length()));
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set POST field size");
        }

        // Set the callback function to write the response
        res = setCurlOption(curl, CURLOPT_WRITEFUNCTION, (void*)WriteCallback);
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set write function");
        }
        res = setCurlOption(curl, CURLOPT_WRITEDATA, &response);
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set write data");
        }

        // Set the content type to application/json
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        res = setCurlOption(curl, CURLOPT_HTTPHEADER, headers);
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            curl_global_cleanup();
            throw std::runtime_error("Failed to set HTTP header");
        }

        // Perform the request
        res = curl_easy_perform(curl);

        // Check for errors
        if (res != CURLE_OK) {
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            curl_global_cleanup();
            throw std::runtime_error(std::string("curl_easy_perform() failed: ") + curl_easy_strerror(res));
        }

        // Always cleanup
        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
    } else {
        curl_global_cleanup();
        throw std::runtime_error("curl_easy_init() failed");
    }
    curl_global_cleanup();

    return response;
}

// Function to get the theme from the LLM with retries
std::vector<std::string> getThemesFromLLM() {
    const int maxRetries = 3;
    int retryCount = 0;
    std::vector<std::string> themes;

    while (retryCount < maxRetries) {
        try {
            std::string prompt = "Suggest 10 themes for a 'Guess Who?' game. The theme should be who the characters in the game are. For example: clowns, shih-tzu dogs, penguins, llamas, etc. Feel free to be creative and random. Return a JSON list of strings, only the themes and nothing else.";
            double temperature = 1.0;
            std::string llmResponse = getLLMResponse(prompt, temperature);

            // Find the content part of the response
            rapidjson::Document doc;
            doc.Parse(llmResponse.c_str());

            if (doc.HasParseError()) {
                std::cerr << "Error: Failed to parse JSON (retry " << retryCount + 1 << "/" << maxRetries << "): " << doc.GetParseError() << std::endl;
                std::cout << "Raw LLM Response: " << llmResponse << std::endl;
            } else if (!doc.IsObject() || !doc.HasMember("choices") || !doc["choices"].IsArray() || doc["choices"].Empty() || !doc["choices"][0].IsObject() || !doc["choices"][0].HasMember("message") || !doc["choices"][0]["message"].IsObject() || !doc["choices"][0]["message"].HasMember("content") || !doc["choices"][0]["message"]["content"].IsString()) {
                std::cerr << "Error: Unexpected JSON format (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                std::cout << "Raw LLM Response: " << llmResponse << std::endl;
            } else {
                std::string content = doc["choices"][0]["message"]["content"].GetString();

                // Remove ```json and ``` if present
                size_t startPos = content.find("```json");
                if (startPos != std::string::npos) {
                    content.erase(startPos, 7);
                }
                size_t endPos = content.find("```");
                if (endPos != std::string::npos) {
                    content.erase(endPos, 3);
                }

                // Remove leading/trailing newline characters
                content.erase(0, content.find_first_not_of("\n"));
                content.erase(content.find_last_not_of("\n") + 1);

                // Parse the content as a JSON array
                rapidjson::Document contentDoc;
                contentDoc.Parse(content.c_str());

                if (contentDoc.HasParseError() || !contentDoc.IsArray()) {
                    std::cerr << "Error: Failed to parse content as JSON array (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                    std::cout << "Raw LLM Response: " << llmResponse << std::endl;
                    std::cout << "Cleaned Content: " << content << std::endl; // Print cleaned content
                } else {
                    for (rapidjson::SizeType i = 0; i < contentDoc.Size(); i++) {
                        if (contentDoc[i].IsString()) {
                            themes.push_back(contentDoc[i].GetString());
                        }
                    }

                    if (!themes.empty()) {
                        return themes; // Return the themes if found
                    } else {
                        std::cerr << "Error: No themes found in JSON list (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error getting theme from LLM (retry " << retryCount + 1 << "/" << maxRetries << "): " << e.what() << std::endl;
        }
        retryCount++;
    }

    // If max retries reached, return a default theme
    std::cerr << "Max retries reached for getting themes. Using default theme." << std::endl;
    return {"Default"};
}

// Function to get character features from the LLM based on the theme
std::vector<std::string> getCharacterFeatures(const std::string& theme) {
    const int maxRetries = 3;
    int retryCount = 0;
    std::vector<std::string> features;

    while (retryCount < maxRetries) {
        try {
            std::string prompt = "Given the theme '" + theme + "', suggest 8 distinct features that could be used to differentiate characters in a 'Guess Who?' game. These features should be physical attributes or accessories. Return a JSON list of strings, only the features and nothing else.  For example, if the theme is 'clowns', the features could be: big red nose, blue wig, green hair, top hat, etc.";
            double temperature = 1.0;
            std::string llmResponse = getLLMResponse(prompt, temperature);

            // Find the content part of the response
            rapidjson::Document doc;
            doc.Parse(llmResponse.c_str());

            if (doc.HasParseError()) {
                std::cerr << "Error: Failed to parse JSON (retry " << retryCount + 1 << "/" << maxRetries << "): " << doc.GetParseError() << std::endl;
                std::cout << "Raw LLM Response: " << llmResponse << std::endl;
            } else if (!doc.IsObject() || !doc.HasMember("choices") || !doc["choices"].IsArray() || doc["choices"].Empty() || !doc["choices"][0].IsObject() || !doc["choices"][0].HasMember("message") || !doc["choices"][0]["message"].IsObject() || !doc["choices"][0]["message"].HasMember("content") || !doc["choices"][0]["message"]["content"].IsString()) {
                std::cerr << "Error: Unexpected JSON format (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                std::cout << "Raw LLM Response: " << llmResponse << std::endl;
            } else {
                std::string content = doc["choices"][0]["message"]["content"].GetString();

                // Remove ```json and ``` if present
                size_t startPos = content.find("```json");
                if (startPos != std::string::npos) {
                    content.erase(startPos, 7);
                }
                size_t endPos = content.find("```");
                if (endPos != std::string::npos) {
                    content.erase(endPos, 3);
                }

                // Remove leading/trailing newline characters
                content.erase(0, content.find_first_not_of("\n"));
                content.erase(content.find_last_not_of("\n") + 1);

                // Parse the content as a JSON array
                rapidjson::Document contentDoc;
                contentDoc.Parse(content.c_str());

                if (contentDoc.HasParseError() || !contentDoc.IsArray()) {
                    std::cerr << "Error: Failed to parse content as JSON array (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                    std::cout << "Raw LLM Response: " << llmResponse << std::endl;
                    std::cout << "Cleaned Content: " << content << std::endl; // Print cleaned content
                } else {
                    for (rapidjson::SizeType i = 0; i < contentDoc.Size(); i++) {
                        if (contentDoc[i].IsString()) {
                            features.push_back(contentDoc[i].GetString());
                        }
                    }

                    if (features.size() == 8) {
                        return features; // Return the features if found
                    } else {
                        std::cerr << "Error: Expected 8 features in JSON list, but found " << features.size() << " (retry " << retryCount + 1 << "/" << maxRetries << ")." << std::endl;
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error getting character features from LLM (retry " << retryCount + 1 << "/" << maxRetries << "): " << e.what() << std::endl;
        }
        retryCount++;
    }

    // If max retries reached, return an empty vector
    std::cerr << "Max retries reached for getting character features." << std::endl;
    return {};
}

// Function for the LLM to make a guessing round
std::string llmGuessingRound(const std::vector<std::vector<std::string>>& characterTraits, int llmCharacter, const std::string& theme) {
    std::string preprompt = "The following is the current character list for the theme '" + theme + "':";
    std::string postprompt = "Given this list of characters and their traits, ask a question about a single trait that will help narrow down the list of possible characters. Only ask the question and nothing else.";
    double temperature = 0.7;

    std::stringstream characterListStream;
    for (size_t i = 0; i < characterTraits.size(); ++i) {
        if ((int)i == llmCharacter) continue; // Skip the LLM's own character

        characterListStream << "user: Character " << i + 1 << ": ";
        for (size_t j = 0; j < characterTraits[i].size(); ++j) {
            characterListStream << characterTraits[i][j];
            if (j < characterTraits[i].size() - 1) {
                characterListStream << ", ";
            }
        }
        characterListStream << "\n";
    }
    std::string characterList = characterListStream.str();

    // Escape newline characters
    std::string escapedCharacterList;
    for (char c : characterList) {
        if (c == '\n') {
            escapedCharacterList += "\\n";
        } else {
            escapedCharacterList += c;
        }
    }

    std::string prompt = preprompt + "\\n" + "user:\\n" + escapedCharacterList + "\\n" + "user:\\n" + postprompt;
    std::string llmResponse = getLLMResponse(prompt, temperature);

    // Parse the JSON response
    rapidjson::Document doc;
    doc.Parse(llmResponse.c_str());

    if (doc.HasParseError()) {
        std::cerr << "Error: Failed to parse JSON: " << doc.GetParseError() << std::endl;
        return "Error: Could not parse LLM response.";
    }

    if (!doc.IsObject() || !doc.HasMember("choices") || !doc["choices"].IsArray() || doc["choices"].Empty() || !doc["choices"][0].IsObject() || !doc["choices"][0].HasMember("message") || !doc["choices"][0]["message"].IsObject() || !doc["choices"][0]["message"].HasMember("content") || !doc["choices"][0]["message"]["content"].IsString()) {
        std::cerr << "Error: Unexpected JSON format." << std::endl;
        return "Error: Unexpected LLM response format.";
    }

    std::string content = doc["choices"][0]["message"]["content"].GetString();
    return content;
}

int main() {
    std::cout << "Guess Who? functionality will be implemented here." << std::endl;

    // Prompt for theme
    std::string theme;
    std::cout << "Enter a theme for the game (or leave blank for a random theme): ";
    std::getline(std::cin, theme);

    // If no theme is entered, get one from the LLM
    if (theme.empty()) {
        std::vector<std::string> themes = getThemesFromLLM();

        // Generate a random index to select a theme
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(0, themes.size() - 1);

        int randomIndex = distrib(gen);
        theme = themes[randomIndex];
        std::cout << "Using theme suggested by LLM: " << theme << std::endl;
    } else {
        std::cout << "Using theme: " << theme << std::endl;
    }

    // Get character features based on the theme
    std::vector<std::string> characterFeatures = getCharacterFeatures(theme);
    if (!characterFeatures.empty()) {
        std::cout << "Character features:" << std::endl;
        for (const auto& feature : characterFeatures) {
            std::cout << "- " << feature << std::endl;
        }

        // Assign 2-3 random features to each of the 24 characters
        int numCharacters = 24;
        int numFeatures = characterFeatures.size();

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> numFeaturesDist(2, 3); // Each character has 2-3 features
        std::uniform_int_distribution<> featureIndexDist(0, numFeatures - 1); // Index for random feature
        std::uniform_int_distribution<> characterDist(0, numCharacters - 1); // Index for random character

        std::vector<std::vector<std::string>> characterTraits(numCharacters); // Store features for each character

        for (int i = 0; i < numCharacters; ++i) {
            int numCharFeatures = numFeaturesDist(gen); // Number of features for this character
            std::vector<int> usedFeatureIndices; // Keep track of used feature indices to avoid duplicates

            for (int j = 0; j < numCharFeatures; ++j) {
                int featureIndex;
                // Ensure we don't use the same feature twice for one character
                do {
                    featureIndex = featureIndexDist(gen);
                } while (std::find(usedFeatureIndices.begin(), usedFeatureIndices.end(), featureIndex) != usedFeatureIndices.end());

                usedFeatureIndices.push_back(featureIndex);
                characterTraits[i].push_back(characterFeatures[featureIndex]); // Assign the feature to the character
            }
        }

        std::cout << "\nCharacter Traits:" << std::endl;
        for (int i = 0; i < numCharacters; ++i) {
            std::cout << "Character " << i + 1 << ": ";
            for (size_t j = 0; j < characterTraits[i].size(); ++j) {
                std::cout << characterTraits[i][j];
                if (j < characterTraits[i].size() - 1) {
                    std::cout << ", ";
                }
            }
            std::cout << std::endl;
        }

        // Assign random character to player
        int playerCharacter = characterDist(gen);
        std::cout << "\nYou are character number " << playerCharacter + 1 << std::endl;

        // Assign random character to LLM, making sure it's different from the player's
        int llmCharacter;
        do {
            llmCharacter = characterDist(gen);
        } while (llmCharacter == playerCharacter);

        // Example usage of the llmGuessingRound function
        std::string llmGuess = llmGuessingRound(characterTraits, llmCharacter, theme);
        std::cout << "LLM asks: " << llmGuess << std::endl;

    } else {
        std::cout << "No character features found." << std::endl;
    }

    return 0;
}
