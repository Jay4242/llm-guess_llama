# AGENTS.md - Guidelines for Coding Agents

## Build Commands

### Compile the project
```bash
make
```

### Clean build artifacts
```bash
make clean
```

### Manual compilation
```bash
gcc -std=gnu11 -D_GNU_SOURCE -Wall -Wextra -g \
  guess_llama.c game_state.c llm_backend.c storage.c gameplay.c game_setup.c stable-diffusion.c \
  -o guess_llama -lcurl -ljansson -lraylib -lpthread
```

## Source Layout

- `guess_llama.c` - Main Raylib event loop and screen/state transitions
- `guess_llama.h` - Shared constants, globals, structs, and function declarations
- `game_state.c` - Global game/config state definitions and shared cleanup helpers
- `game_setup.c` - Setup thread, theme resolution, character creation, and image batch startup
- `gameplay.c` - LLM guessing round and yes/no GUI interaction
- `llm_backend.c` - LLM HTTP helpers, JSON payload building, and vision request logic
- `stable-diffusion.c` - Stable Diffusion backend logic extracted from the old monolith
- `storage.c` - Filesystem helpers and `game_data.json` save/load logic

## Dependencies

- **curl** - HTTP requests to LLM and stable-diffusion servers
- **jansson** - JSON encoding/decoding
- **raylib** - GUI and graphics
- **pthread** - Multi-threading for setup and image generation

## Code Style Guidelines

### File Organization
- Small focused `.c` files are preferred over growing `guess_llama.c`
- Put shared declarations in `guess_llama.h`
- Keep module-local helpers `static` inside their implementation files

### Includes Order
1. Standard C headers (`stdio.h`, `stdlib.h`, `string.h`, etc.)
2. Third-party library headers (`curl/curl.h`, `raylib.h`, `jansson.h`)
3. System headers (`sys/stat.h`, `dirent.h`, `unistd.h`)

### Naming Conventions
- **Variables**: `camelCase`
- **Constants**: `UPPER_SNAKE_CASE` with `#define`
- **Typedefs**: `PascalCase`
- **Functions**: `camelCase`
- **Global variables**: Descriptive names, often matching the shared game state in `guess_llama.h`

### Types
- Use `typedef enum` for state machines
- Use `typedef struct` for shared data containers passed across modules
- Initialize globals to zero or `{ 0 }`
- Use `bool` from `<stdbool.h>` for boolean values

### Memory Management
- Always check return values of `malloc`, `calloc`, `realloc`, and `strdup`
- Free allocated memory when no longer needed
- Document ownership when a function returns allocated memory
- Prefer `calloc` for arrays that benefit from predictable zero-initialization

### Error Handling
- Print errors to `stderr` with `fprintf(stderr, ...)`
- Include `strerror(errno)` for system-call failures
- Return `false` or non-zero on failure
- Clean up partial allocations before returning from failing paths

### Comments
- Comment thread ownership and synchronization when it is not obvious
- Comment protocol assumptions for LLM and Stable Diffusion payloads
- Avoid obvious line-by-line comments

### Thread Safety
- Shared state is protected by the global `mutex`
- The image-regeneration prompt uses `regen_cond`
- UI work stays in the main thread; setup/image generation remain background tasks

### HTTP/Network Code
- Set explicit curl timeouts
- Use write callbacks for response buffering
- Check curl return codes
- Free curl header lists after requests

### JSON Handling (jansson)
- Validate JSON types before reading values
- Use `json_decref` for owned references
- Keep save/load formats compatible with `images/<theme>/game_data.json`

### Function Structure
- Keep functions focused on a single responsibility
- Prefer shared helpers in the appropriate module over duplicating parsing logic
- Keep `guess_llama.c` focused on the UI/state loop

### Preprocessor Directives
- Keep numeric constants in `guess_llama.h`
- Keep compiler flags in `Makefile`

## Running the Game

1. Ensure the LLM server and `stable-diffusion.cpp` server are running
2. Configure `username`, `server_url`, and `llmServerAddress` in `game_state.c`
3. Run `./guess_llama`
4. Enter a theme or use `LLM Random Theme`
5. Press `SPACE` to start setup
6. Reuse or regenerate existing theme assets if prompted
7. Click `Start Guessing Round` once setup finishes

## Debugging Tips

- Build with the provided `-g` flags
- Use `printf` tracing for setup flow, thread transitions, and LLM responses
- Check `images/<theme>/game_data.json` when save/load behavior looks wrong
- Validate JSON responses from both backends before assuming model output shape

## Common Pitfalls

- Forgetting to update `Makefile` when adding a new `.c` file
- Introducing cross-module globals without declaring them in `guess_llama.h`
- Leaking partially allocated character-trait arrays on setup failures
- Blocking the main thread while waiting on long backend calls
- Breaking compatibility with the existing `images/<theme>/game_data.json` format
