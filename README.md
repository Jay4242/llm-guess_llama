# Guess Llama? Game

## Description

This is a C implementation of the classic "Guess Who?" game, enhanced with the power of Language Models (LLMs) and a graphical user interface (GUI) built with Raylib. The game leverages an LLM to dynamically generate themes and character features, providing a unique and endlessly replayable experience. It also integrates with an Easy Diffusion server to generate unique images for each character. Players can either provide their own theme or let the LLM generate one. The LLM then acts as the opponent, asking questions to guess the player's character.

## Features

*   **Dynamic Theme Generation:** Choose your own theme or let the LLM suggest 10 random themes for the game.
*   **AI-Generated Characters:** The LLM generates 8 distinct physical features based on the chosen theme. 24 unique characters are then created, each assigned 2 random features.
*   **Image Generation:** Connects to an Easy Diffusion server to generate unique 512x512 PNG images for all 24 characters based on their theme and features. Generation progress is displayed in the GUI.
*   **Interactive Gameplay:**
    *   A random character is assigned to the player, and its generated image is displayed.
    *   The LLM (as the opponent) asks yes/no questions.
    *   Players answer using "Yes" and "No" buttons in the GUI.
    *   The LLM processes the answer and eliminates characters from its pool of possibilities.
    *   **Player Win Condition:** If the LLM accidentally eliminates the player's character, the player wins!
*   **Graphical User Interface:** Built with Raylib for an interactive and visually engaging experience.

## Dependencies

To compile and run the game, you need the following libraries:

*   **curl:** For making HTTP requests to the LLM server and the Easy Diffusion server.
*   **jansson:** A C library for encoding, decoding, and manipulating JSON data.
*   **raylib:** A simple and easy-to-use library to enjoy videogames programming.
*   **pthread:** POSIX threads for multi-threading (used for image generation).

Ensure these libraries are installed on your system.

## Compilation

The game is compiled using `gcc`. You can compile it using the provided `Makefile`:

```bash
make
```

This command will compile `guess_llama.c` and link the necessary libraries (`curl`, `jansson`, `raylib`, `pthread`).

After compiling, you can run the game:

```bash
./guess_llama
```

## Configuration

Before running, you might need to adjust the `username` and `server_url` constants in `guess_llama.c` to match your Easy Diffusion server setup:

```c
const char* username = "USERNAME";                             //Add username Here.
const char* server_url = "EASY_DIFFUSION_SERVER_ADDRESS:PORT";         //Add Easy Diffusion Server:Port here.
```

Similarly, the `llmServerAddress` needs to point to your LLM API endpoint:

```c
const char* llmServerAddress = "http://LLM_SERVER_ADDRESS:PORT";
```

## Usage

1.  **Theme Selection:**
    *   Upon launching, you'll see a text input box to "Enter a theme". Type your desired theme (e.g., "Capybara", "Space Aliens").
    *   Alternatively, click the "LLM Random Theme" button to have the LLM suggest a theme for you.
    *   After typing your theme, press `ENTER`. If you used the "LLM Random Theme" button, the theme is already confirmed.
    *   Once the theme is confirmed, a message "Press SPACE to continue..." will appear. Press `SPACE` to proceed.

2.  **Character Generation:**
    *   The game will then start preparing game data and generating images for all 24 characters. A status message and percentage will be displayed during this process. This may take some time depending on your server's performance.
    *   Once all images are generated, they are saved as `character_X.png` files in the game's directory.

3.  **Player Character Assignment:**
    *   A random character is assigned to you, and its image is displayed on the screen. You'll also see its features listed.

4.  **LLM Guessing Round:**
    *   The LLM will start asking yes/no questions about character features (e.g., "Does your character have a big red nose?").
    *   Answer by clicking the "Yes" or "No" buttons in the GUI.
    *   Based on your answer, the LLM will eliminate characters from its internal list of possibilities.
    *   If the LLM accidentally eliminates your character, you win!

## Flowchart

```mermaid
        graph TD
    A[Start] --> B{Get Theme Input};
    B -- User Types & Presses ENTER --> C[Transition to Theme Ready];
    B -- LLM Random Theme Button Clicked --> C;
    C --> D{GAME_STATE_THEME_READY};
    D -- Display Press SPACE to continue... --> D;
    D -- User Presses SPACE --> E[Launch gameSetupThread];
    E --> F{GAME_STATE_IMAGE_GENERATION};
    E --> G[Set generation_status_message];
    F -- gameSetupThread running --> G;
    F -- image_gen_master_thread running --> G;
    G[Display Generation Progress] --> F;
    E --> H[gameSetupThread: Determine Selected Theme];
    H --> I[gameSetupThread: Get Character Features from LLM];
    I --> J{gameSetupThread: Character Features Found?};
    J -- Yes --> K[gameSetupThread: Assign Features to 24 Characters];
    J -- No --> Z[Display Error & Exit];
    K --> L[gameSetupThread: Prepare Batch Image Generation Data];
    L --> M[gameSetupThread: Launch image_gen_master_thread];
    M --> N[gameSetupThread: Assign Player Character];
    N --> O[gameSetupThread: Assign LLM Character];
    O --> P[gameSetupThread: Initialize Remaining Characters List];
    P --> Q[gameSetupThread: Set setup_in_progress = false];
    Q --> F;
    F -- Both threads complete --> R[Load Player Character Image];
    R --> S[Display Player Character & Game UI];
    S --> T{User Clicks Start Guessing Round};
    T --> U[LLM Formulates Question];
    U --> V[Ask Question to User GUI];
    V --> W{User Answers Yes/No Buttons};
    W --> X[LLM Eliminates Characters];
    X -- Player Character Eliminated --> PW[Player Wins!];
    X -- Player Character NOT Eliminated --> Y[Update Remaining Characters List];
    Y --> U;
    PW --> Z[End];
    U -- No More Questions / LLM Guesses --> Z;
