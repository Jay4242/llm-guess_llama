# Guess Llama? Game

## Description

This project is a C version of "Guess Who?" built with Raylib, libcurl, Jansson, and pthreads. It uses an LLM to generate themes and character traits, then uses a `stable-diffusion.cpp` server to create the character art the LLM reasons over during play.

The codebase was refactored out of a single large `guess_llama.c` into smaller translation units so the gameplay loop, setup flow, persistence, LLM calls, and Stable Diffusion backend logic are easier to work on independently.

## Screenshot

![Guess Llama gameplay screenshot](game_screenshot.png)

## Project Layout

```text
guess_llama.c        Main UI loop and game-state transitions
guess_llama.h        Shared constants, globals, and cross-module interfaces
game_state.c         Global game state/config definitions and shared cleanup
game_setup.c         Setup thread, theme loading, character assignment, image batch startup
gameplay.c           LLM guessing round and yes/no interaction flow
llm_backend.c        LLM HTTP calls, vision payloads, and response cleanup helpers
stable-diffusion.c   stable-diffusion.cpp backend/image generation logic used by the game
storage.c            Theme directory helpers and JSON save/load logic
```

## Features

- Dynamic theme selection from user input or the LLM.
- AI-generated feature lists for 24 characters per game.
- Stable Diffusion image generation with progress updates in the Raylib UI.
- Reusable image/theme directories under `images/<theme_name>/`.
- Vision-based LLM guessing rounds with yes/no answers in the GUI.

## Dependencies

- `curl` for HTTP requests
- `jansson` for JSON parsing/serialization
- `raylib` for the GUI
- `pthread` for setup and image-generation threading

## Build

```bash
make
```

To clean build artifacts:

```bash
make clean
```

Manual compile equivalent:

```bash
gcc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -g \
  guess_llama.c game_state.c llm_backend.c storage.c gameplay.c game_setup.c stable-diffusion.c \
  -o guess_llama -lcurl -ljansson -lraylib -lpthread
```

`stable-diffusion.c` is now a game module, not a standalone executable target.

## Configuration

Edit the server constants in [game_state.c](game_state.c):

```c
const char* username = "username";
const char* server_url = "localhost:1234";
const char* llmServerAddress = "http://localhost:9090";
```

## Running

```bash
./guess_llama
```

1. Enter a theme or click `LLM Random Theme`.
2. Press `ENTER` for a manual theme, then press `SPACE` to start setup.
3. If an image directory for that theme already exists, choose whether to reuse it or regenerate it.
4. Wait for feature generation, save/load of game data, and image generation.
5. Click `Start Guessing Round` and answer the LLM's yes/no question.

## Notes

- Generated assets and game metadata are stored in `images/<formatted_theme>/`.
- `game_data.json` is reused when you keep existing theme data.
- The LLM currently reasons over generated PNGs saved as `character_<n>.png`.
