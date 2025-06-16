#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <getopt.h>
#include <ncurses.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_LINES 256
#define MAX_SEGMENTS_PER_LINE 128
#define MAX_TEXT_PER_SEGMENT 512
#define RAW_BUFFER_SIZE (MAX_LINES * 1024)

// --- Data Structures ---

typedef struct {
    char text[MAX_TEXT_PER_SEGMENT];
    int width;
    short ncurses_pair_id;
} TextSegment;

typedef struct {
    TextSegment segments[MAX_SEGMENTS_PER_LINE];
    int segment_count;
    int total_width;
} MarqueeLine;

typedef struct {
    int direction;
    long delay_us;
    int repeat_count;
    bool ignore_interrupts;
} AppConfig;

typedef struct {
    int position;
    int screen_width;
    int screen_height;
} AnimationState;


// --- Forward Declarations ---

static void parse_line(const char* line_text, MarqueeLine* line);
static int build_text_block(char* raw_text, MarqueeLine* lines, int* max_block_width);
static void print_help(const char* prog_name);

// --- Ncurses & Animation ---

static void setup_ncurses() {
    initscr();
    if (has_colors()) {
        start_color();
        use_default_colors();
    }
    noecho();
    curs_set(FALSE);
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
}

static short get_or_create_color_pair(short fg, short bg) {
    // A simple cache to find already-initialized color pairs.
    typedef struct { short fg, bg; } PairDef;
    static PairDef defined_pairs[256]; // COLOR_PAIRS is often 256
    static short next_pair_id = 1;
    static bool is_initialized = false;

    if (!is_initialized) {
        // Initialize all to an invalid state.
        for(int i = 0; i < 256; ++i) { defined_pairs[i].fg = -9; defined_pairs[i].bg = -9; }
        is_initialized = true;
    }

    // Return default pair for invalid color numbers.
    if (fg < -1 || fg > 255 || bg < -1 || bg > 255) return 0;

    // Search for an existing pair that matches the request.
    for (short i = 1; i < next_pair_id; ++i) {
        if (defined_pairs[i].fg == fg && defined_pairs[i].bg == bg) {
            return i;
        }
    }

    // If no existing pair is found, create a new one if possible.
    if (next_pair_id < COLOR_PAIRS) {
        init_pair(next_pair_id, fg, bg);
        defined_pairs[next_pair_id].fg = fg;
        defined_pairs[next_pair_id].bg = bg;
        return next_pair_id++;
    }

    // Fallback to the default pair if we've run out of available pair slots.
    return 0;
}

static void draw_frame(const MarqueeLine lines[], int line_count, int y_start, const AnimationState* state) {
    clear();
    for (int i = 0; i < line_count; ++i) {
        int current_y = y_start + i;
        if (current_y < 0 || current_y >= state->screen_height) continue;

        int drawn_width = 0;
        for (int j = 0; j < lines[i].segment_count; ++j) {
            const TextSegment* seg = &lines[i].segments[j];
            attron(COLOR_PAIR(seg->ncurses_pair_id));
            for (int k = 0; k < seg->width; ++k) {
                int screen_pos = state->position + drawn_width + k;
                if (screen_pos >= 0 && screen_pos < state->screen_width) {
                    mvaddch(current_y, screen_pos, seg->text[k]);
                }
            }
            attroff(COLOR_PAIR(seg->ncurses_pair_id));
            drawn_width += seg->width;
        }
    }
    refresh();
}

static void update_position(AnimationState* state, int block_width, int direction, int* repeat_count) {
    if (direction == 0) { // Right to left
        state->position--;
        if (state->position < -block_width) {
            state->position = state->screen_width;
            if (*repeat_count > 0) (*repeat_count)--;
        }
    } else { // Left to right
        state->position++;
        if (state->position > state->screen_width) {
            state->position = -block_width;
            if (*repeat_count > 0) (*repeat_count)--;
        }
    }
}

static void run_animation(char* raw_text, const AppConfig* config) {
    void (*original_sigint_handler)(int) = NULL;
    if (config->ignore_interrupts) {
        original_sigint_handler = signal(SIGINT, SIG_IGN);
    }

    setup_ncurses();

    MarqueeLine* lines = malloc(sizeof(MarqueeLine) * MAX_LINES);
    if (!lines) {
        endwin();
        perror("malloc for lines");
        return;
    }

    int max_block_width = 0;
    int line_count = build_text_block(raw_text, lines, &max_block_width);

    if (line_count > 0) {
        int repeat_count = config->repeat_count;
        AnimationState state;
        getmaxyx(stdscr, state.screen_height, state.screen_width);
        state.position = (config->direction == 0) ? state.screen_width : -max_block_width;
        
        while (repeat_count != 0) {
            int ch = getch();
            if (ch == 'q' || ch == 'Q') break;

            if (ch == KEY_RESIZE) {
                getmaxyx(stdscr, state.screen_height, state.screen_width);
            }

            int y_start = (state.screen_height - line_count) / 2;
            draw_frame(lines, line_count, y_start, &state);
            update_position(&state, max_block_width, config->direction, &repeat_count);
            usleep(config->delay_us);
        }
    }

    if (config->ignore_interrupts) {
        signal(SIGINT, original_sigint_handler);
    }
    endwin();
    free(lines);
}

// Converts a 24-bit RGB color to the nearest color in the xterm 256-color palette.
static short rgb_to_256(int r, int g, int b) {
    // Clamp values to the valid 0-255 range.
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    // Check for grayscale first, as it has a separate ramp.
    // The check `r - g < 8 && g - b < 8` is a simple way to find near-grays.
    if (abs(r - g) < 8 && abs(g - b) < 8) {
        // Map the average intensity to the 24-step grayscale ramp.
        int gray = (r + g + b) / 3;
        if (gray < 8) return 16;   // Black
        if (gray > 248) return 231; // White
        return 232 + ((gray - 8) * 24) / 241;
    }
    
    // Map 0-255 to the 0-5 range for the 6x6x6 color cube.
    int r_idx = (r * 5 + 127) / 255;
    int g_idx = (g * 5 + 127) / 255;
    int b_idx = (b * 5 + 127) / 255;
    
    return 16 + (r_idx * 36) + (g_idx * 6) + b_idx;
}

// --- Text Parsing ---

static void parse_line(const char* line_text, MarqueeLine* line) {
    line->segment_count = 0;
    line->total_width = 0;
    int segment_idx = 0;
    short current_fg = COLOR_WHITE;
    short current_bg = -1; // Default terminal background
    
    line->segments[0].ncurses_pair_id = get_or_create_color_pair(current_fg, current_bg);
    line->segments[0].width = 0;
    line->segments[0].text[0] = '\0';

    const char* p = line_text;
    while (*p != '\0') {
        if (*p == '\x1b' && p[1] == '[') {
            const char* seq_end = p + 2;
            while (*seq_end != '\0' && !isalpha((unsigned char)*seq_end)) {
                seq_end++;
            }

            if (*seq_end == 'm') {
                char* seq_codes = strndup(p + 2, seq_end - (p + 2));
                if (seq_codes) {
                    char* saveptr;
                    char* code_str = strtok_r(seq_codes, ";", &saveptr);
                    
                    while (code_str != NULL) {
                        int code = atoi(code_str);
                        if (code == 38 || code == 48) { // Extended color sequence
                            char* type_str = strtok_r(NULL, ";", &saveptr);
                            if (type_str != NULL) {
                                if (atoi(type_str) == 5) { // 256-color mode
                                    char* id_str = strtok_r(NULL, ";", &saveptr);
                                    if (id_str) {
                                        if (code == 38) current_fg = atoi(id_str);
                                        else current_bg = atoi(id_str);
                                    }
                                } else if (atoi(type_str) == 2) { // True color mode
                                    char* r_str = strtok_r(NULL, ";", &saveptr);
                                    char* g_str = strtok_r(NULL, ";", &saveptr);
                                    char* b_str = strtok_r(NULL, ";", &saveptr);
                                    if (r_str && g_str && b_str) {
                                        short color_id = rgb_to_256(atoi(r_str), atoi(g_str), atoi(b_str));
                                        if (code == 38) current_fg = color_id;
                                        else current_bg = color_id;
                                    }
                                }
                            }
                        } else if (code == 0) {
                            current_fg = COLOR_WHITE; current_bg = -1;
                        } else if (code >= 30 && code <= 37) {
                            current_fg = code - 30;
                        } else if (code >= 40 && code <= 47) {
                            current_bg = code - 40;
                        }
                        code_str = strtok_r(NULL, ";", &saveptr);
                    }
                    free(seq_codes);
                }

                if (line->segments[segment_idx].width > 0) {
                    if (++segment_idx >= MAX_SEGMENTS_PER_LINE) break;
                }
                
                line->segments[segment_idx].ncurses_pair_id = get_or_create_color_pair(current_fg, current_bg);
                line->segments[segment_idx].width = 0;
                line->segments[segment_idx].text[0] = '\0';
            }
            p = (*seq_end != '\0') ? (seq_end + 1) : seq_end;
        } else {
            TextSegment* current_segment = &line->segments[segment_idx];
            if (current_segment->width < MAX_TEXT_PER_SEGMENT - 1) {
                current_segment->text[current_segment->width++] = *p;
                current_segment->text[current_segment->width] = '\0';
                line->total_width++;
            }
            p++;
        }
    }
    line->segment_count = (line->segments[0].width > 0 || segment_idx > 0) ? segment_idx + 1 : 0;
}

static int build_text_block(char* raw_text, MarqueeLine* lines, int* max_block_width) {
    int line_count = 0;
    *max_block_width = 0;
    char* saveptr;

    char* line_str = strtok_r(raw_text, "\n", &saveptr);
    while (line_str != NULL && line_count < MAX_LINES) {
        parse_line(line_str, &lines[line_count]);
        if (lines[line_count].total_width > *max_block_width) {
            *max_block_width = lines[line_count].total_width;
        }
        line_count++;
        line_str = strtok_r(NULL, "\n", &saveptr);
    }
    return line_count;
}

// --- Main Program Flow ---

static char* acquire_input_text(int argc, char* argv[], int optind) {
    char* buffer = malloc(RAW_BUFFER_SIZE);
    if (!buffer) {
        perror("malloc");
        return NULL;
    }
    buffer[0] = '\0';

    if (optind < argc) {
        for (int i = optind; i < argc; i++) {
            strncat(buffer, argv[i], RAW_BUFFER_SIZE - strlen(buffer) - 1);
            if (i < argc - 1) {
                strncat(buffer, "\n", RAW_BUFFER_SIZE - strlen(buffer) - 1);
            }
        }
    } else if (!isatty(STDIN_FILENO)) {
        fread(buffer, 1, RAW_BUFFER_SIZE - 1, stdin);
    }

    if (strlen(buffer) == 0) {
        free(buffer);
        return NULL;
    }
    return buffer;
}

static void parse_options(int argc, char* argv[], AppConfig* config) {
    static struct option long_options[] = {
        {"accident", no_argument, 0, 'a'},
        {"reverse", no_argument, 0, 'r'},
        {"loop", no_argument, 0, 'l'},
        {"speed", required_argument, 0, 's'},
        {"count", required_argument, 0, 'c'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "arls:c:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'a': config->ignore_interrupts = true; break;
            case 'r': config->direction = 1; break;
            case 'l': config->repeat_count = -1; break; // -1 for infinite loop
            case 's': config->delay_us = strtol(optarg, NULL, 10); break;
            case 'c': config->repeat_count = strtol(optarg, NULL, 10); break;
            case 'h':
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
}

int main(int argc, char* argv[]) {
    AppConfig config = {
        .direction = 0,
        .delay_us = 100000,
        .repeat_count = 1,
        .ignore_interrupts = false
    };

    parse_options(argc, argv, &config);

    const int SAFE_LOOP_LIMIT = 10;
    if (config.ignore_interrupts && (config.repeat_count == -1 || config.repeat_count > SAFE_LOOP_LIMIT)) {
        fprintf(stderr, "Warning: --accident (-a) is disabled for safety.\n");
        config.ignore_interrupts = false;
        sleep(2);
    }

    char* raw_text = acquire_input_text(argc, argv, optind);
    if (!raw_text) {
        print_help(argv[0]);
        return EXIT_FAILURE;
    }
    
    // Make a mutable copy for the animation function, which uses strtok_r.
    char* text_buffer = strdup(raw_text);
    if (!text_buffer) {
        perror("strdup");
        free(raw_text);
        return EXIT_FAILURE;
    }
    
    run_animation(text_buffer, &config);
    
    free(text_buffer);
    free(raw_text);
    return EXIT_SUCCESS;
}

static void print_help(const char* prog_name) {
    printf("Usage: %s [text...] [OPTIONS]\n\n", prog_name);
    printf("Displays multi-line text as a scrolling marquee in the terminal.\n");
    printf("Text can be provided as arguments (use $'...' for newlines), or piped via stdin.\n\n");
    printf("OPTIONS:\n");
    printf("  -c, --count <n>      Scroll <n> times. (Default: 1)\n");
    printf("  -s, --speed <usec>   Set animation delay in microseconds. (Default: 100000)\n");
    printf("  -r, --reverse        Scroll from left to right.\n");
    printf("  -l, --loop           Scroll infinitely.\n");
    printf("  -a, --accident       Ignore Ctrl-C interruptions.\n");
    printf("  -h, --help           Display this help message and exit.\n\n");
    printf("EXAMPLES:\n");
    printf("  # Scroll simple text from command-line arguments (runs once by default).\n");
    printf("  %s \"Hello, world!\"\n\n", prog_name);
    printf("  # Scroll multi-line text from arguments using $'...' syntax for newlines.\n");
    printf("  %s $'First line\\nSecond line' --count 2\n\n", prog_name);
    printf("  # Scroll from left to right, slowly, and loop infinitely.\n");
    printf("  %s --reverse --speed 200000 --loop \"Slowly to the right...\"\n\n", prog_name);
    printf("  # Scroll colored text piped from 'echo'.\n");
    printf("  echo -e \"\\x1b[31mRED\\x1b[0m, \\x1b[32mGREEN\\x1b[0m, and \\x1b[34mBLUE\\x1b[0m\" | %s -l\n\n", prog_name);
    printf("  # Scroll ASCII art from 'figlet' (if installed).\n");
    printf("  figlet \"Marquee\" | %s --loop --speed 120000\n", prog_name);
}