// Reference: https://viewsourcecode.org/snaptoken/kilo/index.html

#include <stdbool.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include <termios.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <ncurses.h>
#include <stddef.h>
#include <sys/wait.h>

#define STRINGS_IMPLEMENTATION
#include "strings.h"

#define DEBUG true

static inline bool streq(const char *s1, const char *s2) { return strcmp(s1, s2) == 0; }
static inline bool strneq(const char *s1, const char *s2, size_t n) { return strncmp(s1, s2, n) == 0; }

#define BOOL_AS_CSTR(value) ((value) ? "true" : "false")

typedef struct
{
    String content;
} Row;

typedef struct
{
    char **items;
    size_t count;
    size_t capacity;
} Strings;

typedef struct
{
    Row *items;
    size_t count;
    size_t capacity;
} Rows;

typedef struct
{
    char **items;
    size_t count;
    size_t capacity;
    size_t index;
} CyclableStrings;

typedef struct
{
    size_t x;
    size_t y;
} Cursor;

typedef struct
{
    Cursor *items;
    size_t count;
    size_t capacity;
    bool is_enabled;
} MultiCursor;

typedef struct
{
    Cursor **items;
    size_t count;
    size_t capacity;
} CursorPtrs;

typedef struct
{
    char *name;
    Cursor cursor;
    bool is_primary;
} SnippetMark;

typedef struct
{
    SnippetMark *items;
    size_t count;
    size_t capacity;
} SnippetMarks;

typedef struct
{
    char *handle;
    size_t handle_len;
    char *body;
    bool is_inline;
    bool handle_is_prefix;
    SnippetMark *marks;
    size_t marks_count;
} Snippet;

typedef struct
{
    Snippet *items;
    size_t count;
    size_t capacity;
} Snippets;

typedef enum
{
    PISQUY_INT,
    PISQUY_UINT,
    PISQUY_STRING,
    PISQUY_BOOL,
    PISQUY_ARG_PLACEHOLDER,
    PISQUY_TYPES_COUNT
} PisquyType;

typedef struct
{
    PisquyType type;
    union {
        int int_value;
        size_t uint_value;
        char *string_value;
        bool bool_value;
    };
    char *name;
} Var;

typedef struct
{
    Var *items;
    size_t count;
    size_t capacity;
} Vars;

typedef enum { LN_NO, LN_ABS, LN_REL } ConfigLineNumbers;
typedef enum { CONFIGLOG_ALL, CONFIGLOG_WARNING, CONFIGLOG_ERROR } ConfigLogLevel;

typedef struct
{
    size_t quit_times;
    ConfigLineNumbers line_numbers;
    bool tab_to_spaces;
    size_t tab_spaces_number;
    ConfigLogLevel configlog_level;

    Vars vars;
} Config;

typedef enum
{
    CONFIG_QUIT_TIMES,
    CONFIG_LINE_NUMBERS,
    CONFIG_TAB_TO_SPACES,
    CONFIG_TAB_SPACES_NUMBER,
    CONFIG_CONFIGLOG_LEVEL,

    CONFIG_FIELDS_COUNT
} __ActualConfigFields;

typedef struct
{
    char *filepath;
    char *filename;
    Rows rows;
    int dirty;

    Cursor cursor;
    MultiCursor multicursor;
    CursorPtrs sorted_multicursor;

    size_t offset;
    size_t screen_rows;
    size_t screen_cols;
    size_t page;

    CyclableStrings messages;
    bool is_showing_message;

    String cmd;
    size_t cmd_pos;
    bool in_cmd;
    CyclableStrings commands_history;

    Snippets snippets;
    struct {
        Snippet *snippet;
        int mark_index;
        Cursor base_cursor;
        SnippetMarks active_marks;
    } expanding_snippet;

    Config config;

    size_t current_quit_times;

    int N;

} Editor;
static Editor editor = {0};

#define N_DEFAULT -1 
#define N_OR_DEFAULT(n) (assert(n >= 0), (size_t)(editor.N == N_DEFAULT ? (n) : editor.N))
#define N_TIMES for (size_t i = 0; i < N_OR_DEFAULT(1); i++)

#define LINE_NUMBERS_SPACE \
    (editor.config.line_numbers == LN_NO ? 0 : (size_t)log10(editor.rows.count == 0 ? 1 : editor.rows.count)+3)

#define CURRENT_Y_POS (editor.offset+editor.cursor.y)
#define CURRENT_X_POS (editor.cursor.x)
#define ROW(i) (assert((i) <= editor.rows.count), &editor.rows.items[i])
#define CURRENT_ROW ROW(CURRENT_Y_POS)
#define LINE(i) (ROW(i)->content.items)
#define CURRENT_LINE LINE(CURRENT_Y_POS)
#define CHAR(row, i) (LINE(row)[i])
#define CURRENT_CHAR CHAR(CURRENT_Y_POS, CURRENT_X_POS)
#define N_PAGES (editor.rows.count/win_main.height + 1)

/* ANSI escape sequences */
#define ANSI_ERASE_LINE_FROM_CURSOR "\x1b[K"
#define ANSI_INVERSE "\x1b[7m"
#define ANSI_FG_COLOR(rgb_color) "\x1b[38;2;"rgb_color"m"
#define ANSI_RESET "\x1b[0m"
#define ANSI_SAVE_CURSOR "\x1b[s"
#define ANSI_RESTORE_CURSOR "\x1b[u"

/* Colors */
typedef enum
{
    ED_COLOR_DEFAULT_BACKGROUND = 10,
    ED_COLOR_DEFAULT_FOREGROUND,
    ED_COLOR_YELLOW,
    ED_COLOR_RED,
    ED_COLOR_BLUE
} Ed_Color;

typedef enum
{
    KEY_NULL  = 0,
    CTRL_H    = 8,
    TAB       = 9,
    CTRL_J    = 10,
    CTRL_K    = 11,
    CTRL_L    = 12,
    ENTER     = 13,
    CTRL_M    = 13,
    CTRL_N    = 14,
    CTRL_P    = 16,
    CTRL_Q    = 17,
    CTRL_S    = 19,
    ESC       = 27,

    ALT_0     = 1000,
    ALT_1,
    ALT_2,
    ALT_3,
    ALT_4,
    ALT_5,
    ALT_6,
    ALT_7,
    ALT_8,
    ALT_9, // NOTE: ALT_digit sequences must be consecutive

    ALT_k,
    ALT_j,
    ALT_h,
    ALT_l,

    ALT_c,
    ALT_C,
    ALT_K,
    ALT_J,
    ALT_H,
    ALT_L,

    ALT_m,
    ALT_n,
    ALT_p,

    CTRL_ALT_C,
    CTRL_ALT_K,
    CTRL_ALT_J,
    CTRL_ALT_H,
    CTRL_ALT_L,

    ALT_BACKSPACE,
    ALT_COLON,
} Key;

typedef struct
{
    WINDOW *win;
    size_t height;
    size_t width;
    size_t start_y;
    size_t start_x;
} Window;

static Window win_main = {0};
static Window win_line_numbers = {0};
static Window win_message = {0};
static Window win_command = {0};
static Window win_status = {0};

_Noreturn void print_error_and_exit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    clear();
    printw("ERROR: ");
    vw_printw(stdscr, fmt, ap);
    refresh();
    nodelay(stdscr, FALSE);
    getch();
    exit(1);
}

const char *logpath = "./log.txt";
void log_this(char *format, ...)
{
    if (!DEBUG) return;

    FILE *logfile = fopen(logpath, "a");
    if (logfile == NULL) {
        print_error_and_exit("Could not open log file at `%s`\n", logpath);
    }
    va_list fmt; 

    va_start(fmt, format);
    vfprintf(logfile, format, fmt);
    fprintf(logfile, "\n");

    va_end(fmt);
    fclose(logfile);
}

void write_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024] = {0};
    vsnprintf(buf, win_message.width, fmt, ap);  // TODO: aumentare la dimensione della finestra
                                                 //       se il messaggio e' lungo o su piu' righe
    va_end(ap);

    // maybe I can do some sort of temporary messages
    // maybe I can display in the status that there is a new message so that they're not invasive (config?)
    editor.messages.index = editor.messages.count;
    da_push(&editor.messages, strdup(buf)); // NOTE: rember to free
    if (!editor.in_cmd) editor.is_showing_message = true;
}

static inline bool editor_is_expanding_snippet(void) { return editor.expanding_snippet.snippet != NULL; }

/// BEGIN Cursors

int compare_cursors_reverse(const void *p1, const void *p2)
{
    Cursor *c1 = *(Cursor **)p1;
    Cursor *c2 = *(Cursor **)p2;
    if (c1->y != c2->y) return c2->y - c1->y;
    return c2->x - c1->x;
}

void add_multicursor_mark(void)
{
    if (editor.multicursor.is_enabled || editor.in_cmd) return;

    da_foreach (editor.multicursor, Cursor, cursor)
        if (editor.cursor.x == cursor->x && editor.cursor.y == cursor->y)
            return;

    da_push(&editor.multicursor, editor.cursor);
    write_message("Added cursor mark at (%zu, %zu)", editor.cursor.x, editor.cursor.y);
}

void sort_multicursor(void)
{
    da_clear(&editor.sorted_multicursor);
    da_push(&editor.sorted_multicursor, &editor.cursor);
    
    da_foreach (editor.multicursor, Cursor, cursor)
        if (editor.cursor.x != cursor->x || editor.cursor.y != cursor->y)
            da_push(&editor.sorted_multicursor, cursor);

    qsort(editor.sorted_multicursor.items, editor.sorted_multicursor.count, sizeof(Cursor*), compare_cursors_reverse);
}

void enable_multicursor(void)
{
    if (editor.multicursor.is_enabled || editor.in_cmd) return;

    if (!da_is_empty(&editor.multicursor)) {
        sort_multicursor();
        editor.multicursor.is_enabled = true;
        //write_message("Multicursor has been enabled");
    } //else write_message("No marks to enable multicursor");
}

void disable_multicursor(void)
{
    if (!editor.multicursor.is_enabled || editor.in_cmd) return;

    da_clear(&editor.multicursor);
    editor.multicursor.is_enabled = false;

    if (editor_is_expanding_snippet()) editor.expanding_snippet.snippet = NULL;
    //else write_message("Multicursor has been disabled and all marks have been cleared");
}

/// END Cursors

/// BEGIN Commands

typedef enum
{
    BUILTIN_SAVE,
    BUILTIN_QUIT,
    BUILTIN_SAVE_AND_QUIT,
    BUILTIN_FORCE_QUIT,
    BUILTIN_MOVE_CURSOR,
    BUILTIN_MOVE_CURSOR_UP,
    BUILTIN_MOVE_CURSOR_DOWN,
    BUILTIN_MOVE_CURSOR_LEFT,
    BUILTIN_MOVE_CURSOR_RIGHT,
    BUILTIN_MOVE_LINE_UP,
    BUILTIN_MOVE_LINE_DOWN,
    BUILTIN_INSERT,
    BUILTIN_DATE,
    BUILTIN_GOTO_LINE,
    BUILTIN_CMDS_COUNT,
    UNKNOWN,
    ERROR,
    COMMAND_FROM_LINE,
    USER_DEFINED,
} CommandType;

static_assert(BUILTIN_CMDS_COUNT == 14, "Associate a name to all builtin commands");
/* NOTE: name of the builtin commands */
#define SAVE              "s"
#define QUIT              "q"
#define SAVE_AND_QUIT     "sq"
#define FORCE_QUIT        "fq"
#define MOVE_CURSOR       "mv"
#define MOVE_CURSOR_UP    "mvu"
#define MOVE_CURSOR_DOWN  "mvd"
#define MOVE_CURSOR_LEFT  "mvl"
#define MOVE_CURSOR_RIGHT "mvr"
#define MOVE_LINE_UP      "mvlu"
#define MOVE_LINE_DOWN    "mvld"
#define INSERT            "ins"
#define DATE              "date"
#define GOTO_LINE         "goto"

typedef struct
{
    PisquyType type;
    union {
        int int_value;
        size_t uint_value;
        char *string_value;
        size_t placeholder_index;
    };
    size_t index;
    char *name;
} CommandArg;

typedef struct
{
    CommandArg *items;
    size_t count;
    size_t capacity;
} CommandArgs;

void add_command_arg_int(CommandArgs *args, char *name, int value)
{
    CommandArg arg = {
        .type = PISQUY_INT,
        .name = strdup(name),
        .int_value = value,
        .index = args->count
    };
    da_push(args, arg);
}

void add_command_arg_uint(CommandArgs *args, char *name, size_t value)
{
    CommandArg arg = {
        .type = PISQUY_UINT,
        .name = strdup(name),
        .uint_value = value,
        .index = args->count
    };
    da_push(args, arg);
}

void add_command_arg_string(CommandArgs *args, char *name, char *value)
{
    CommandArg arg = {
        .type = PISQUY_STRING,
        .name = strdup(name),
        .string_value = strdup(value),
        .index = args->count
    };
    da_push(args, arg);
}

CommandArgs deep_copy_command_args(const CommandArgs *args)
{
    CommandArgs copy = {0};
    copy.items = malloc(sizeof(CommandArg)*args->count);
    copy.count = args->count;
    copy.capacity = args->capacity;
    for (size_t i = 0; i < args->count; i++) {
        copy.items[i] = args->items[i];
        copy.items[i].name = strdup(args->items[i].name);
        if (args->items[i].type == PISQUY_STRING) {
            copy.items[i].string_value = strdup(args->items[i].string_value);
        }
    }
    return copy;
}

typedef struct Command Command;

typedef struct
{
    struct Command *items;
    size_t count;
    size_t capacity;
} Commands;

typedef void (*CommandFn)(Command *cmd, CommandArgs *args);

struct Command
{
    char *name;
    CommandType type;
    CommandArgs baked_args;
    Commands subcmds;
    size_t n;
    CommandFn execute;
};

static Commands commands = {0}; // NOTE: all commands are stored in here
#define USER_CMDS_COUNT (commands.count - BUILTIN_CMDS_COUNT)

bool is_command_type_builtin(CommandType type) { return type >= 0 && type < BUILTIN_CMDS_COUNT; }
bool is_command_type_user_defined(CommandType type)
{
    return type >= USER_DEFINED && type < USER_DEFINED + USER_CMDS_COUNT;
}

bool cs_is_empty(CyclableStrings cs) { return cs.count == 0; }

char *cs_get_current(CyclableStrings cs)
{
    return cs_is_empty(cs) ? NULL : cs.items[cs.index];
}

void cs_push(CyclableStrings *cs, char *s) { da_push(cs, strdup(s)); }

char *cs_previous(CyclableStrings *cs)
{
    if (da_is_empty(cs)) return NULL;
    if (cs->index > 0) cs->index--; 
    //else cs->index = cs->count-1; // TODO: make it a config?
    return cs->items[cs->index];

    //size_t len = strlen(cmd);
    //s_clear(&editor.cmd);
    //s_push_str(&editor.cmd, cmd, len);
    //editor.cmd_pos = len;
}

char *cs_next(CyclableStrings *cs)
{
    if (da_is_empty(cs)) return NULL;
    if (cs->index < cs->count-1) cs->index++;
    //else cs->index = 0; // TODO: make it a config?
    return cs->items[cs->index];

    //size_t len = strlen(cmd);
    //s_clear(&editor.cmd);
    //s_push_str(&editor.cmd, cmd, len);
    //editor.cmd_pos = len;
}

bool expect_n_arguments(Command *cmd, CommandArgs *args, size_t n)
{
    if (args->count == n) return true;

    write_message("ERROR: command `%s` expects %zu argument%s, but got %zu",
            cmd->name, n, n == 1 ? "" : "s", args->count);
    return false;
}

static_assert(BUILTIN_CMDS_COUNT == 14, "Parse all commands in get_command_type_from_string");
CommandType get_command_type_from_string(char *type)
{
    if      (streq(type, SAVE))              return BUILTIN_SAVE;
    else if (streq(type, QUIT))              return BUILTIN_QUIT;
    else if (streq(type, SAVE_AND_QUIT))     return BUILTIN_SAVE_AND_QUIT;
    else if (streq(type, FORCE_QUIT))        return BUILTIN_FORCE_QUIT;
    else if (streq(type, MOVE_CURSOR))       return BUILTIN_MOVE_CURSOR;
    else if (streq(type, MOVE_CURSOR_UP))    return BUILTIN_MOVE_CURSOR_UP;
    else if (streq(type, MOVE_CURSOR_DOWN))  return BUILTIN_MOVE_CURSOR_DOWN;
    else if (streq(type, MOVE_CURSOR_LEFT))  return BUILTIN_MOVE_CURSOR_LEFT;
    else if (streq(type, MOVE_CURSOR_RIGHT)) return BUILTIN_MOVE_CURSOR_RIGHT;
    else if (streq(type, MOVE_LINE_UP))      return BUILTIN_MOVE_LINE_UP;
    else if (streq(type, MOVE_LINE_DOWN))    return BUILTIN_MOVE_LINE_DOWN;
    else if (streq(type, INSERT))            return BUILTIN_INSERT;
    else if (streq(type, DATE))              return BUILTIN_DATE;
    else if (streq(type, GOTO_LINE))         return BUILTIN_GOTO_LINE;
    else {
        for (size_t i = USER_DEFINED; i < commands.count; i++) {
            if (streq(type, commands.items[i].name))
                return i;
        }
        return UNKNOWN;
    }
}

int get_command_index(CommandType type)
{
    if (is_command_type_builtin(type))      return type;
    if (is_command_type_user_defined(type)) return type - USER_DEFINED + BUILTIN_CMDS_COUNT;
    return -1;
}

Command *get_command(CommandType type)
{
    int index = get_command_index(type);
    if (index == -1) return NULL;
    return &commands.items[index];
}

static_assert(BUILTIN_CMDS_COUNT == 14, "get_command_type_as_cstr");
char *get_command_type_as_cstr(CommandType type)
{
    switch (type)
    {
        case BUILTIN_SAVE:              return SAVE;
        case BUILTIN_QUIT:              return QUIT;
        case BUILTIN_SAVE_AND_QUIT:     return SAVE_AND_QUIT;
        case BUILTIN_FORCE_QUIT:        return FORCE_QUIT;
        case BUILTIN_MOVE_CURSOR:       return MOVE_CURSOR;
        case BUILTIN_MOVE_CURSOR_UP:    return MOVE_CURSOR_UP;
        case BUILTIN_MOVE_CURSOR_DOWN:  return MOVE_CURSOR_DOWN;
        case BUILTIN_MOVE_CURSOR_LEFT:  return MOVE_CURSOR_LEFT;
        case BUILTIN_MOVE_CURSOR_RIGHT: return MOVE_CURSOR_RIGHT;
        case BUILTIN_MOVE_LINE_UP:      return MOVE_LINE_UP;
        case BUILTIN_MOVE_LINE_DOWN:    return MOVE_LINE_DOWN;
        case BUILTIN_INSERT:            return INSERT;
        case BUILTIN_DATE:              return DATE;
        case BUILTIN_GOTO_LINE:         return GOTO_LINE;
        case UNKNOWN:                   return "unknown";

        case BUILTIN_CMDS_COUNT:
        case ERROR:
        case COMMAND_FROM_LINE:
        case USER_DEFINED:
        default:
            if (is_command_type_user_defined(type)) return get_command(type)->name;
            else print_error_and_exit("Unreachable command type %u in get_command_type_as_cstr", type);
    }
}

void add_builtin_command(char *name, CommandType type, CommandFn execute, CommandArgs *baked_args)
{
    Command cmd = {0};
    cmd.name = name;
    cmd.type = type;
    if (baked_args) cmd.baked_args = deep_copy_command_args(baked_args);
    cmd.execute = execute;
    da_push(&commands, cmd);
}

void add_user_defined_command(Command cmd)
{
    cmd.type = USER_DEFINED + commands.count - BUILTIN_CMDS_COUNT;
    da_push(&commands, cmd);
}

void free_command_arg(CommandArg *arg)
{
    if (arg->name) free(arg->name);
    if (arg->type == PISQUY_STRING) free(arg->string_value);
}

void free_command_args(CommandArgs *args)
{
    da_foreach(*args, CommandArg, arg)
        free_command_arg(arg);
    da_free(args);
}

void free_commands(Commands *cmds);
void free_command(Command *cmd)
{
    if (is_command_type_user_defined(cmd->type))
        free(cmd->name);
    if (cmd->baked_args.count > 0)
        free_command_args(&cmd->baked_args);
    if (cmd->subcmds.count > 0)
        free_commands(&cmd->subcmds);
}

void free_commands(Commands *cmds)
{
    da_foreach(*cmds, Command, cmd)
        free_command(cmd);
    da_free(cmds);
}

typedef struct
{
    char *path;
    size_t row;
    size_t col;
} Location;

#define LOC_FMT "%s:%zu:%zu:"
#define LOC_ARG(loc) ((loc).path), ((loc).row+1), ((loc).col+1)

size_t trim_left(char **str)
{
    char *tmp = *str;
    while (isspace(**str)) *str += 1;
    return *str - tmp;
}
size_t trim_right(char *str)
{
    char *end = str+strlen(str)-1;
    char *tmp = end;
    while (isspace(*end)) end--;
    *(end+1) = '\0';
    return tmp - end;
}
void trim(char **str)
{
    trim_left(str);
    trim_right(*str);
}

/* Command grammar
    CMD_DEF -> # cmd_name : CMD_LIST
    CMD_LIST -> CMD CMD_LIST | epsilon
    CMD -> n cmd_name ( ARG_LIST )
    ARG -> arg_name = arg_value
    ARG_LIST -> ARG ARG_LIST | epsilon
*/

char *read_file(char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *content = malloc(len + 1);
    fread(content, len, 1, f);
    fclose(f);
    content[len] = '\0';
    return content;
}

typedef enum
{
    TOKEN_IDENT = 256,
    TOKEN_NUMBER,
    TOKEN_STRING,

    TOKEN_TRUE,
    TOKEN_FALSE,

    TOKEN_SET,
    TOKEN_VAR,
    TOKEN_DEF,
    TOKEN_SNIPPET,

    TOKEN_NEWLINE,
    TOKEN_EOF,
    TOKENS_COUNT
} TokenType;
#define ACTUAL_TOKENS_COUNT (TOKENS_COUNT-256)

typedef struct
{
    TokenType type;
    union {
        int number_value;
        char *string_value;
    };
    Location loc;
} Token;

static_assert(ACTUAL_TOKENS_COUNT == 11, "token type string");
#define TOKEN_CHAR_STRING         "char"
#define TOKEN_CONTROL_CHAR_STRING "control char"
#define TOKEN_IDENT_STRING        "identifier"
#define TOKEN_NUMBER_STRING       "number"
#define TOKEN_STRING_STRING       "string"
#define TOKEN_TRUE_STRING         "true"
#define TOKEN_FALSE_STRING        "false"
#define TOKEN_SET_STRING          "set"
#define TOKEN_VAR_STRING          "var"
#define TOKEN_DEF_STRING          "def"
#define TOKEN_SNIPPET_STRING      "snippet"
#define TOKEN_NEWLINE_STRING      "new line"
#define TOKEN_EOF_STRING          "EOF"

static_assert(ACTUAL_TOKENS_COUNT == 11, "token_type_as_string");
char *token_type_as_string(TokenType type)
{
    const size_t size = 128*sizeof(char);
    char *result = malloc(size);
    memset(result, 0, size);
    if (type >= 0 && type <= 255) {
        if (isprint(type)) sprintf(result, "char '%c'", type);
        else               sprintf(result, "control char %d", type);
    } else {
        switch (type)
        {
            case TOKEN_IDENT:   strcat(result, TOKEN_IDENT_STRING);   break;
            case TOKEN_NUMBER:  strcat(result, TOKEN_NUMBER_STRING);  break;
            case TOKEN_STRING:  strcat(result, TOKEN_STRING_STRING);  break;
            case TOKEN_TRUE:    strcat(result, TOKEN_TRUE_STRING);    break;
            case TOKEN_FALSE:   strcat(result, TOKEN_FALSE_STRING);   break;
            case TOKEN_SET:     strcat(result, TOKEN_SET_STRING);     break;
            case TOKEN_VAR:     strcat(result, TOKEN_VAR_STRING);     break;
            case TOKEN_DEF:     strcat(result, TOKEN_DEF_STRING);     break;
            case TOKEN_SNIPPET: strcat(result, TOKEN_SNIPPET_STRING); break;
            case TOKEN_NEWLINE: strcat(result, TOKEN_NEWLINE_STRING); break;
            case TOKEN_EOF:     strcat(result, TOKEN_DEF_STRING);     break;
            default:            return NULL;
        }
    }
    return result;
}

static_assert(ACTUAL_TOKENS_COUNT == 11, "token_type_and_value_as_string");
char *token_type_and_value_as_string(Token token)
{
    const size_t n = 128;
    char *result = malloc(sizeof(char)*n);
    char *type = token_type_as_string(token.type);
    if (token.type >= 0 && token.type <= 255) {
        if (isprint(token.type)) sprintf(result, "%s", type);
        else                     sprintf(result, "%s", type);
    } else {
        switch (token.type)
        {
            case TOKEN_IDENT: sprintf(result, "%s `%s`", type, token.string_value); break;
            case TOKEN_NUMBER: sprintf(result, "%s `%d`", type, token.number_value); break;
            case TOKEN_STRING: {
                snprintf(result, n-1, "%s \"%s\"", type, token.string_value);
                result[n] = '\0';
            } break;

            case TOKEN_TRUE:
            case TOKEN_FALSE:
            case TOKEN_SET:
            case TOKEN_VAR:
            case TOKEN_DEF:
            case TOKEN_SNIPPET:
            case TOKEN_NEWLINE:
            case TOKEN_EOF:
                sprintf(result, "%s", type);
                break;
            default: print_error_and_exit("Unreachable");
        }
    }
    free(type);
    return result;
}

void token_log(Token token)
{
    char *string = token_type_and_value_as_string(token);
    log_this(LOC_FMT" %s", LOC_ARG(token.loc), string);
    free(string);
}

#define NO_LOG NULL
#define EXTRA_NEWLINE true
bool expect_token_to_be_of_type(Token t, TokenType type, String *s_log)
{
    if (t.type == type) return true;

    if (s_log) {
        char *tokstrtype = token_type_as_string(type);
        char *tokstrval = token_type_and_value_as_string(t);
        s_push_fstr(s_log, LOC_FMT"\n- ERROR: expecting %s but got %s\n", LOC_ARG(t.loc),
                tokstrtype, tokstrval);
        free(tokstrtype);
        free(tokstrval);
    }
    return false;
}

bool expect_token_to_be_of_type_extra_newline(Token t, TokenType type, String *s_log)
{
    if (expect_token_to_be_of_type(t, type, s_log)) return true;
    if (s_log) s_push(s_log, '\n');
    return false;
}

bool expect_token_to_be_of_types(Token t, TokenType *types, size_t n, String *s_log)
{
    if (!types || n == 0) return true;

    for (size_t i = 0; i < n; i++)
        if (t.type == types[i]) return true;

    if (s_log) {
        s_push_fstr(s_log, LOC_FMT"\n- ERROR: expecting one of the following tokens ", LOC_ARG(t.loc));
        for (size_t i = 0; i < n; i++) {
            char *tokstrtype = token_type_as_string(types[i]);
            s_push_fstr(s_log, "`%s`", tokstrtype);
            free(tokstrtype);
            if (i < n-1) s_push_fstr(s_log, ", ");
        }
        char *tokstrval = token_type_and_value_as_string(t);
        s_push_fstr(s_log, " but got %s\n", tokstrval);
        free(tokstrval);
    }
    return false;
}

bool expect_token_to_be_of_types_extra_newline(Token t, TokenType *types, size_t n, String *s_log)
{
    if (expect_token_to_be_of_types(t, types, n, s_log)) return true;
    if (s_log) s_push(s_log, '\n');
    return false;
}

typedef struct
{
    Token *items;
    size_t count;
    size_t capacity;
} Tokens;

typedef struct
{
    char *str;
    Location loc;
    Token token;
} Lexer;

Lexer lexer_create_from_path(char *path)
{
    Lexer l = {0};
    l.loc.path = path;
    l.str = read_file(path);
    return l;
}

Lexer lexer_create_from_string(char *string)
{
    Lexer l = {0};
    l.str = string;
    return l;
}


void lexer_trim_left(Lexer *l)
{
    if (!l->str) return;
    while (*l->str && isblank(*l->str)) {
        l->loc.col++;
        l->str++;
    }
}

void lexer_skip_comment(Lexer *l)
{
    while (l->str && *l->str && *l->str != '\n') {
        l->str++;
        l->loc.col++;
    }
}

bool lexer_get_string(Lexer *l)
{
    l->str++;
    l->loc.col++;
    l->token.type = TOKEN_STRING;
    char *begin = l->str;
    char *end = strchr(begin, '"');
    if (end == NULL) return false;
    ptrdiff_t len = end - begin;
    l->token.string_value = malloc(sizeof(char)*len + 1);
    strncpy(l->token.string_value, begin, len);
    l->token.string_value[len] = '\0';
    l->str = end+1;
    l->loc.col += len;
    return true;
}

static_assert(ACTUAL_TOKENS_COUNT == 11, "lexer_next");
bool lexer_next(Lexer *l)
{
    if (!l->str) return false;

    lexer_trim_left(l);

    char c = *l->str;
    if (c == '\0') return false;

    l->token.loc = l->loc;

    if (c == '\n') {
        l->token.type = TOKEN_NEWLINE;
        l->loc.col = 0;
        l->loc.row++;
        l->str++;
    } else if (c == '/') {
        l->str++;
        char second = *l->str;
        if (second != '/') {
            l->loc.col++;
            l->token.type = c;
        } else {
            l->str++;
            l->loc.col += 2;
            lexer_skip_comment(l);
            return lexer_next(l);
        }
    } else if (isalpha(c) || c == '_') {
        char *begin = l->str;
        size_t len = 0;
        while (isalnum(c) || c == '_') {
            l->str++;
            len++;
            c = *l->str;
        }
        l->loc.col += len;
        if      (strneq(begin, TOKEN_SET_STRING,     len)) l->token.type = TOKEN_SET;
        else if (strneq(begin, TOKEN_VAR_STRING,     len)) l->token.type = TOKEN_VAR;
        else if (strneq(begin, TOKEN_DEF_STRING,     len)) l->token.type = TOKEN_DEF;
        else if (strneq(begin, TOKEN_SNIPPET_STRING, len)) l->token.type = TOKEN_SNIPPET;
        else if (strneq(begin, TOKEN_TRUE_STRING,    len)) l->token.type = TOKEN_TRUE;
        else if (strneq(begin, TOKEN_FALSE_STRING,   len)) l->token.type = TOKEN_FALSE;
        else {
            l->token.type = TOKEN_IDENT;
            l->token.string_value = malloc(sizeof(char)*len + 1);
            strncpy(l->token.string_value, begin, len);
            l->token.string_value[len] = '\0';
        }
    } else if (isdigit(c) || c == '-') {
        char *end;
        long n = strtol(l->str, &end, 10);
        l->token.number_value = n;
        l->loc.col += end - l->str;
        l->str = end;
        l->token.type = TOKEN_NUMBER;
    } else if (c == '"') {
        lexer_get_string(l);
    } else {
        l->token.type = c;
        l->str++;
        l->loc.col++;
    }
    return true;
}

static_assert(ACTUAL_TOKENS_COUNT == 11, "lexer_get_current_token");
Token lexer_get_current_token(Lexer *l)
{
    Token token = l->token;
    if (l->loc.path) token.loc.path = strdup(l->loc.path);

    if (token.type >= 0 && token.type <= 255) return token;

    switch (token.type)
    {
    case TOKEN_NUMBER:
    case TOKEN_TRUE:
    case TOKEN_FALSE:
    case TOKEN_SET:
    case TOKEN_VAR:
    case TOKEN_DEF:
    case TOKEN_SNIPPET:
    case TOKEN_NEWLINE:
    case TOKEN_EOF:
        break;

    case TOKEN_IDENT:
    case TOKEN_STRING:
        token.string_value = strdup(l->token.string_value);
        break;

    default: print_error_and_exit("Unreachable token type %u in lexer_get_current_token", token.type);
    }

    return token;
}

static_assert(ACTUAL_TOKENS_COUNT == 11, "free_token");
void free_token(Token *token)
{
    if (token->loc.path) free(token->loc.path);
    switch (token->type)
    {
    case TOKEN_IDENT:
    case TOKEN_STRING:
        free(token->string_value);
        break;

    default: {}
    }
}

void free_tokens(Tokens *tokens)
{
    da_foreach (*tokens, Token, token) {
        free_token(token);
    }
    da_free(tokens);
}

void lexer_free_current_token(Lexer *l)
{
    l->token.loc.path = NULL;
    free_token(&l->token);
}

Tokens lex(Lexer *l)
{
    Tokens tokens = {0};
    while (lexer_next(l)) {
        Token token = lexer_get_current_token(l);
        da_push(&tokens, token);
        //token_log(token);
        lexer_free_current_token(l);
    }
    Token eof = { .type = TOKEN_EOF };
    da_push(&tokens, eof);
    return tokens;
}

Tokens lex_file(char *path)
{
    Lexer lexer = lexer_create_from_path(path); 
    return lex(&lexer);
}

Tokens lex_string(char *string)
{
    Lexer lexer = lexer_create_from_string(string); 
    return lex(&lexer);
}

/// END Commands

/// BEGIN Config

typedef enum
{
    FIELD_BOOL,
    FIELD_INT,
    FIELD_UINT,
    FIELD_STRING,
    FIELD_LIMITED_STRING,
    //FIELD_LIMITED_UINT,
    //FIELD_RANGE_UINT,
    FIELD_TYPES_COUNT,
} FieldType;

static_assert(FIELD_TYPES_COUNT == 5, "fieldtype_to_string");
char *fieldtype_to_string(FieldType type)
{
    switch (type)
    {
        case FIELD_BOOL:           return "bool";
        case FIELD_INT:            return "int";
        case FIELD_UINT:           return "uint";
        case FIELD_STRING:         return "string";
        case FIELD_LIMITED_STRING: return "limited string";
        default:
            print_error_and_exit("Unreachable\n");
    }
}

typedef int LimitedStringIndex;

typedef struct
{
    const char *name;
    FieldType type;
    union {
        struct { // FIELD_BOOL
            bool *bool_ptr;
            bool bool_default;
        };
        struct { // FIELD_INT
            int *int_ptr;   
            int int_default;   
        };
        struct { // FIELD_UINT
            size_t *uint_ptr;   
            size_t uint_default;   
        };
        struct { // FIELD_STRING
            char **string_ptr;
            char *string_default;
        };
        struct { // FIELD_LIMITED_STRING
            LimitedStringIndex *limited_string_ptr;
            LimitedStringIndex limited_string_default;
            Strings limited_string_valid_values;
        };
    };
} ConfigField;

typedef struct
{
    ConfigField *items;
    size_t count;
    size_t capacity;
} ConfigFields;

#define macro_make_config_field_bool(field_name) \
    ((ConfigField){                                \
        .name = #field_name,                       \
        .type = FIELD_BOOL,                        \
        .bool_ptr = &editor.config.field_name,     \
        .bool_default = default_##field_name       \
    })

#define macro_make_config_field_int(field_name) \
    ((ConfigField){                               \
        .name = #field_name,                      \
        .type = FIELD_INT,                        \
        .int_ptr = &editor.config.field_name,     \
        .int_default = default_##field_name       \
    })

#define macro_make_config_field_uint(field_name) \
    ((ConfigField){                                \
        .name = #field_name,                       \
        .type = FIELD_UINT,                        \
        .uint_ptr = &editor.config.field_name,     \
        .uint_default = default_##field_name       \
    })

#define macro_make_config_field_string(field_name) \
    ((ConfigField){                                  \
        .name = #field_name,                         \
        .type = FIELD_STRING,                        \
        .string_ptr = &editor.config.field_name,     \
        .string_default = default_##field_name       \
    })

#define macro_make_config_field_limited_string(field_name)                   \
    ((ConfigField){                                                            \
        .name = #field_name,                                                   \
        .type = FIELD_LIMITED_STRING,                                          \
        .limited_string_ptr = (LimitedStringIndex *)&editor.config.field_name, \
        .limited_string_default = default_##field_name,                        \
        .limited_string_valid_values = valid_values_##field_name               \
    })

void save(void)
{
    String save_buf = {0};
    da_foreach(editor.rows, Row, row) {
        s_push_str(&save_buf, row->content.items, row->content.count);
        s_push(&save_buf, '\n');
    }

    // TODO: make the user decide the name of the file if not set
    // - probabilmente devo fare un sistema che permetta di cambiare l'inizio della command line (ora e' sempre Command:) e poi fare cose diverse una volta che si e' premuto ENTER
    if (!editor.filepath) {
        write_message("TODO: make the user decide the name of the file", editor.filename);
        return;
    }
    int fd = open(editor.filepath, O_RDWR|O_CREAT, 0644);
    if (fd == -1) goto writeerr;

    /* Use truncate + a single write(2) call in order to make saving
     * a bit safer, under the limits of what we can do in a small editor. */
    ssize_t len = save_buf.count;
    if (ftruncate(fd, len) == -1) goto writeerr;
    if (write(fd, save_buf.items, len) != len) goto writeerr;

    close(fd);
    s_free(&save_buf);
    editor.dirty = 0;
    write_message("%zu bytes written on disk", len);
    return;

writeerr:
    s_free(&save_buf);
    if (fd != -1) close(fd);
    write_message("Can't save! I/O error: %s", strerror(errno));
}

// TODO: argomento per salvare il file con un nome
void builtin_save(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;
    save();
}

void ncurses_end(void)
{
    // TODO:
    // - restore original colors
    // - restore original terminal options (maybe not needed)
    curs_set(1);
    clear();
    refresh();
    endwin();
    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
}

_Noreturn void quit()
{
    ncurses_end();
    exit(0);
}

bool can_quit(void)
{
    if (!editor.dirty || editor.current_quit_times == 1) return true;

    editor.current_quit_times--;
    write_message("Session is not saved. If you really want to quit press CTRL-q %zu more time%s.",
            editor.current_quit_times, editor.current_quit_times == 1 ? "" : "s");
    return false;
}

void builtin_quit(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;
    if (can_quit()) quit();
}

void builtin_save_and_quit(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;
    builtin_save(cmd, args); quit();
}

void builtin_force_quit(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;
    quit();
}

void move_cursor_up_internal(void)
{
    if (editor.cursor.y == 0 && editor.page != 0) {
        editor.offset--;
        // TODO: deve cambiare anche la pagina, ma come?
    }
    if (editor.cursor.y - 1 > 0) editor.cursor.y--;
    else editor.cursor.y = 0;
}

void move_cursor_up(void)
{
    move_cursor_up_internal();
    if (editor.multicursor.is_enabled) {
        Cursor saved = editor.cursor;
        da_foreach (editor.multicursor, Cursor, cursor) {
            editor.cursor = *cursor;
            move_cursor_up_internal();
            *cursor = editor.cursor;
        }
        editor.cursor = saved;
    }
}

void move_cursor_down_internal(void)
{ 
    //if ((editor.offset+N_OR_DEFAULT(1) / N_PAGES) > editor.page) editor.page += (editor.offset+N_OR_DEFAULT(1)) / N_PAGES; // TODO: No, non funziona cosi' devo modificare l'offset in questo caso
    if (editor.cursor.y == win_main.height-1 && editor.page != N_PAGES-1) {
        editor.offset++;
        // TODO: deve cambiare anche la pagina, ma come?
    }
    if (editor.cursor.y + 1 < win_main.height-1) editor.cursor.y++;
    else editor.cursor.y = win_main.height-1;
    //if (editor.offset < editor.rows.count-N_OR_DEFAULT(0)-1) editor.offset += N_OR_DEFAULT(1); 
    //else editor.offset = editor.rows.count-1;
}

void move_cursor_down(void)
{
    move_cursor_down_internal();
    if (editor.multicursor.is_enabled) {
        Cursor saved = editor.cursor;
        da_foreach (editor.multicursor, Cursor, cursor) {
            editor.cursor = *cursor;
            move_cursor_down_internal();
            *cursor = editor.cursor;
        }
        editor.cursor = saved;
    }
}

void move_cursor_left_internal(void)
{
    if (editor.in_cmd) {
        if (editor.cmd_pos > 0) editor.cmd_pos--;
        else editor.cmd_pos = 0;
    } else {
        if (editor.cursor.x > 0) editor.cursor.x--;
        else editor.cursor.x = 0;
    }
}

void move_cursor_left(void)
{
    move_cursor_left_internal();
    if (editor.multicursor.is_enabled) {
        Cursor saved = editor.cursor;
        da_foreach (editor.multicursor, Cursor, cursor) {
            editor.cursor = *cursor;
            move_cursor_left_internal();
            *cursor = editor.cursor;
        }
        editor.cursor = saved;
    }
}


void move_cursor_right_internal(void)
{
    if (editor.in_cmd) {
        if (editor.cmd_pos < editor.cmd.count-1) editor.cmd_pos++;
        else editor.cmd_pos = editor.cmd.count;
    } else {
        if (editor.cursor.x < win_main.width-1) editor.cursor.x++;
        else editor.cursor.x = win_main.width-1;
    }
}

void move_cursor_right(void)
{
    move_cursor_right_internal();
    if (editor.multicursor.is_enabled) {
        Cursor saved = editor.cursor;
        da_foreach (editor.multicursor, Cursor, cursor) {
            editor.cursor = *cursor;
            move_cursor_right_internal();
            *cursor = editor.cursor;
        }
        editor.cursor = saved;
    }
}


void builtin_move_cursor(Command *cmd, CommandArgs *args) 
{
    (void)cmd;

    if (args->count != 2) {
        write_message("COMMAND ERROR: command %s takes 2 arguments, but got %zu", args->count);
        return;
    }

    size_t x = args->items[0].int_value;
    size_t y = args->items[1].int_value;

    if (x > 0) for (size_t i = 0; i < x; i++) move_cursor_right();
    else       for (int i = x; i >= 0; i++) move_cursor_left();

    if (y > 0) for (size_t i = 0; i < y; i++) move_cursor_down();
    else       for (int i = y; i >= 0; i++) move_cursor_up();
}

void builtin_move_line_up()
{
    size_t y = CURRENT_Y_POS;
    if (y == 0) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y-1];
    editor.rows.items[y-1] = tmp;
    move_cursor_up();
    editor.dirty++;
}

void builtin_move_line_down()
{
    size_t y = CURRENT_Y_POS;
    if (y == win_main.height-1) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y+1];
    editor.rows.items[y+1] = tmp;
    move_cursor_down();
    editor.dirty++;
}

void insert_char_at(Row *row, size_t at, int c)
{
    if (at > row->content.count) {
        size_t padlen = at-row->content.count;
        for (size_t i = 0; i < padlen; i++)
            s_push(&row->content, ' ');
        s_push(&row->content, c);
    } else {
        s_insert(&row->content, c, at);
    }
}

#define WITH_LOCATION true
Command parse_commands_list(Tokens tokens, size_t *index, String *log, bool with_location)
{
    //log_this("Parsing commands list");
    Command cmd = {0};
    size_t i = *index;
    Token tok_it = tokens.items[i];

    bool can_continue(void) { return i < tokens.count && tok_it.type != TOKEN_EOF && tok_it.type != TOKEN_NEWLINE; }

    while (can_continue()) {
        size_t n = 1;
        if (tok_it.type == TOKEN_NUMBER) {
            if (tok_it.number_value <= 0) {
                if (log) {
                    if (with_location) {
                        s_push_fstr(log, LOC_FMT"\n- ERROR: multiplicity number should be > 0 (got %d)\n\n",
                                LOC_ARG(tok_it.loc), tok_it.number_value);
                    } else s_push_fstr(log, "ERROR: multiplicity number should be > 0 (got %d)\n", tok_it.number_value);
                }
                while (can_continue()) {
                    i++;
                    tok_it = tokens.items[i];
                }
                cmd.type = ERROR;
                break;
            }
            n = tok_it.number_value;
            i++;
            tok_it = tokens.items[i];
        }
        if (!expect_token_to_be_of_type_extra_newline(tok_it, TOKEN_IDENT, log)) {
            while (can_continue()) {
                i++;
                tok_it = tokens.items[i];
            }
            cmd.type = ERROR;
            break;
        }

        char *subcmd_name = tok_it.string_value;
        //log_this("subcommand name: %s", subcmd_name);
        CommandType subcmd_type = get_command_type_from_string(subcmd_name);
        //log_this("subcommand type: %s", get_command_type_as_cstr(subcmd_type));
        if (subcmd_type == UNKNOWN) {
            if (log) {
                if (with_location) {
                    s_push_fstr(log, LOC_FMT"\n- ERROR: unknown command `%s`\n\n",
                            LOC_ARG(tok_it.loc), subcmd_name);
                } else s_push_fstr(log, "ERROR: unknown command `%s`\n", subcmd_name);
            }
            while (can_continue()) {
                i++;
                tok_it = tokens.items[i];
            }
            cmd.type = ERROR;
            break;
        }

        Command subcmd = {
            .type = subcmd_type,
            .n = n
        };

        i++;
        tok_it = tokens.items[i];

        if (tok_it.type == '(') {
            //log_this("TODO: parse arguments list for command `%s`", subcmd_name);
            i++;
            tok_it = tokens.items[i];
            while (can_continue() && tok_it.type != ')') {
                // TODO
                i++;
                tok_it = tokens.items[i];
            } 
            if (tok_it.type != ')') {
                // TODO: error
            }
            i++;
            tok_it = tokens.items[i];
        }

        subcmd.name = strdup(subcmd_name);
        da_push(&cmd.subcmds, subcmd);
    }
    *index = i;
    //log_this("Done parsing command list");
    return cmd;
}

void execute_command(Command *cmd, CommandArgs *args);
void insert_char_internal(char c)
{
    if (editor.in_cmd) {
        if (c == '\n') {
            s_push_null(&editor.cmd);
            char *cmd_str = editor.cmd.items;
            cs_push(&editor.commands_history, cmd_str);
            editor.in_cmd = false;
            s_clear(&editor.cmd);
            editor.cmd_pos = 0;

            //log_this("Lexing command line `%s`", cmd_str);
            CommandArgs runtime_args = {0};
            Tokens tokens = lex_string(cmd_str);
            //log_this("Lexed command line:");
            da_foreach (tokens, Token, tok) {
                char *tokstrval = token_type_and_value_as_string(*tok);
                //log_this(" - %s", tokstrval);
                free(tokstrval);
            }

            size_t index = 0;
            String parse_log = {0};
            Command cmd_from_line = parse_commands_list(tokens, &index, &parse_log, !WITH_LOCATION);
            bool error = cmd_from_line.type == ERROR;
            if (error) {
                s_push_null(&parse_log);
                write_message("Command: %s", parse_log.items); 
            }
            if (parse_log.items) s_free(&parse_log);
            if (error) return;

            //log_this("Ready to execute command from line -> type %u", cmd_from_line.type);
            for (size_t i = 0; i < cmd_from_line.subcmds.count; i++) {
                //log_this("- %s", cmd_from_line.subcmds.items[i].name);
            }

            cmd_from_line.name = "command from line";
            cmd_from_line.type = COMMAND_FROM_LINE;
            cmd_from_line.n = 1; // TODO: trovare un modo
            execute_command(&cmd_from_line, &runtime_args);
            free_command(&cmd_from_line);
            free_command_args(&runtime_args);
        } else {
            da_push(&editor.cmd, c);
            editor.cmd_pos++;
        }
        return;
    }

    size_t y = CURRENT_Y_POS;
    size_t x = CURRENT_X_POS;

    if (c == '\n') {
        if (y == editor.rows.count) {
            Row newrow = {0};
            da_push(&editor.rows, newrow);
        } else {
            Row *row = CURRENT_ROW;
            if (x >= row->content.count) x = row->content.count;
            if (x == 0) {
                Row newrow = {0};
                da_insert(&editor.rows, newrow, y);
            } else {
                /* We are in the middle of a line. Split it between two rows. */
                Row newrow = {0};
                s_push_str(&newrow.content, row->content.items+x, row->content.count-x);
                da_insert(&editor.rows, newrow, y+1);
                row->content.count = x;
            }
        }
        if (editor.cursor.y == win_main.height-1) editor.offset++;
        else editor.cursor.y++;
        editor.cursor.x = 0;
    } else {
        if (y >= editor.rows.count) {
            while (editor.rows.count <= y) {
                Row newrow = {0};
                da_push(&editor.rows, newrow);
            }
        }
        insert_char_at(CURRENT_ROW, x, c);
        editor.cursor.x++;
    }
    editor.dirty++;
}

void insert_char(char c)
{
    if (!editor.multicursor.is_enabled || editor.in_cmd) {
        insert_char_internal(c);
        return;
    }

    Cursor saved_main = editor.cursor;

    for (size_t i = 0; i < editor.sorted_multicursor.count; i++) {
        Cursor *current = editor.sorted_multicursor.items[i];
        editor.cursor = (current == &editor.cursor) ? saved_main : *current;
        insert_char_internal(c);
        if (current == &editor.cursor) saved_main = editor.cursor;
        else {
            *current = editor.cursor;
            editor.cursor = saved_main;
        }

        for (size_t j = 0; j < i; j++) {
            Cursor *processed = editor.sorted_multicursor.items[j];
            
            if (c == '\n') {
                // If we inserted a newline, shift rows below down
                if (processed->y > current->y) {
                    processed->y++; 
                    // Note: You might want to reset x to 0 or keep relative, 
                    // strictly editor.cursor.x resets on newline in insert_internal
                } 
                // If the processed cursor was on the same line (to the right),
                // it essentially got moved to the new line.
                else if (processed->y == current->y - 1) { // -1 because current->y incremented
                     // Complex case: intra-line split.
                     // For simplicity in this snippet, we assume standard behavior.
                     processed->y++;
                     processed->x -= current->x; // Shift X relative to split
                }
            } else {
                // Standard char: shift X if on same row
                // current->y matches processed->y because we sorted reverse.
                // If they are on the same line, processed MUST be to the right.
                if (processed->y == current->y) {
                    processed->x++;
                }
            }
            if (processed == &editor.cursor) saved_main = *processed;
        }
    }
    editor.cursor = saved_main;
}

void insert_cstr(char *string)
{
    const size_t len = strlen(string);
    for (size_t i = 0; i < len; i++) {
        insert_char(string[i]);
    }
}

void builtin_insert(Command *cmd, CommandArgs *args)
{
    if (!expect_n_arguments(cmd, args, 1)) return;
    char *string = args->items[0].string_value;
    insert_cstr(string);
}

void builtin_date(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date[64];
    if (strftime(date, sizeof(date), "%c", tm) == 0) {
        write_message("Could not get date");
        return;
    }
    for (size_t i = 0; i < strlen(date); i++) {
        insert_char(date[i]);
    }
}

void builtin_goto_line(Command *cmd, CommandArgs *args)
{
    if (!expect_n_arguments(cmd, args, 1)) return;
    size_t line = args->items[0].uint_value;
    write_message("TODO: goto line %zu", line);
}

#define SNIPPET_BODY_INDENTATION 4
bool parse_snippet_body(Token token_body, Snippet *snippet, String *log)
{
    char *body = token_body.string_value;
    assert(body);
    size_t len = strlen(body);
    if (len == 0) {
        s_push_fstr(log, LOC_FMT"\n- ERROR: empty snippet body\n\n");
        return false;
    }
    snippet->is_inline = strchr(body, '\n') == NULL;
    if (!snippet->is_inline) {
        if (body[0] != '{' || body[len-1] != '}') {
            s_push_fstr(log, LOC_FMT"\n- ERROR: multiline snippet body must be enclosed in curly brackets\n\n",
                    LOC_ARG(token_body.loc));
            return false;
        }
        body[len-1] = '\0';
        body++;
        while (*body && isblank(*body)) body++;
        if (*body != '\n') {
            s_push_fstr(log, LOC_FMT"\n- ERROR: newline needed after '{' in multiline snippet definition\n\n",
                    LOC_ARG(token_body.loc));
            return false;
        }
        body++;
        len = strlen(body);
        if (len == 0) {
            s_push_fstr(log, LOC_FMT"\n- WARNING: empty snippet body\n\n", LOC_ARG(token_body.loc));
            return false;
        }
    }

    Location loc = {
        .row = token_body.loc.row+1,
        .col = 0,
        .path = token_body.loc.path
    };
    String parsed_body = {0};
    size_t i = 0;
    Cursor cursor = {0};
    SnippetMarks marks_da = {0};

    while (i < len) {
        if (!snippet->is_inline) {
            if (body[i] == '\n') {
                // NOTE: empty line cannot have indentation
            } else if (body[i] == '\t') {
                body++;
                loc.col++;
            } else {
                // TODO: maybe just calculate the indentation of the first non empty line and use it afterwards
                size_t indentation = 0;
                while (i < len && indentation < SNIPPET_BODY_INDENTATION && body[i] == ' ') {
                    i++;
                    indentation++;
                    loc.col++;
                }
                if (indentation < SNIPPET_BODY_INDENTATION && body[i] != '\n') {
                    s_push_fstr(log, LOC_FMT"\n- ERROR: incorrect indentation (should be a tab or %d spaces but is just %zu space%s)\n\n", LOC_ARG(loc), SNIPPET_BODY_INDENTATION, indentation, indentation == 1 ? "" : "s");
                    return false;
                }
            }
        }
        while (i < len && body[i] != '\n') {
            if (body[i] == '\\' && i+1 < len && body[i+1] == '$') {
                i++;
                loc.col++;
            } else if (body[i] == '$') {
                //log_this("Parsing snippet mark at "LOC_FMT, LOC_ARG(loc));
                SnippetMark mark = {
                    .name = NULL,
                    .cursor = cursor,
                    .is_primary = true
                };
                i++;
                loc.col++;
                if (i >= len || body[i] != '{') {
                    da_push(&marks_da, mark);
                    //log_this("snippet mark without name at (%zu, %zu)", mark.cursor.x, mark.cursor.y);
                    continue;
                }
                i++;
                loc.col++;
                char *name = body+i;
                while (i < len-1 && body[i] != '\n' && body[i] != '}') {
                    i++;
                    loc.col++;
                }
                if (i >= len-1 || body[i] == '\n') {
                    s_push_fstr(log, LOC_FMT"\n- ERROR: unclosed named mark bracket\n\n", LOC_ARG(loc));
                    return false;
                }
                body[i] = '\0';
                mark.name = strdup(name);
                da_foreach (marks_da, SnippetMark, m) {
                    if (streq(m->name, mark.name)) {
                        mark.is_primary = false;
                        break;
                    }
                }
                da_push(&marks_da, mark);
                //log_this("snippet mark `%s` at (%zu, %zu)", mark.name, mark.cursor.x, mark.cursor.y);
                i++;
            }
            s_push(&parsed_body, body[i]);
            i++;
            loc.col++;
            cursor.x++;
        }
        if (!snippet->is_inline) {
            if (i >= len) {
                s_push_fstr(log, LOC_FMT"\n- ERROR: unexpected end of snippet body, it should end with a newline\n\n",
                        LOC_ARG(loc));
                return false;
            }
            s_push(&parsed_body, '\n');
            loc.col = 0;
            i++;
            loc.row++;
            cursor.x = 0;
            cursor.y++;
        } else {
            i++;
            loc.col++;
            cursor.x++;
        }
    }
    if (parsed_body.items[parsed_body.count-1] == '\n')
        s_pop(&parsed_body);
    s_push_null(&parsed_body);
    body = strdup(parsed_body.items);
    s_free(&parsed_body);

    snippet->marks_count = marks_da.count;
    snippet->marks = malloc(sizeof(SnippetMark)*marks_da.count);
    for (size_t m = 0; m < marks_da.count; m++)
        snippet->marks[m] = marks_da.items[m];

    da_free(&marks_da);
    snippet->body = body;
    return true;
}

int read_key(); // Forward declaration
void load_config()
{
    char *home = getenv("HOME");
    if (home == NULL) {
        print_error_and_exit("Env variable HOME not set\n");
    }

    static_assert(CONFIG_FIELDS_COUNT == 5, "Set defaults and valid values for all config fields");
    const size_t default_quit_times = 3;
    const bool default_tab_to_spaces = true;
    const size_t default_tab_spaces_number = 4;

    const ConfigLineNumbers default_line_numbers = LN_REL;
    Strings valid_values_line_numbers = {0};
    {
        char *values[] = {"no", "absolute", "relative"};
        da_push_many(&valid_values_line_numbers, values, 3);
    }

    const ConfigLogLevel default_configlog_level = CONFIGLOG_ALL;
    Strings valid_values_configlog_level = {0};
    {
        char *values[] = {"all", "warning", "error"};
        da_push_many(&valid_values_configlog_level, values, 3);
    }

    String config_log = {0};

    const char *config_path = ".config/editor/config.pisquy";
    char full_config_path[256] = {0};
    sprintf(full_config_path, "%s/%s", home, config_path);
    FILE *config_file = fopen(full_config_path, "r");
    if (config_file == NULL) {
        s_push_fstr(&config_log, "WARNING: config file not found at %s\n\n", full_config_path);
        config_file = fopen(full_config_path, "w+");
        if (config_file == NULL) {
            s_push_fstr(&config_log, "ERROR: could not create config file\n\n");
            goto fail;
        }

        static_assert(CONFIG_FIELDS_COUNT == 5, "Write defaults and descriptions for all config fields in fresh config file");
        // TODO: make the descriptions macro/const
        fprintf(config_file, "set quit_times = %zu\t// times you need to press CTRL-q before exiting without saving\n",
                default_quit_times);
        fprintf(config_file,
                "set line_numbers = %s\t// display line numbers (don't, absolute or relative to the current line)\n",
                valid_values_line_numbers.items[default_line_numbers]);
        fprintf(config_file,
                "set tab_to_spaces = %s\t// tabs are inserted as spaces\n", BOOL_AS_CSTR(default_tab_to_spaces));
        fprintf(config_file,
                "set tab_spaces_number = %zu\t// how many spaces a tab expands to (works only if tab_to_spaces is set to 'true')\n", default_tab_spaces_number);
        fprintf(config_file,
                "set configlog_level = %s\t// log level of config (all, warning, error)\n",
                valid_values_configlog_level.items[default_configlog_level]);

        s_push_fstr(&config_log, "NOTE: default config file has been created\n\n");
        rewind(config_file);
    }

    static_assert(CONFIG_FIELDS_COUNT == 5, "Add all config fields to remaining_fields");
    ConfigFields remaining_fields = {0};
    da_push(&remaining_fields, macro_make_config_field_uint(quit_times));
    da_push(&remaining_fields, macro_make_config_field_limited_string(line_numbers));
    da_push(&remaining_fields, macro_make_config_field_bool(tab_to_spaces));
    da_push(&remaining_fields, macro_make_config_field_uint(tab_spaces_number));
    da_push(&remaining_fields, macro_make_config_field_limited_string(configlog_level));
    
    //if (DEBUG) {
    //    for (size_t i = 0; i < remaining_fields.count; i++) {
    //        const ConfigField *field = &remaining_fields.items[i];
    //        s_push_fstr(&config_log, "Field { name: `%s`, type: `%s` }\n",
    //                field->name, fieldtype_to_string(field->type));
    //    }
    //    s_push(&config_log, '\n');
    //}

    commands = (Commands){0};
    static_assert(BUILTIN_CMDS_COUNT == 14, "Add all builtin commands in commands");
    add_builtin_command(SAVE,              BUILTIN_SAVE,              builtin_save,              NULL);
    add_builtin_command(QUIT,              BUILTIN_QUIT,              builtin_quit,              NULL);
    add_builtin_command(SAVE_AND_QUIT,     BUILTIN_SAVE_AND_QUIT,     builtin_save_and_quit,     NULL);
    add_builtin_command(FORCE_QUIT,        BUILTIN_FORCE_QUIT,        builtin_force_quit,        NULL);

    CommandArgs baked_args = {0};
    add_command_arg_int(&baked_args, "x", 0);
    add_command_arg_int(&baked_args, "y", -1);
    add_builtin_command(MOVE_CURSOR_UP, BUILTIN_MOVE_CURSOR_UP, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    add_command_arg_int(&baked_args, "x", 0);
    add_command_arg_int(&baked_args, "y", 1);
    add_builtin_command(MOVE_CURSOR_DOWN, BUILTIN_MOVE_CURSOR_DOWN, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    add_command_arg_int(&baked_args, "x", -1);
    add_command_arg_int(&baked_args, "y", 0);
    add_builtin_command(MOVE_CURSOR_LEFT, BUILTIN_MOVE_CURSOR_LEFT, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    add_command_arg_int(&baked_args, "x", 1);
    add_command_arg_int(&baked_args, "y", 0);
    add_builtin_command(MOVE_CURSOR_RIGHT, BUILTIN_MOVE_CURSOR_RIGHT, builtin_move_cursor, &baked_args);

    add_builtin_command(MOVE_LINE_UP,      BUILTIN_MOVE_LINE_UP,      builtin_move_line_up,      NULL);
    add_builtin_command(MOVE_LINE_DOWN,    BUILTIN_MOVE_LINE_DOWN,    builtin_move_line_down,    NULL);
    add_builtin_command(INSERT,            BUILTIN_INSERT,            builtin_insert,            NULL);
    add_builtin_command(DATE,              BUILTIN_DATE,              builtin_date,              NULL);
    add_builtin_command(GOTO_LINE,         BUILTIN_GOTO_LINE,         builtin_goto_line,         NULL);

    free_command_args(&baked_args);

    Tokens tokens = lex_file(full_config_path);
    ConfigFields inserted_fields = {0};

    for (size_t i = 0; i < tokens.count; i++) {
        Token first_token = tokens.items[i];

        switch (first_token.type)
        {
        case TOKEN_SET: {
            i++;
            Token token_field_name = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_field_name, TOKEN_IDENT, &config_log)) continue;
            i++;
            Token token_equals = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_equals, '=', &config_log)) continue;
            char *field_name = token_field_name.string_value;
            i++;
            Token token_field_value = tokens.items[i];

            bool found = false;
            size_t j = 0;
            while (!found && j < remaining_fields.count) {
                if (streq(field_name, remaining_fields.items[j].name)) {
                    found = true;
                    ConfigField removed_field = remaining_fields.items[j];
                    da_remove(&remaining_fields, j);
                    da_push(&inserted_fields, removed_field);
                    switch (removed_field.type)
                    {
                        case FIELD_BOOL: {
                            bool value;
                            const TokenType TOKEN_BOOL[2] = {TOKEN_TRUE, TOKEN_FALSE};
                            if (!expect_token_to_be_of_types(token_field_value, TOKEN_BOOL, 2, &config_log)) {
                                s_push_fstr(&config_log, "- NOTE: defaulted to `%s`\n\n",
                                        BOOL_AS_CSTR(removed_field.bool_default));
                                value = removed_field.bool_default;
                            }
                            if (token_field_value.type == TOKEN_TRUE) value = true;
                            else value = false;
                            *removed_field.bool_ptr = value;
                        } break;

                        case FIELD_UINT: {
                            size_t value = token_field_value.number_value;
                            if (!expect_token_to_be_of_type(token_field_value, TOKEN_NUMBER, &config_log)
                                || value <= 0) {
                                s_push_cstr(&config_log, "- NOTE: value must be > 0\n");
                                s_push_fstr(&config_log, "- NOTE: defaulted to `%zu`\n\n", removed_field.uint_default);
                                value = removed_field.uint_default;
                            }
                            *removed_field.uint_ptr = value;
                        } break;

                        case FIELD_STRING: {
                            char *value;
                            if (!expect_token_to_be_of_type(token_field_value, TOKEN_STRING, &config_log) || strlen(value) == 0) {
                                s_push_cstr(&config_log, "- NOTE: value must be a non empty string\n");
                                s_push_fstr(&config_log, "- NOTE: defaulted to \"%s\"\n\n",
                                        removed_field.string_default);
                                value = strdup(removed_field.string_default);
                            } else value = strdup(token_field_value.string_value);
                            *removed_field.string_ptr = value;
                        } break;

                        case FIELD_LIMITED_STRING: {
                            char *value = token_field_value.string_value;
                            TokenType LIMITED_STRING_TOKENS[2] = {TOKEN_IDENT, TOKEN_STRING};
                            bool is_wrong_token_type = !expect_token_to_be_of_types(token_field_value,
                                    LIMITED_STRING_TOKENS, 2, NO_LOG);
                            bool is_str_empty = strlen(value) == 0;
                            LimitedStringIndex index = -1;
                            if (!is_wrong_token_type && !is_str_empty) {
                                for (size_t i = 0; i < removed_field.limited_string_valid_values.count; i++) {
                                    if (streq(removed_field.limited_string_valid_values.items[i], value)) {
                                        index = i;
                                        break;
                                    }
                                }
                            }
                            bool is_not_valid = index == -1;
                            if (is_wrong_token_type || is_str_empty || is_not_valid) {
                                if (is_wrong_token_type) {
                                    expect_token_to_be_of_types(token_field_value, LIMITED_STRING_TOKENS, 2, &config_log);
                                } else if (is_str_empty) {
                                    s_push_fstr(&config_log, LOC_FMT"\n- ERROR: empty string\n");
                                } else if (is_not_valid) {
                                    s_push_fstr(&config_log, LOC_FMT"\n- ERROR: string not valid\n");
                                }
                                s_push_fstr(&config_log,
                                        "- NOTE: value must be a non empty string among one of the following values: ");
                                size_t count = removed_field.limited_string_valid_values.count;
                                for (size_t i = 0; i < count; i++) {
                                    s_push_fstr(&config_log, "`%s`", removed_field.limited_string_valid_values.items[i]);
                                    if (count > 2 && i < count-2) s_push_cstr(&config_log, ", ");
                                    else if (i == count-2) s_push_cstr(&config_log, " or ");
                                    else s_push_fstr(&config_log, "\n");
                                }

                                s_push_fstr(&config_log, "- NOTE: defaulted to `%s`\n\n", removed_field.limited_string_valid_values.items[removed_field.limited_string_default]);
                                *removed_field.limited_string_ptr = removed_field.limited_string_default;
                            } else {
                                *removed_field.limited_string_ptr = index;
                            }
                        } break;

                        default: print_error_and_exit(LOC_FMT" Unreachable field type %u in load_config",
                                    LOC_ARG(token_field_value.loc), removed_field.type);
                    }
                } else j++;
            }

            if (!found) {
                size_t k = 0;
                while (!found && k < inserted_fields.count) {
                    if (streq(field_name, inserted_fields.items[k].name)) {
                        s_push_fstr(&config_log, LOC_FMT"\n- ERROR: redeclaration of field `%s`\n\n",
                                LOC_ARG(token_field_name.loc), field_name);
                        found = true;
                    } else k++;
                }
            }

            if (!found) {
                s_push_fstr(&config_log, LOC_FMT"\n- ERROR: unknown field `%s`\n\n",
                        LOC_ARG(token_field_name.loc), field_name);
            }
        } break;

        case TOKEN_VAR: {
            i++;
            Token token_var_name = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_var_name, TOKEN_IDENT, &config_log)) continue;
            char *var_name = token_var_name.string_value;
            bool found = false;
            for (size_t j = 0; j < editor.config.vars.count; j++) {
                if (streq(var_name, editor.config.vars.items[j].name)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                s_push_fstr(&config_log, LOC_FMT"\n- ERROR: redeclaration of variable `%s`\n",
                        LOC_ARG(token_var_name.loc), var_name);
                continue;
            }
            i++;
            Token token_equals = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_equals, '=', &config_log)) continue;
            i++;
            Token token_var_value = tokens.items[i];
            TokenType VAR_VALUE_TOKENS[5] = {TOKEN_IDENT, TOKEN_NUMBER, TOKEN_STRING, TOKEN_TRUE, TOKEN_FALSE}; 
            if (!expect_token_to_be_of_types_extra_newline(token_var_value, VAR_VALUE_TOKENS, 5, &config_log)) continue;
            Var var = {0};
            var.name = strdup(var_name);
            switch (token_var_value.type)
            {
            case TOKEN_IDENT:
                // TODO: check if there is a variable with this name else treat it like a string? not so sure
                s_push_fstr(&config_log, LOC_FMT"\n- TODO: assign identifier to variable '%s'",
                        LOC_ARG(token_var_value.loc), var_name);
                break;

            case TOKEN_NUMBER:
                if (token_var_value.number_value > 0) {
                    var.type = PISQUY_UINT;
                    var.uint_value = token_var_value.number_value;
                } else {
                    var.type = PISQUY_INT;
                    var.int_value = token_var_value.number_value;
                }
                break;

            case TOKEN_STRING:
                var.type = PISQUY_STRING;
                var.string_value = strdup(token_var_value.string_value);
                break;

            case TOKEN_TRUE:
                var.type = PISQUY_BOOL;
                var.bool_value = true;
                break;

            case TOKEN_FALSE:
                var.type = PISQUY_BOOL;
                var.bool_value = false;
                break;

            default: print_error_and_exit("Unreachable token type parsing variable value in load_config");
            }
            da_push(&editor.config.vars, var);
        } break;

        case TOKEN_DEF: {
            i++;
            Token token_command_name = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_command_name, TOKEN_IDENT, &config_log)) continue;
            char *command_name = token_command_name.string_value;
            bool already_defined = false;
            da_enumerate(commands, Command, i, command) {
                if (streq(command_name, command->name)) {
                    s_push_fstr(&config_log, LOC_FMT"\n- ERROR: redeclaration of %s command `%s`\n\n",
                            LOC_ARG(token_command_name.loc), i < BUILTIN_CMDS_COUNT ? "builtin" : "user defined",
                            command_name); 
                    already_defined = true;
                    break;
                }
            }
            if (already_defined) continue;
            i++;
            if (!expect_token_to_be_of_type_extra_newline(tokens.items[i], '=', &config_log)) continue;
            i++;
            Command user_defined_cmd = parse_commands_list(tokens, &i, &config_log, WITH_LOCATION);

            // TODO: this is a work in progress, don't delete these comments
            //
            //Command user_defined_cmd = {0};
            //i++;
            //Token tok_it = tokens.items[i];
            //while (tok_it.type != TOKEN_NEWLINE) {
            //    size_t n = 1; (void)n; // TODO
            //    if (tok_it.type == TOKEN_NUMBER) {
            //        if (tok_it.number_value <= 0) {
            //            s_push_fstr(&config_log, LOC_FMT"\n- ERROR: multiplicity number should be > 0 (got %d)\n\n",
            //                    LOC_ARG(tok_it.loc), tok_it.number_value);
            //            while (tok_it.type != TOKEN_NEWLINE) {
            //                i++;
            //                tok_it = tokens.items[i];
            //            }
            //            break;
            //        }
            //        n = tok_it.number_value;
            //        i++;
            //        tok_it = tokens.items[i];
            //    }
            //    if (!expect_token_to_be_of_type_extra_newline(tok_it, TOKEN_IDENT, &config_log)) {
            //        while (tok_it.type != TOKEN_NEWLINE) {
            //            i++;
            //            tok_it = tokens.items[i];
            //        }
            //        break;
            //    }
            //    char *subcmd_name = tok_it.string_value;
            //    CommandType subcmd_type = get_command_type_from_string(subcmd_name);
            //    if (subcmd_type == UNKNOWN) {
            //        s_push_fstr(&config_log, LOC_FMT"\n- ERROR: unknown command `%s`\n\n",
            //                LOC_ARG(tok_it.loc), subcmd_name);
            //        while (tok_it.type != TOKEN_NEWLINE) {
            //            i++;
            //            tok_it = tokens.items[i];
            //        }
            //        break;
            //    }
            //    Command subcmd = {
            //        .type = subcmd_type,
            //        .n = n
            //    };
            //    i++;
            //    tok_it = tokens.items[i];
            //    if (tok_it.type == '(') {
            //        log_this("TODO: parse arguments list for command `%s`", subcmd_name);
            //        i++;
            //        tok_it = tokens.items[i];
            //        while (tok_it.type != ')') {
            //            i++;
            //            tok_it = tokens.items[i];
            //        } 
            //        i++;
            //        tok_it = tokens.items[i];
            //    }
            //    subcmd.name = strdup(subcmd_name);
            //    da_push(&user_defined_cmd.subcmds, subcmd);
            //}

            user_defined_cmd.name = strdup(command_name);
            add_user_defined_command(user_defined_cmd);
        } break;

        case TOKEN_SNIPPET: {
            i++;
            Token token_snippet_handle = tokens.items[i];
            TokenType SNIPPET_HANDLE_TOKENS[2] = {TOKEN_IDENT, TOKEN_STRING};
            if (!expect_token_to_be_of_types_extra_newline(token_snippet_handle, SNIPPET_HANDLE_TOKENS, 2, &config_log)) continue;
            i++;
            if (!expect_token_to_be_of_type_extra_newline(tokens.items[i], '=', &config_log)) continue;
            i++;
            Token token_snippet_body = tokens.items[i];
            TokenType SNIPPET_BODY_TOKENS[2] = {TOKEN_IDENT, TOKEN_STRING};
            if (!expect_token_to_be_of_types_extra_newline(token_snippet_body, SNIPPET_BODY_TOKENS, 2, &config_log)) continue;
            Snippet snippet = {0};
            if (!parse_snippet_body(token_snippet_body, &snippet, &config_log)) continue;
            snippet.handle = strdup(token_snippet_handle.string_value);
            snippet.handle_len = strlen(token_snippet_handle.string_value);
            snippet.handle_is_prefix = strneq(snippet.handle, snippet.body, snippet.handle_len);
            //if (DEBUG) {
            //    if (snippet.is_inline)
            //        s_push_fstr(&config_log, "\nParsed snippet `%s` = \"%s\"\n", snippet.handle, snippet.body);
            //    else s_push_fstr(&config_log, "\nParsed snippet `%s` = \"{\n%s\n}\"\n", snippet.handle, snippet.body);
            //}
            da_push(&editor.snippets, snippet);
        } break;

        case TOKEN_NEWLINE: break;
            
        case TOKEN_EOF: 
            assert(i == tokens.count-1);
            break;

        default:
            char *tokstrval = token_type_and_value_as_string(first_token);
            s_push_fstr(&config_log, LOC_FMT"\n- ERROR: unexpected %s\n\n", LOC_ARG(first_token.loc), tokstrval);
            free(tokstrval);
        }
    }

    free_tokens(&tokens);

    if (remaining_fields.count > 0) {
        s_push_cstr(&config_log, "WARNING: the following fields have not been set:\n");
        da_foreach(remaining_fields, ConfigField, field) {
            s_push_fstr(&config_log, " -> %s (type: %s, default: ", field->name, fieldtype_to_string(field->type));
            switch (field->type)
            {
                case FIELD_BOOL:
                    s_push_fstr(&config_log, "`%s`)\n", BOOL_AS_CSTR(field->bool_default));
                    *field->bool_ptr = field->bool_default;
                    break;
                case FIELD_UINT:
                    s_push_fstr(&config_log, "`%zu`)\n", field->uint_default);
                    *field->uint_ptr = field->uint_default;
                    break;
                case FIELD_STRING:
                    s_push_fstr(&config_log, "`%s`)\n", field->string_default);
                    *field->string_ptr = strdup(field->string_default);
                    break;
                case FIELD_LIMITED_STRING:
                    s_push_fstr(&config_log, "`%s`)\n",
                            field->limited_string_valid_values.items[field->limited_string_default]);
                    *field->limited_string_ptr = field->limited_string_default;
                    // TODO: magari elencare anche qua i possibili valori
                    break;
                default:
                    print_error_and_exit("Unreachable field type in load_config\n");
            }
        }
        s_push(&config_log, '\n');
    }

    //if (DEBUG) {
    //    s_push_fstr(&config_log, "\nDefined commands:\n");
    //    da_enumerate (commands, Command, i, command) {
    //        s_push_fstr(&config_log, "%s: `%s`\n", i < BUILTIN_CMDS_COUNT ? "Builtin" : "User", command->name);
    //    }
    //}

    if (s_is_empty(config_log)) return;

fail:

    s_push_null(&config_log);

    bool config_has_errors = strstr(config_log.items, "ERROR") != NULL;
    bool config_has_warnings = strstr(config_log.items, "WARNING") != NULL;

    bool suppress_logs = !config_has_errors   && (editor.config.configlog_level >= CONFIGLOG_ERROR
                     || (!config_has_warnings &&  editor.config.configlog_level >= CONFIGLOG_WARNING));

    if (!suppress_logs) {
        char tmp_filename[] = "/tmp/editor_config_log_XXXXXX";
        int fd = mkstemp(tmp_filename);
        if (fd == -1) print_error_and_exit("Could not create temporary file\n");
        FILE *f = fdopen(fd, "w");
        if (!f) {
            close(fd);
            print_error_and_exit("Could not open temporary file\n"); 
        }

        fputs(config_log.items, f);
        fclose(f);

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "cat %s | less", tmp_filename);

        pid_t child = fork();
        switch (child)
        {
            case -1: print_error_and_exit("Could not create child process to execute cmd\n");
            case 0:
                     execl("/bin/sh", "sh", "-c", cmd, NULL);
                     exit(0);
            default: waitpid(child, NULL, 0);
        }
    }

    s_free(&config_log);
    if (config_has_errors) exit(1);
}

/// END Config 

/* Pairs */
typedef enum
{
    DEFAULT_EDITOR_PAIR = 1,
    DEFAULT_EDITOR_PAIR_INV,
    DEFAULT_STATUS_PAIR,
    DEFAULT_MESSAGE_PAIR,
    DEFAULT_COMMAND_PAIR
} EditorPair;

/* Windows */
static inline void get_screen_size(void) { getmaxyx(stdscr, editor.screen_rows, editor.screen_cols); }

Window create_window(int h, int w, int y, int x, int color_pair)
{
    Window win = {0};
    win.win = newwin(h, w, y, x);
    win.height = h;
    win.width = w;
    win.start_y = y;
    win.start_x = x;
    if (has_colors() && can_change_color()) wbkgd(win.win, COLOR_PAIR(color_pair));
    return win;
}

void create_windows(void)
{
    win_main = create_window(editor.screen_rows-1, editor.screen_cols-LINE_NUMBERS_SPACE, 0, LINE_NUMBERS_SPACE,
            DEFAULT_EDITOR_PAIR);
    win_line_numbers = create_window(editor.screen_rows-1, LINE_NUMBERS_SPACE, 0, 0, DEFAULT_EDITOR_PAIR);
    win_message      = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_MESSAGE_PAIR);
    win_command      = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_COMMAND_PAIR);
    win_status       = create_window(1, editor.screen_cols, editor.screen_rows-1, 0, DEFAULT_STATUS_PAIR);
}

void destroy_windows(void)
{
    delwin(win_main.win);
    delwin(win_line_numbers.win);
    delwin(win_message.win);
    delwin(win_command.win);
    delwin(win_status.win);
}

void cleanup_on_terminating_signal(int sig)
{
    log_this("Program received signal %d", sig);
    ncurses_end();
    exit(1);
}

#define COLOR_VALUE_TO_NCURSES(value) ((value*1000)/255)
#define RGB_TO_NCURSES(r, g, b) COLOR_VALUE_TO_NCURSES(r), COLOR_VALUE_TO_NCURSES(g), COLOR_VALUE_TO_NCURSES(b)

void ncurses_init(void)
{
    initscr();

    raw();
    noecho();
    nonl();
    nodelay(stdscr, TRUE);
    set_escdelay(25);
    keypad(stdscr, TRUE);

    signal(SIGINT, cleanup_on_terminating_signal);
    signal(SIGTERM, cleanup_on_terminating_signal);
    signal(SIGSEGV, cleanup_on_terminating_signal);
}

void initialize_colors(void)
{
    if (has_colors()) {
        start_color();
        if (can_change_color()) {
            init_color(ED_COLOR_DEFAULT_BACKGROUND, RGB_TO_NCURSES(18, 18, 18));
            init_color(ED_COLOR_DEFAULT_FOREGROUND, RGB_TO_NCURSES(150, 200, 150));
            init_color(ED_COLOR_YELLOW, RGB_TO_NCURSES(178, 181, 0));
            init_color(ED_COLOR_RED, RGB_TO_NCURSES(150, 20, 20));
            init_color(ED_COLOR_BLUE, RGB_TO_NCURSES(20, 20, 150));

            init_pair(DEFAULT_EDITOR_PAIR,     ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_DEFAULT_BACKGROUND);
            init_pair(DEFAULT_EDITOR_PAIR_INV, ED_COLOR_DEFAULT_BACKGROUND, ED_COLOR_DEFAULT_FOREGROUND);
            init_pair(DEFAULT_MESSAGE_PAIR,    ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_RED);
            init_pair(DEFAULT_COMMAND_PAIR,    ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_BLUE);
            init_pair(DEFAULT_STATUS_PAIR,     ED_COLOR_DEFAULT_BACKGROUND, ED_COLOR_DEFAULT_FOREGROUND);
        } else {
            use_default_colors();
        }
    }
}

int read_key()
{
    int c = getch();
    if (c != ESC) return c;

    int first = getch();
    if (first == ERR) return ESC;

    if (first == '[') { // ESC-[-X sequence
        int second = getch();
        if (second == ERR) return ESC;
        log_this("Read ESC-[-%c sequence", first);

        return ESC; // TODO: togli

        switch (second) { default: return ESC; }
    }

    switch (first) { // ALT-X sequence
        case '0'          : return ALT_0;
        case '1'          : return ALT_1;
        case '2'          : return ALT_2;
        case '3'          : return ALT_3;
        case '4'          : return ALT_4;
        case '5'          : return ALT_5;
        case '6'          : return ALT_6;
        case '7'          : return ALT_7;
        case '8'          : return ALT_8;
        case '9'          : return ALT_9;

        case 'c'          : return ALT_c;
        case 'C'          : return ALT_C;
        case 'k'          : return ALT_k;
        case 'K'          : return ALT_K;
        case 'j'          : return ALT_j;
        case 'J'          : return ALT_J;
        case 'h'          : return ALT_h;
        case 'H'          : return ALT_H;
        case 'l'          : return ALT_l;
        case 'L'          : return ALT_L;
        case 'm'          : return ALT_m;
        case 'n'          : return ALT_n;
        case 'p'          : return ALT_p;
        case KEY_BACKSPACE: return ALT_BACKSPACE;
        case ':'          : return ALT_COLON;

        case CTRL('C'): return CTRL_ALT_C;
        case CTRL('K'): return CTRL_ALT_K;
        case CTRL('J'): return CTRL_ALT_J;
        case CTRL('H'): return CTRL_ALT_H;
        case CTRL('L'): return CTRL_ALT_L;

        default: return ESC;
    }
}

#define HIDE_MAIN true
// TODO: maybe even change cursor color
void show_ghost_cursor(Cursor cursor, bool hide_main)
{
    if (hide_main && editor.cursor.x == cursor.x && editor.cursor.y == cursor.y) return;
    size_t screen_y = cursor.y - editor.offset;
    if (screen_y >= win_main.height) return;

    wmove(win_main.win, screen_y, cursor.x);
    wattrset(win_main.win, A_REVERSE); {
        char char_at_cursor = mvwinch(win_main.win, screen_y, cursor.x) & A_CHARTEXT;
        if (char_at_cursor == 0) char_at_cursor = ' ';
        waddch(win_main.win, char_at_cursor);
    } wattrset(win_main.win, COLOR_PAIR(DEFAULT_EDITOR_PAIR)); // TODO: rember to change it when the colors
                                                               //       can be chosen by the user
}

void update_window_main(void)
{
    for (size_t i = editor.offset; i < editor.offset+win_main.height; i++) {
        if (i >= editor.rows.count) {
            wprintw(win_main.win, "~\n");
            continue;
        }
        wprintw(win_main.win, S_FMT"\n", S_ARG(ROW(i)->content));
    }
    if (editor.in_cmd) {
        show_ghost_cursor(editor.cursor, !HIDE_MAIN);
        if (editor.multicursor.is_enabled)
            da_foreach (editor.sorted_multicursor, Cursor *, cursor)
                show_ghost_cursor(**cursor, HIDE_MAIN);
    } else if (!da_is_empty(&editor.multicursor)) {
        if (editor.multicursor.is_enabled) {
            da_foreach (editor.sorted_multicursor, Cursor *, cursor)
                show_ghost_cursor(**cursor, HIDE_MAIN);
        } else {
        da_foreach (editor.multicursor, Cursor, cursor)
            show_ghost_cursor(*cursor, HIDE_MAIN);
        }
    }
}

void update_window_line_numbers(void)
{
    for (size_t i = 0; i < win_line_numbers.height; i++) {
        wprintw(win_line_numbers.win, "%zu\n", i+1);
    }
}

void update_window_message(void)
{
    waddstr(win_message.win, cs_get_current(editor.messages));
}

void update_window_command(void)
{
    waddstr(win_command.win, "Command: ");
    wprintw(win_command.win, S_FMT, S_ARG(editor.cmd));
}

void update_window_status(void)
{
    char perc_buf[5] = {0};
    if (editor.rows.count == 0) strcpy(perc_buf, "0%");
    else if (CURRENT_Y_POS == 0) strcpy(perc_buf, "Top");
    else if (CURRENT_Y_POS == editor.rows.count-1) strcpy(perc_buf, "Bot");
    else if (CURRENT_Y_POS > editor.rows.count-1)  strcpy(perc_buf, "Over");
    else sprintf(perc_buf, "%d%%", (int)(((float)(CURRENT_Y_POS)/(editor.rows.count))*100));

    // WARN: it could overflow
    waddstr(win_status.win, editor.filename);
    waddch(win_status.win, ' ');
    wprintw(win_status.win, "%c", editor.dirty ? '+' : '-');
    waddch(win_status.win, ' ');
    wprintw(win_status.win, "(%zu, %zu)", CURRENT_X_POS+1, CURRENT_Y_POS+1);
    wprintw(win_status.win, " | ");
    wprintw(win_status.win, "%zu lines", editor.rows.count); // TODO: singular/plural
    waddch(win_status.win, ' ');
    wprintw(win_status.win, "(%s)", perc_buf);
    wprintw(win_status.win, " | ");
    wprintw(win_status.win, "page %zu/%zu", editor.page+1, N_PAGES);
    wprintw(win_status.win, " | ");
    wprintw(win_status.win, "page %zu/%zu", editor.page+1, N_PAGES);
    if (editor_is_expanding_snippet()) {
        wprintw(win_status.win, " | ");
        wprintw(win_status.win, "expanding snippet `%s`", editor.expanding_snippet.snippet->handle);
    }
    if (editor.N != N_DEFAULT) {
        wprintw(win_status.win, " | ");
        wprintw(win_status.win, "%d", editor.N);
    }
}

#define update_window(window_name)           \
    do {                                     \
        werase(win_##window_name.win);       \
        update_window_##window_name();       \
        wnoutrefresh(win_##window_name.win); \
    } while (0)

void update_windows(void)
{
    update_window(main);
    update_window(line_numbers);
    if (editor.in_cmd) update_window(command);
    else if (editor.is_showing_message) update_window(message);
    update_window(status);

    //Row *row;
    //for (size_t y = editor.offset; y < editor.offset+win_main.height; y++) {
    //    // TODO: si puo' fattorizzare la funzione che aggiunge gli spazi per keep_cursor
    //    bool is_current_line = y == editor.cursor.y-editor.offset;

    //    if (y >= editor.rows.count) {
    //        if (is_current_line && editor.in_cmd && editor.cursor.x == 0) s_push_cstr(&screen_buf, ANSI_INVERSE);
    //        s_push(&screen_buf, '~'); // TODO: poi lo voglio togliere, forse, ma ora lo uso per debuggare
    //        if (is_current_line && editor.in_cmd && editor.cursor.x == 0) s_push_cstr(&screen_buf, ANSI_RESET);
    //        if (is_current_line && editor.in_cmd && editor.cursor.x > 0) {
    //            for (size_t x = 1; x < editor.cursor.x; x++) {
    //                s_push(&screen_buf, ' ');
    //            }
    //            s_push_cstr(&screen_buf, ANSI_INVERSE);
    //            s_push(&screen_buf, ' ');
    //            s_push_cstr(&screen_buf, ANSI_RESET);
    //        }
    //        s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    //        continue;
    //    }

    //    row = ROW(y);

    //    if (config.line_numbers == LN_REL) {
    //        if (is_current_line) {
    //            size_t line_mag = y == 0 ? 1 : log10(y)+1;
    //            size_t spaces_to_add = LINE_NUMBERS_SPACE-line_mag-1;
    //            if ((size_t)(log10(y+1)+1) > line_mag) spaces_to_add--;
    //            for (size_t x = 0; x < spaces_to_add; x++) 
    //                s_push(&screen_buf, ' '); 
    //            //s_push_cstr(&screen_buf, ANSI_FG_COLOR(ED_COLOR_YELLOW)); // TODO: color
    //            s_push_fstr(&screen_buf, "%zu", y+1); 
    //            s_push_cstr(&screen_buf, ANSI_RESET);
    //            s_push(&screen_buf, ' '); 
    //        } else {
    //            size_t rel_num = y > CURRENT_Y_POS ? y-CURRENT_Y_POS : CURRENT_Y_POS-y;
    //            size_t line_mag = rel_num == 0 ? 1 : log10(rel_num)+1;
    //            for (size_t x = 0; x < LINE_NUMBERS_SPACE-line_mag-1; x++) 
    //                s_push(&screen_buf, ' '); 
    //            s_push_fstr(&screen_buf, "%zu ", rel_num);
    //        }
    //    } else if (config.line_numbers == LN_ABS) {
    //        size_t line_mag = y == 0 ? 1 : log10(y)+1;
    //        for (size_t x = 0; x < LINE_NUMBERS_SPACE-line_mag-1; x++)
    //            s_push(&screen_buf, ' '); 
    //        //if (is_current_line) s_push_cstr(&screen_buf, ANSI_FG_COLOR(ED_COLOR_YELLOW)); // TODO: color
    //        s_push_fstr(&screen_buf, "%zu", y+1); 
    //        if (is_current_line) s_push_cstr(&screen_buf, ANSI_RESET);
    //        s_push(&screen_buf, ' '); 
    //    }

    //    size_t len = row->content.count;
    //    if (len > editor.screen_cols-LINE_NUMBERS_SPACE) len = editor.screen_cols-LINE_NUMBERS_SPACE; // TODO: viene troncata la riga
    //    if (len == 0) {
    //        if (is_current_line && editor.in_cmd) {
    //            for (size_t x = 0; x < editor.cursor.x; x++) {
    //                s_push(&screen_buf, ' ');
    //            }
    //            s_push_cstr(&screen_buf, ANSI_INVERSE);
    //            s_push(&screen_buf, ' ');
    //            s_push_cstr(&screen_buf, ANSI_RESET);
    //        }
    //        s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    //        continue; 
    //    }

    //    int c;
    //    for (size_t x = 0; x < len; x++) {
    //        c = row->content.items[x];
    //        bool keep_cursor = is_current_line && editor.in_cmd && x == editor.cursor.x-LINE_NUMBERS_SPACE;
    //        if (keep_cursor) s_push_cstr(&screen_buf, ANSI_INVERSE);
    //        if (!isprint(c)) {
    //            s_push_cstr(&screen_buf, ANSI_INVERSE);
    //            if (c <= 26) {
    //                s_push(&screen_buf, '^');
    //                s_push(&screen_buf, '@'+c);
    //            } else s_push(&screen_buf, '?');
    //            s_push_cstr(&screen_buf, ANSI_RESET);
    //        } else s_push(&screen_buf, c);
    //        if (keep_cursor) s_push_cstr(&screen_buf, ANSI_RESET);
    //    }
    //    if (is_current_line && editor.in_cmd && len <= editor.cursor.x) {
    //        for (size_t x = len; x < editor.cursor.x; x++) {
    //            s_push(&screen_buf, ' ');
    //        }
    //        s_push_cstr(&screen_buf, ANSI_INVERSE);
    //        s_push(&screen_buf, ' ');
    //        s_push_cstr(&screen_buf, ANSI_RESET);
    //    }
    //    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    //}

    ///* Create a two rows status. First row: */
    //s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    //s_push_cstr(&screen_buf, ANSI_INVERSE); // displayed with inversed colours

    //char perc_buf[4];
    //if (CURRENT_Y_POS == 0) strcpy(perc_buf, "Top");
    //else if (CURRENT_Y_POS >= editor.rows.count-1) strcpy(perc_buf, "Bot");
    //else if (editor.rows.count == 0) strcpy(perc_buf, "0%");
    //else sprintf(perc_buf, "%d%%", (int)(((float)(CURRENT_Y_POS)/(editor.rows.count))*100));

    //char status[80];
    //size_t len = snprintf(status, sizeof(status), " %s %c (%zu, %zu) | %zu lines (%s) | page %zu/%zu", 
    //        editor.filename,
    //        editor.dirty ? '+' : '-',
    //        CURRENT_X_POS+1,
    //        CURRENT_Y_POS+1, 
    //        editor.rows.count, 
    //        perc_buf,
    //        editor.page+1,
    //        N_PAGES
    //);
    //if (len > editor.screen_cols) len = editor.screen_cols;
    //s_push_str(&screen_buf, status, len);

    //char rstatus[80];
    //size_t rlen = editor.N == N_DEFAULT ? 0 : snprintf(rstatus, sizeof(rstatus), "%d", editor.N);
    //while (len < editor.screen_cols && editor.screen_cols - (len+1) != rlen) {
    //    s_push(&screen_buf, ' ');
    //    len++;
    //}
    //if (editor.N != N_DEFAULT) s_push_str(&screen_buf, rstatus, rlen);
    //s_push(&screen_buf, ' ');
    //s_push_cstr(&screen_buf, ANSI_RESET);

    //if (!editor.in_cmd && !da_is_empty(&editor.messages)) {
    //    char *msg;
    //    if (time(NULL)-editor.current_msg_time > config.msg_lifetime)
    //        msg = next_message();
    //    else
    //        msg = editor.messages.items[0];
    //    // TODO: now it does it every 'frame', but if the message did not change it shouln't clear/redraw the window
    //    if (msg) {
    //        wprintw(win_message.win, msg);
    //        wrefresh(win_message.win);
    //    }
    //}

    ///* Put cursor at its current position. Note that the horizontal position
    // * at which the cursor is displayed may be different compared to 'editor.cursor.x'
    // * because of TABs. */
    //size_t cx = editor.cursor.x+1;
    //size_t cy = editor.cursor.y+1;
    //if (editor.in_cmd) {
    //    cx = strlen("Command: ")+editor.cmd_pos+1;
    //    cy = editor.screen_cols-1;
    //} else {
    //    size_t y = CURRENT_Y_POS;
    //    if (y < editor.rows.count) {
    //        Row *row = ROW(y);
    //        size_t rowlen = strlen(row->content.items);
    //        for (size_t j = 0; j < (CURRENT_X_POS); j++) {
    //            if (j < rowlen && row->content.items[j] == TAB) cx += 4;
    //        }
    //    }
    //}
    //char buf[32];
    //snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", cy, cx);
    //s_push_str(&screen_buf, buf, strlen(buf));

    //wrefresh(win_main.win);
}

void update_cursor(void)
{
    size_t cy = editor.cursor.y;
    size_t cx = editor.cursor.x;
    WINDOW *win = win_main.win;

    if (editor.in_cmd) {
        cy = win_command.start_y;
        cx = strlen("Command: ") + editor.cmd_pos;
        win = win_command.win;
    }

    wmove(win, cy, cx);
    wnoutrefresh(win);
}

void handle_sigwinch(int signo)
{
    (void)signo;
    get_screen_size();
    destroy_windows();
    create_windows();
    
    if (editor.cursor.y >= win_main.height) editor.cursor.y = win_main.height - 1;
    if (editor.cursor.x >= win_main.width) editor.cursor.x = win_main.width - 1;
}

void editor_init()
{
    get_screen_size();
    editor.current_quit_times = editor.config.quit_times;
    editor.N = N_DEFAULT;

    signal(SIGWINCH, handle_sigwinch);
}

bool open_file(char *filepath)
{
    if (editor.filepath) free(editor.filepath);
    if (editor.filename) free(editor.filename);
    da_clear(&editor.rows);

    if (filepath == NULL) {
        editor.filepath = NULL;
        editor.filename = strdup("new");
        return true;
    }

    editor.filepath = strdup(filepath);

    FILE *file = fopen(filepath, "r");
    if (file == NULL) return false;

    char *last_slash = strrchr(filepath, '/');
    editor.filename = strdup(last_slash ? last_slash+1 : filepath);

    ssize_t res; 
    size_t len;
    char *line = NULL;
    errno = 0;
    while ((res = getline(&line, &len, file)) != -1) {
        Row row = {0};
        s_push_str(&row.content, line, res-1);
        da_push(&editor.rows, row);
    }
    free(line);
    if (errno) return false;

    // TODO: set cursor (0,0)
    editor.cursor.x = 0;

    return true;
}

void scroll_up() // TODO: cy va oltre
{ 
    if (editor.offset > 0) {
        editor.offset--;
        move_cursor_down();
    } else editor.offset = 0;
}

void scroll_down() // TODO: cy fa cose strane
{
    if (editor.offset < editor.rows.count - 1) {
        editor.offset++;
        move_cursor_up();
    } else editor.offset = editor.rows.count-1;
}

void move_page_up() // TODO: non funziona benissimo
{
    if (editor.page > 0) editor.page--;
    else editor.page = 0;
    editor.offset = editor.page*win_main.height + editor.cursor.y;
}             

void move_page_down() // TODO: non funziona benissimo
{
    if (editor.page < N_PAGES-2) editor.page++;
    else editor.page = N_PAGES-1;
    editor.offset = editor.page*win_main.height + editor.cursor.y;
}           

static inline void move_cursor_begin_of_screen() { editor.cursor.y = 0; }

static inline void move_cursor_end_of_screen() { editor.cursor.y = win_main.height-1; }

void move_cursor_begin_of_file()
{
    editor.page = 0;
    editor.offset = 0;
    editor.cursor.y = 0;
}     

void move_cursor_end_of_file() 
{
    editor.page = N_PAGES-1;
    editor.offset = editor.rows.count-win_main.height;
    editor.cursor.y = win_main.height-1;
}

static inline void move_cursor_begin_of_line() { editor.cursor.x = 0; }
static inline void move_cursor_end_of_line() { editor.cursor.x = win_main.width-1; }

void move_cursor_first_non_space()
{
    size_t count = CURRENT_ROW->content.count;
    if (count == 0) return;
    editor.cursor.x = 0;
    while (editor.cursor.x < count && isspace(CURRENT_CHAR))
        editor.cursor.x++;
}

void move_cursor_last_non_space()
{
    size_t count = CURRENT_ROW->content.count;
    if (count == 0) {
        editor.cursor.x = 0;
        return;
    }
    editor.cursor.x = count - 1;
    while (editor.cursor.x > 0 && isspace(CURRENT_CHAR))
        editor.cursor.x--;
    if (editor.cursor.x != win_main.width-1) editor.cursor.x++;
}

void itoa(int n, char *buf)
{
    if (n == 0) {
        buf[0] = '0';
        return;
    }
    char tmp[64] = {0};
    int i = 0;
    while (n > 0) {
        tmp[i] = n % 10 + '0';
        i++;
        n /= 10;
    }
    int len = strlen(tmp);
    for (int i = 0; i < len; i++) buf[i] = tmp[len-i-1];
}

void execute_command(Command *cmd, CommandArgs *runtime_args)
{
    log_this("Executing command `%s`", cmd->name);
    da_foreach (cmd->subcmds, Command, subcmd) {
        log_this(" - %s", subcmd->name);
    }

    if (is_command_type_builtin(cmd->type)) {
        CommandArgs final_args = {0}; // TODO: maybe it can be just an array
        for (size_t i = 0; i < cmd->baked_args.count; i++) {
            log_this("baked argument %zu", i);
            CommandArg arg = cmd->baked_args.items[i];
            if (arg.type == PISQUY_ARG_PLACEHOLDER) {
                if (runtime_args && arg.placeholder_index < runtime_args->count) {
                    da_push(&final_args, runtime_args->items[arg.placeholder_index]);
                } else {
                    write_message("ERROR: Missing argument $%zu for %s", arg.index, cmd->name);
                    log_this("ERROR: Missing argument $%zu for %s", arg.index, cmd->name);
                    return; 
                }
            } else da_push(&final_args, arg);
        }
        if (cmd->baked_args.count == 0 && runtime_args)
            da_push_many(&final_args, cmd->baked_args.items, cmd->baked_args.count);
        assert(cmd->execute);
        for (size_t i = 0; i < cmd->n; i++)
            cmd->execute(cmd, &final_args);
        if (final_args.count > 0) free(final_args.items);
    } else if (cmd->type == COMMAND_FROM_LINE || is_command_type_user_defined(cmd->type)) {
        da_foreach(cmd->subcmds, Command, subcmd)
            execute_command(subcmd, runtime_args);
    } else if (cmd->type == UNKNOWN) {
        write_message("Unknown command `%s`", cmd->name);
    } else {
        print_error_and_exit("Unreachable command `%s` (%u) in execute_command\n",
                cmd->name ? cmd->name : "", cmd->type);
    }
}

void insert_newline_and_keep_pos()
{
    write_message("TODO: insert_newline_and_keep_pos");
}

void delete_char_at(Row *row, size_t at)
{
    if (row->content.count <= at) return;
    s_remove(&row->content, at);
}

void delete_char_internal()
{
    if (editor.in_cmd) {
        if (editor.cmd.count <= 0) return;
        if (editor.cmd_pos == 0) return;
        da_remove(&editor.cmd, editor.cmd_pos-1);
        editor.cmd_pos--;
        return;
    }

    size_t y = CURRENT_Y_POS;
    size_t x = CURRENT_X_POS;
    Row *row = (y >= editor.rows.count) ? NULL : CURRENT_ROW;
    if (!row || (x == 0 && y == 0)) return;
    if (x == 0) {
        /* Handle the case of column 0, we need to move the current line
         * on the right of the previous one. */
        Row *prev = ROW(y-1);
        x = prev->content.count;
        s_push_str(&prev->content, row->content.items, row->content.count);
        da_remove(&editor.rows, y);
        if (editor.cursor.y == 0) editor.offset--;
        else editor.cursor.y--;
        editor.cursor.x = x;
        if (editor.cursor.x >= win_main.width) {
            int shift = (win_main.width-editor.cursor.x)+1;
            editor.cursor.x -= shift;
        }
    } else {
        delete_char_at(row, x-1);
        if (editor.cursor.x > 0) editor.cursor.x--;
    }
    editor.dirty++;
}

void delete_char(void)
{
    if (!editor.multicursor.is_enabled || editor.in_cmd) {
        delete_char_internal();
        return;
    }

    Cursor saved_main = editor.cursor;

    for (size_t i = 0; i < editor.sorted_multicursor.count; i++) {
        Cursor *current = editor.sorted_multicursor.items[i];
        editor.cursor = (current == &editor.cursor) ? saved_main : *current;
        delete_char_internal();
        if (current == &editor.cursor) saved_main = editor.cursor;
        else {
            *current = editor.cursor;
            editor.cursor = saved_main;
        }

        // Shift Adjustment for Deletion
        for (size_t j = 0; j < i; j++) {
            Cursor *processed = editor.sorted_multicursor.items[j];
            // If we are on same line, shift left
            if (processed->y == current->y && processed->x > 0) {
                processed->x--;
            }
            // If we merged lines (deleted newline), shift rows up
            // (Detection of line merge requires checking if row count changed, 
            // which is tricky here without return values. 
            // Basic implementation: Shift X left.)
            if (processed == &editor.cursor) saved_main = *processed;
        }
    }
    editor.cursor = saved_main;
}

// TODO: non funziona
void delete_word()
{
    if (editor.in_cmd) {
        if (isspace(editor.cmd.items[editor.cmd_pos])) {
            while (editor.cmd_pos > 0 && isspace(editor.cmd.items[editor.cmd_pos])) {
                da_remove(&editor.cmd, editor.cmd_pos-1);
                editor.cmd_pos--;
            }
        } else {
            while (editor.cmd_pos > 0 && !isspace(editor.cmd.items[editor.cmd_pos])) {
                da_remove(&editor.cmd, editor.cmd_pos-1);
                editor.cmd_pos--;
            }
        }
        return;
    }

    size_t y = CURRENT_Y_POS;
    if (y >= editor.rows.count) return;
    size_t x = CURRENT_X_POS;
    if (x == 0 && y == 0) return;
    Row *row = CURRENT_ROW;
    if (isspace(CHAR(CURRENT_Y_POS, x))) {
        while (x > 0 && isspace(CHAR(CURRENT_Y_POS, x))) {
            delete_char_at(row, x-1);
            x--;
        }
    } else {
        while (x > 0 && !isspace(CHAR(CURRENT_Y_POS, x))) {
            delete_char_at(row, x-1);
            x--;
        }
    }
    editor.cursor.x = x;
    editor.dirty++;
}

bool set_N(int key)
{
    int digit = key - ALT_0;
    if (editor.N == N_DEFAULT) {
        if (digit == 0) return false;
        else editor.N = digit;
    } else {
        editor.N *= 10;
        editor.N += digit;
    }
    return true;
}

void expanding_snippet_next_mark(void)
{
    if (!editor_is_expanding_snippet()) return;

    da_clear(&editor.multicursor);
    editor.multicursor.is_enabled = false;

    Snippet *snippet = editor.expanding_snippet.snippet;
    size_t mark_index = editor.expanding_snippet.mark_index + 1;
    log_this("expanding_snippet_next_mark: %zu", mark_index);
    while (mark_index < snippet->marks_count && !snippet->marks[mark_index].is_primary)
        mark_index++;
    log_this("skipped secondary marks");
    if (mark_index >= snippet->marks_count) {
        log_this("finished expanding snippet");
        editor.expanding_snippet.snippet = NULL;
        editor.expanding_snippet.mark_index = -1;
        disable_multicursor();
        return;
    }

    SnippetMark mark = snippet->marks[mark_index];
    log_this("Setting mark `%s` - %s - (%zu, %zu)", mark.name ? mark.name : "unnamed",
            mark.is_primary ? "primary" : "secondary", mark.cursor.x, mark.cursor.y);
    editor.expanding_snippet.mark_index = mark_index;
    size_t indentation = editor.expanding_snippet.base_cursor.x;

    Cursor absolute_cursor_from(Cursor relative_cursor)
    {
        return (Cursor){
            .x = ((relative_cursor.y == 0) ? editor.expanding_snippet.base_cursor.x : indentation) + relative_cursor.x,
            .y = editor.expanding_snippet.base_cursor.y + relative_cursor.y
        };
    }
    editor.cursor = absolute_cursor_from(mark.cursor);
    log_this("Set primary mark at (%zu, %zu)", editor.cursor.x, editor.cursor.y);

    if (mark.name) {
        for (size_t i = mark_index+1; i < snippet->marks_count; i++) {
            SnippetMark *candidate = &snippet->marks[i];
            if (candidate->name && streq(candidate->name, mark.name)) {
                Cursor candidate_absolute_cursor = absolute_cursor_from(candidate->cursor);
                log_this("Set secondary mark at (%zu, %zu)", candidate_absolute_cursor.x, candidate_absolute_cursor.y);
                da_push(&editor.multicursor, candidate_absolute_cursor);
            }
        }
    }

    if (!da_is_empty(&editor.multicursor)) enable_multicursor();
}

void try_to_expand_snippet(void)
{
    Snippet *snippet_to_expand = NULL;
    da_foreach (editor.snippets, Snippet, snippet) {
        if (editor.in_cmd && !snippet->is_inline) continue; // NOTE: cannot expand non inline snippets in command line
        if (CURRENT_X_POS < snippet->handle_len) continue;
        assert(snippet->handle);
        size_t i = 0;
        bool matches = true;
        while (i < snippet->handle_len) {
            char snippet_char = snippet->handle[snippet->handle_len-i-1];
            char current_char = CHAR(CURRENT_Y_POS, CURRENT_X_POS-i-1);
            if (snippet_char != current_char) {
                matches = false;
                break;
            }
            i++;
        }
        if (matches && (!snippet_to_expand || snippet->handle_len > snippet_to_expand->handle_len))
            snippet_to_expand = snippet;
    }
    if (!snippet_to_expand) {
        write_message("ERROR: no snippet handle found");
        return;
    }

    log_this("Expanding snippet: '%s' (%zu - %s) -> '%s'", snippet_to_expand->handle, snippet_to_expand->handle_len,
            BOOL_AS_CSTR(snippet_to_expand->is_inline), snippet_to_expand->body);

    char *body = snippet_to_expand->body;
    if (snippet_to_expand->handle_is_prefix) {
        body += snippet_to_expand->handle_len;
    } else {
        for (size_t i = 0; i < snippet_to_expand->handle_len; i++) delete_char();
    }

    editor.expanding_snippet.base_cursor.x = CURRENT_X_POS - snippet_to_expand->handle_len;
    editor.expanding_snippet.base_cursor.y = CURRENT_Y_POS;

    if (snippet_to_expand->is_inline) insert_cstr(body); 
    else {
        // TODO: it should check for tabs
        size_t indentation = CURRENT_X_POS - (snippet_to_expand->handle_is_prefix ? snippet_to_expand->handle_len : 0);
        char *current = body;
        char *next_newline = NULL;
        while (current) {
            next_newline = strchr(current, '\n');
            if (next_newline) *next_newline = '\0';
            if (current != body && *current)
                // TODO: it may insert a tab based on indentation size and config
                for (size_t i = 0; i < indentation; i++) insert_char(' ');
            insert_cstr(current);
            if (next_newline) {
                insert_char('\n');
                current = next_newline + 1;
            } else current = NULL;
        }
    }


    if (snippet_to_expand->marks_count > 0) {
        log_this("\nMarks:");
        for (size_t i = 0; i < snippet_to_expand->marks_count; i++) {
            SnippetMark *m = &snippet_to_expand->marks[i];
            log_this("Mark `%s` - %s - (%zu, %zu)", m->name ? m->name : "unnamed",
                    m->is_primary ? "primary" : "secondary", m->cursor.x, m->cursor.y);
        }
        log_this("\n");
        editor.expanding_snippet.snippet = snippet_to_expand;
        editor.expanding_snippet.mark_index = -1;
        expanding_snippet_next_mark();
    }
}

void process_pressed_key(void)
{
    int key = read_key();
    if (key == ERR) return;

    bool has_inserted_number = false;

    switch (key)
    {
        case ALT_0:
        case ALT_1:
        case ALT_2:
        case ALT_3:
        case ALT_4:
        case ALT_5:
        case ALT_6:
        case ALT_7:
        case ALT_8:
        case ALT_9:
            if (set_N(key)) has_inserted_number = true;
            break;

        case KEY_UP:
        case ALT_k: N_TIMES move_cursor_up();    break;

        case KEY_DOWN:
        case ALT_j: N_TIMES move_cursor_down();  break;

        case KEY_LEFT:
        case ALT_h: N_TIMES move_cursor_left();  break;

        case KEY_RIGHT:
        case ALT_l: N_TIMES move_cursor_right(); break;

        case ALT_K: move_cursor_begin_of_screen(); break;
        case ALT_J: move_cursor_end_of_screen();   break;
        case ALT_H: move_cursor_first_non_space(); break;
        case ALT_L: move_cursor_last_non_space();  break;

        case ALT_m:
            if (!cs_is_empty(editor.messages)) {
                    editor.is_showing_message = !editor.is_showing_message;
            }
            break;

        case ALT_p:
            if (editor.in_cmd) {
                char *previous_command = cs_previous(&editor.commands_history);
                size_t len = strlen(previous_command);
                s_clear(&editor.cmd);
                s_push_str(&editor.cmd, previous_command, len);
                editor.cmd_pos = len;
            } else cs_previous(&editor.messages);
            break;

        case ALT_n:
            if (editor.in_cmd) {
                char *next_command = cs_next(&editor.commands_history);
                size_t len = strlen(next_command);
                s_clear(&editor.cmd);
                s_push_str(&editor.cmd, next_command, len);
                editor.cmd_pos = len;
            } else cs_next(&editor.messages);
            break;

        case ALT_c: add_multicursor_mark(); break;
        case ALT_C: enable_multicursor(); break;
        case CTRL_ALT_C: disable_multicursor(); break;

        case CTRL_K: N_TIMES scroll_up();   break;
        case CTRL_J: N_TIMES scroll_down(); break;

        //case CTRL_H: write_message("TODO: CTRL-H"); break;
        //case CTRL_L: write_message("TODO: CTRL-L"); break;

        case KEY_PPAGE:
        case CTRL_ALT_K: N_TIMES move_page_up(); break; 

        case KEY_NPAGE:
        case CTRL_ALT_J: N_TIMES move_page_down(); break;

        //case ALT_h: write_message("TODO: ALT-h"); break;
        //case ALT_l: write_message("TODO: ALT-l"); break;
        //            
        //case ALT_K: move_cursor_begin_of_file(); break;
        //case ALT_J: move_cursor_end_of_file();   break;
        //case ALT_H: move_cursor_begin_of_line(); break;
        //case ALT_L: move_cursor_end_of_line();   break; 

        case ALT_COLON: editor.in_cmd = true; break;
        case ALT_BACKSPACE: N_TIMES delete_word(); break;

        case TAB:
            if (editor.in_cmd) {
                // TODO: autocomplete command
            } else {
                N_TIMES {
                    if (editor.config.tab_to_spaces) {
                        for (size_t i = 0; i < editor.config.tab_spaces_number; i++)
                            insert_char(' ');
                    } else insert_char('\t');
                }
            }
            break;

        case KEY_BTAB:
            if (editor_is_expanding_snippet()) expanding_snippet_next_mark();
            else try_to_expand_snippet();
            break;

        case ENTER: // TODO: se si e' in_cmd si esegue execute_command (che fa anche il resto)
            N_TIMES insert_char('\n');
            break;

        case CTRL_Q:
            if (can_quit()) quit();
            return;

        case CTRL_S:
            save();
            break;

        case KEY_BACKSPACE:
            N_TIMES delete_char();
            break;

        case ESC:
            if (editor.in_cmd) {
                s_push_null(&editor.cmd);
                cs_push(&editor.commands_history, editor.cmd.items);
                editor.in_cmd = false;
                editor.cmd_pos = 0;
                s_clear(&editor.cmd);
            }
            break;

        default:
            if (isprint(key)) N_TIMES insert_char(key);
            break;
    }
    if (!has_inserted_number) editor.N = N_DEFAULT;
    editor.current_quit_times = editor.config.quit_times;
}

int main(int argc, char **argv)
{
    if (argc <= 0 || argc >= 3) {
        printw("TODO: usage\n");
        return 1;
    }

    char *filepath = argc == 2 ? argv[1] : NULL;

    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

    ncurses_init();
    load_config();
    editor_init();
    initialize_colors();
    create_windows();

    if (!open_file(filepath)) {
        if (filepath) print_error_and_exit("Could not open file `%s`. %s.\n", filepath, errno ? strerror(errno) : "");
        else          print_error_and_exit("Could not open new file. %s.\n", errno ? strerror(errno) : "");
    }

    while (true) {
        process_pressed_key();
        update_windows();
        update_cursor();
        doupdate();
    }

    // NOTE: this code should be unreachable
    ncurses_end();

    return 0;
}
