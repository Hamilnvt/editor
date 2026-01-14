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
    char *filename;
    Rows rows;
    int dirty;

    size_t cx;
    size_t cy;
    size_t offset;
    size_t screen_rows;
    size_t screen_cols;
    size_t page;

    Strings messages;
    time_t current_msg_time;

    String cmd;
    size_t cmd_pos;
    bool in_cmd;

    size_t current_quit_times;

    int N; // multiplicity for the commands
} Editor;
static Editor editor = {0};

#define N_DEFAULT -1 
#define N_OR_DEFAULT(n) (assert(n >= 0), (size_t)(editor.N == N_DEFAULT ? (n) : editor.N))
#define N_TIMES for (size_t i = 0; i < N_OR_DEFAULT(1); i++)

#define LINE_NUMBERS_SPACE (config.line_numbers == LN_NO ? 0 : (size_t)log10(editor.rows.count)+3)
#define CURRENT_Y_POS (editor.offset+editor.cy)
#define CURRENT_X_POS (editor.cx)
#define ROW(i) (assert((i) <= editor.rows.count), &editor.rows.items[i])
#define CURRENT_ROW ROW(CURRENT_Y_POS)
#define LINE(i) (ROW(i)->content.items)
#define CURRENT_LINE LINE(CURRENT_Y_POS)
#define CHAR(row, i) (LINE(row)[i])
#define CURRENT_CHAR CHAR(CURRENT_Y_POS, CURRENT_X_POS)
#define N_PAGES (editor.rows.count/win_main.height + 1)

/* ANSI escape sequences */
#define ANSI_GO_HOME_CURSOR "\x1B[H"
#define ANSI_SHOW_CURSOR "\x1b[?25h"
#define ANSI_HIDE_CURSOR "\x1b[?25l"
#define ANSI_ERASE_LINE_FROM_CURSOR "\x1b[K"
#define ANSI_CLEAR_SCREEN "\x1b[2J"
#define ANSI_INVERSE "\x1b[7m"
#define ANSI_FG_COLOR(rgb_color) "\x1b[38;2;"rgb_color"m"
#define ANSI_RESET "\x1b[0m"
#define ANSI_SAVE_CURSOR "\x1b[s"
#define ANSI_RESTORE_CURSOR "\x1b[u"

/* Colors */
typedef enum
{
    ED_COLOR_DEFAULT_BACKGROUND = 100,
    ED_COLOR_DEFAULT_FOREGROUND,
    ED_COLOR_YELLOW
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

    ALT_K,
    ALT_J,
    ALT_H,
    ALT_L,
    ALT_M,

    // TODO
    //CTRL_ALT_k,
    //CTRL_ALT_j,
    //CTRL_ALT_h,
    //CTRL_ALT_l,

    //CTRL_ALT_K,
    //CTRL_ALT_J,
    //CTRL_ALT_H,
    //CTRL_ALT_L,

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

void enqueue_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024] = {0};
    vsnprintf(buf, win_message.width, fmt, ap);  // TODO: si puo' anche togliere la limitazione di 1024,
                                                 // tanto ora posso creare una finestra grande a piacere
                                                 // (ovviamente non oltre le dimensioni del terminale, per ora)
    da_push(&editor.messages, strdup(buf)); // NOTE: rember to free
    va_end(ap);
}

static inline bool are_there_pending_messages(void) { return !da_is_empty(&editor.messages); }

void next_message(void)
{
    if (!are_there_pending_messages()) return;
    char *msg = editor.messages.items[0];
    da_remove_first(&editor.messages);
    free(msg);
    editor.current_msg_time = time(NULL);
}

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
#define INSERT            "i"
#define DATE              "date"
#define GOTO_LINE         "goto"

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
        size_t placeholder_index;
    };
    size_t index;
    char *name;
} CommandArg;

CommandArg make_command_arg_int(int value, size_t index, char *name) 
{
    return (CommandArg){
        .type = PISQUY_INT,
        .int_value = value,
        .index = index,
        .name = strdup(name)
    };
}

CommandArg make_command_arg_uint(size_t value, size_t index, char *name) 
{
    return (CommandArg){
        .type = PISQUY_UINT,
        .uint_value = value,
        .index = index,
        .name = strdup(name)
    };
}

CommandArg make_command_arg_string(char *value, size_t index, char *name) 
{
    return (CommandArg){
        .type = PISQUY_STRING,
        .string_value = strdup(value),
        .index = index,
        .name = strdup(name)
    };
}

typedef struct
{
    CommandArg *items;
    size_t count;
    size_t capacity;
} CommandArgs;

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

typedef struct
{
    int index;
    char **items;
    size_t count;
    size_t capacity;
} CommandsHistory;
static CommandsHistory commands_history = {0};

// index == 1
// [ciao, caro]
// 1. :ciao\n
// 2. :caro\n
// 3. :C-P

void commands_history_add(char *cmd)
{
    //log_this("Adding command in history: `%s`", cmd);
    da_push(&commands_history, strdup(cmd));
    commands_history.index = -1;
}

void commands_history_previous()
{
    if (!editor.in_cmd) return;
    if (da_is_empty(&commands_history)) return;
    if (commands_history.index == 0) return;
    if (commands_history.index == -1) commands_history.index = commands_history.count-1;
    else commands_history.index--;
    char *cmd = commands_history.items[commands_history.index];
    size_t len = strlen(cmd);
    s_clear(&editor.cmd);
    s_push_str(&editor.cmd, cmd, len);
    editor.cmd_pos = len;
}

void commands_history_next()
{
    if (!editor.in_cmd) return;
    if (da_is_empty(&commands_history)) return;
    if (commands_history.index >= (int)commands_history.count-1) return;
    commands_history.index++;
    char *cmd = commands_history.items[commands_history.index];
    size_t len = strlen(cmd);
    s_clear(&editor.cmd);
    s_push_str(&editor.cmd, cmd, len);
    editor.cmd_pos = len;
}

#define USER_CMDS_COUNT (commands.count - BUILTIN_CMDS_COUNT)

bool expect_n_arguments(Command *cmd, CommandArgs *args, size_t n)
{
    if (args->count == n) return true;

    enqueue_message("ERROR: command `%s` expects %zu argument%s, but got %zu",
            cmd->name, n, n == 1 ? "" : "s", args->count);
    return false;
}

CommandType parse_cmdtype(char *type)
{
    static_assert(BUILTIN_CMDS_COUNT == 14, "Parse all commands in parse_cmd");
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
        for (size_t i = BUILTIN_CMDS_COUNT; i < commands.count; i++) {
            if (streq(type, commands.items[i].name))
                return USER_DEFINED + i - BUILTIN_CMDS_COUNT;
        }
        return UNKNOWN;
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

void add_user_command(Command cmd)
{
    cmd.type = USER_DEFINED + commands.count - BUILTIN_CMDS_COUNT;
    da_push(&commands, cmd);
}

int get_command_index(const Command *cmd)
{
    if (cmd->type >= USER_DEFINED) return cmd->type - USER_DEFINED + BUILTIN_CMDS_COUNT;
    else if (cmd->type >= 0 && cmd->type < BUILTIN_CMDS_COUNT) return cmd->type;
    else return -1;
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
    if (cmd->type > BUILTIN_CMDS_COUNT && cmd->type != COMMAND_FROM_LINE)
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

//void print_commands(const Commands *cmds)
//{
//    log_this("Commands:");
//    for (size_t i = 0; i < cmds->count; i++) {
//        const Command *cmd = &cmds->items[i];
//        log_this("%d. %d", i, cmd->type);
//        if (cmd->args.count > 0) log_this("args:");
//        for (size_t j = 0; j < cmd->args.count; j++) {
//            char *arg = cmd->args.items[j];
//            log_this(" - %d. %s", j, arg);
//        }
//    }
//}

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
static Vars vars = {0};

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

static_assert(ACTUAL_TOKENS_COUNT == 10, "token_type_as_cstr");
char *token_type_as_cstr(TokenType type)
{
    if (type >= 0 && type <= 255) {
        if (isprint(type)) return "char";
        else               return "control char";
    } else {
        switch (type)
        {
            case TOKEN_IDENT:   return "identifier";
            case TOKEN_NUMBER:  return "number";
            case TOKEN_STRING:  return "string";
            case TOKEN_TRUE:    return "true";
            case TOKEN_FALSE:   return "false";
            case TOKEN_SET:     return "set";
            case TOKEN_VAR:     return "var";
            case TOKEN_DEF:     return "def";
            case TOKEN_NEWLINE: return "new line";
            case TOKEN_EOF:     return "EOF";
            default:            return NULL;
        }
    }
}

static_assert(ACTUAL_TOKENS_COUNT == 10, "token_type_and_value_as_str");
char *token_type_and_value_as_str(Token token)
{
    const size_t n = 128;
    char *result = malloc(sizeof(char)*n);
    char *type = token_type_as_cstr(token.type);
    if (token.type >= 0 && token.type <= 255) {
        if (isprint(token.type)) sprintf(result, "%s '%c'", type, token.type);
        else                     sprintf(result, "%s '%d'", type, token.type);
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
            case TOKEN_NEWLINE:
            case TOKEN_EOF:
                sprintf(result, "%s", type);
                break;
            default: print_error_and_exit("Unreachable");
        }
    }
    return result;
}

void token_log(Token token)
{
    char *string = token_type_and_value_as_str(token);
    log_this(LOC_FMT" %s", LOC_ARG(token.loc), string);
    free(string);
}

#define NO_LOG NULL
#define EXTRA_NEWLINE true
bool expect_token_to_be_of_type(Token t, TokenType type, String *s_log)
{
    if (t.type == type) return true;

    if (s_log) {
        char *tokstrval = token_type_and_value_as_str(t);
        s_push_fstr(s_log, LOC_FMT"\n- ERROR: expecting %s but got %s\n", LOC_ARG(t.loc),
                token_type_as_cstr(type), tokstrval);
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
            s_push_fstr(s_log, "`%s`", token_type_as_cstr(types[i]));
            if (i < n-1) s_push_fstr(s_log, ", ");
        }
        char *tokstrval = token_type_and_value_as_str(t);
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

Lexer lexer_create(char *path)
{
    Lexer l = {0};
    l.loc.path = path;
    l.str = read_file(path);
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

bool lexer_string(Lexer *l)
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

static_assert(ACTUAL_TOKENS_COUNT == 10, "lexer_next");
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
        if      (strneq(begin, "set", len)) l->token.type = TOKEN_SET;
        else if (strneq(begin, "var", len)) l->token.type = TOKEN_VAR;
        else if (strneq(begin, "def", len)) l->token.type = TOKEN_DEF;
        else if (strneq(begin, "true", len)) l->token.type = TOKEN_TRUE;
        else if (strneq(begin, "false", len)) l->token.type = TOKEN_FALSE;
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
        lexer_string(l);
    } else {
        l->token.type = c;
        l->str++;
        l->loc.col++;
    }
    return true;
}

static_assert(ACTUAL_TOKENS_COUNT == 10, "lexer_get_current_token");
Token lexer_get_current_token(Lexer *l)
{
    Token token = l->token;
    token.loc.path = strdup(l->loc.path);

    if (token.type >= 0 && token.type <= 255) return token;

    switch (token.type)
    {
    case TOKEN_NUMBER:
    case TOKEN_TRUE:
    case TOKEN_FALSE:
    case TOKEN_SET:
    case TOKEN_VAR:
    case TOKEN_DEF:
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

static_assert(ACTUAL_TOKENS_COUNT == 10, "free_token");
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

Tokens lex_file(char *path)
{
    log_this("Lexing file `%s`...", path);
    Lexer lex = lexer_create(path); 
    Tokens tokens = {0};
    while (lexer_next(&lex)) {
        Token token = lexer_get_current_token(&lex);
        da_push(&tokens, token);
        token_log(token);
        lexer_free_current_token(&lex);
    }
    Token eof = { .type = TOKEN_EOF };
    da_push(&tokens, eof);
    return tokens;
}

#define NO_LOCATION NULL
char *parse_commands(char *cmds_str, Command *cmd, CommandArgs *args, Location *loc)
{
    (void)args; // TODO

    if (loc) loc->col += trim_left(&cmds_str);
    //log_this("- Parsing cmds from `%s`", cmds_str);
    size_t i = 0;
    size_t len = strlen(cmds_str);
    while (i < len && strlen(cmds_str) > 0) {
        Command subcmd = {0};

        if (isdigit(*cmds_str)) {
            char *end_of_n;
            long n = strtol(cmds_str, &end_of_n, 10);
            if (n <= 0) {
                char *error = malloc(sizeof(char)*256);
                sprintf(error, "command multiplicity must be greater than 0, but got `%ld`", n);
                return error;
            } else subcmd.n = n;
            if (loc) loc->col += end_of_n - cmds_str;
            i += end_of_n - cmds_str;
            cmds_str = end_of_n;
        } else subcmd.n = 1;
        //log_this("multiplicity: %zu", subcmd.n);

        char *end_of_cmd_name = strpbrk(cmds_str, " \t(");
        bool has_args = false;
        if (end_of_cmd_name != NULL) {
            if (*end_of_cmd_name == '(') has_args = true;
            *end_of_cmd_name = '\0';
        }
        subcmd.type = parse_cmdtype(cmds_str);
        if (subcmd.type == UNKNOWN) {
            char *error = malloc(sizeof(char)*256);
            sprintf(error, "unknown command `%s`", cmds_str);
            return error;
        } else {
            //log_this("name: `%s` => type: %d", cmds_str, subcmd.type);
        }

        int index = get_command_index(&subcmd);
        if (index == -1) {
            print_error_and_exit("FATAL: unreachable command `%d`\n", subcmd.type);
        }
        const Command *ref_cmd = &commands.items[index];
        subcmd.name = ref_cmd->name;
        subcmd.execute = ref_cmd->execute;
        if (ref_cmd->baked_args.count > 0)
            subcmd.baked_args = deep_copy_command_args(&ref_cmd->baked_args);
        // TODO: Command does not contains arguments anymore, this logic is moved to the execute function
        //if (has_args && ref_cmd->args.count == 0) {
        //    char *error = malloc(sizeof(char)*256);
        //    sprintf(error, "command `%s` does not require arguments", subcmd.name);
        //    return error;
        //}
        size_t name_len = strlen(cmds_str);
        cmds_str += name_len + 1;
        if (loc) loc->col += name_len;

        // TODO:
        // - parse strings
        // - allow \( and \) in strings
        // - allow \n in strings
        // - allow \" in strings
        if (has_args) {
            char *end_of_cmd_args = strchr(cmds_str, ')');
            if (end_of_cmd_args == NULL) return strdup("unclosed arguments parenthesis");

            if (loc) loc->col += trim_left(&cmds_str);
            size_t argslen = end_of_cmd_args - cmds_str;
            if (argslen == 0) return strdup("no arguments provided");
            if (loc) loc->col++;

            //log_this("  > Parsing arguments `%s` (%zu)", cmds_str, argslen);
            size_t args_count = 0;
            char *arg = cmds_str;
            bool in_string = false;
            (void)in_string;
            size_t j = 0;
            while (j < argslen+1) {
                size_t trim = trim_left(&cmds_str);
                j += trim;
                arg += trim;
                if (loc) loc->col += trim;

                char c = cmds_str[j];
                if (c == ',' || c == ')') { // TODO: next arg
                    size_t arglen = arg - cmds_str;
                    if (arglen == 0) {
                        char *error = malloc(sizeof(char)*256);
                        // TODO: report the type maybe
                        sprintf(error, "Argument %zu not provided", args_count+1);
                        return error;
                    }
                    //log_this("    - TODO: parse argument `%.*s`", arglen, cmds_str);
                    cmds_str += arglen;
                    argslen -= arglen;
                    j = 1;
                    arg++;
                    if (loc) loc->col += arglen + 1;
                    args_count++;
                } else {
                    j++;
                    arg++;
                    if (loc) loc->col++;
                }
            }

            if (loc) loc->col += strlen(cmds_str) + 1;
            cmds_str = end_of_cmd_args + 1;
        }

        da_push(&cmd->subcmds, subcmd);
        if (loc) loc->col += trim_left(&cmds_str);
    }

    return NULL;
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
    FIELDS_COUNT,
} FieldType;

static_assert(FIELDS_COUNT == 5, "fieldtype_to_string");
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

ConfigField create_field_bool(char *name, bool *ptr, bool thefault)
{
    return (ConfigField){
        .name = name,
        .type = FIELD_BOOL,
        .bool_ptr = ptr,
        .bool_default = thefault
    };
}

ConfigField create_field_int(char *name, int *ptr, int thefault)
{
    return (ConfigField){
        .name = name,
        .type = FIELD_INT,
        .int_ptr = ptr,
        .int_default = thefault
    };
}

ConfigField create_field_uint(char *name, size_t *ptr, size_t thefault)
{
    return (ConfigField){
        .name = name,
        .type = FIELD_UINT,
        .uint_ptr = ptr,
        .uint_default = thefault
    };
}

ConfigField create_field_string(char *name, char **ptr, char *thefault)
{
    return (ConfigField){
        .name = name,
        .type = FIELD_STRING,
        .string_ptr = ptr,
        .string_default = strdup(thefault)
    };
}

ConfigField create_field_limited_string(char *name, LimitedStringIndex *ptr, LimitedStringIndex thefault, Strings valid_values)
{
    return (ConfigField){
        .name = name,
        .type = FIELD_LIMITED_STRING,
        .limited_string_ptr = ptr,
        .limited_string_default = thefault,
        .limited_string_valid_values = valid_values
    };
}

typedef enum { LN_NO, LN_ABS, LN_REL } ConfigLineNumbers;

typedef struct
{
    size_t quit_times;
    time_t msg_lifetime;
    ConfigLineNumbers line_numbers;
} Config;
Config config = {0};

void save(void)
{
    String save_buf = {0};
    da_foreach(editor.rows, Row, row) {
        s_push_str(&save_buf, row->content.items, row->content.count);
        s_push(&save_buf, '\n');
    }

    // TODO: make the user decide, in the status line, the name of the file if not set
    // - probabilmente devo fare un sistema che permetta di cambiare l'inizio della command line (ora e' sempre Command:) e poi fare cose diverse una volta che si e' premuto ENTER
    int fd = open(editor.filename, O_RDWR|O_CREAT, 0644);
    if (fd == -1) goto writeerr;

    /* Use truncate + a single write(2) call in order to make saving
     * a bit safer, under the limits of what we can do in a small editor. */
    ssize_t len = save_buf.count;
    if (ftruncate(fd, len) == -1) goto writeerr;
    if (write(fd, save_buf.items, len) != len) goto writeerr;

    close(fd);
    s_free(&save_buf);
    editor.dirty = 0;
    enqueue_message("%zu bytes written on disk", len);
    return;

writeerr:
    s_free(&save_buf);
    if (fd != -1) close(fd);
    enqueue_message("Can't save! I/O error: %s", strerror(errno));
}

// TODO: argomento per salvare il file con un nome
void builtin_save(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;
    save();
}

_Noreturn void quit() { exit(0); }

bool can_quit(void)
{
    if (!editor.dirty || editor.current_quit_times == 0) return true;

    // TODO: set higher priority messages that overwrite the current and then restore them after msg_lifetime secs
    // - da pensare 
    enqueue_message("Session is not saved. If you really want to quit press CTRL-q %zu more time%s.", editor.current_quit_times-1, editor.current_quit_times-1 == 1 ? "" : "s");
    editor.current_quit_times--;
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

void move_cursor_up(void)
{
    if (editor.cy == 0 && editor.page != 0) {
        editor.offset--;
        // TODO: deve cambiare anche la pagina, ma come?
    }
    if (editor.cy - N_OR_DEFAULT(1) > 0) editor.cy -= N_OR_DEFAULT(1);
    else editor.cy = 0;
}

void move_cursor_down(void)
{ 
    //if ((editor.offset+N_OR_DEFAULT(1) / N_PAGES) > editor.page) editor.page += (editor.offset+N_OR_DEFAULT(1)) / N_PAGES; // TODO: No, non funziona cosi' devo modificare l'offset in questo caso
    if (editor.cy == win_main.height-1 && editor.page != N_PAGES-1) {
        editor.offset++;
        // TODO: deve cambiare anche la pagina, ma come?
    }
    if (editor.cy+N_OR_DEFAULT(1) < win_main.height-1) editor.cy += N_OR_DEFAULT(1);
    else editor.cy = win_main.height-1;
    //if (editor.offset < editor.rows.count-N_OR_DEFAULT(0)-1) editor.offset += N_OR_DEFAULT(1); 
    //else editor.offset = editor.rows.count-1;
}

void move_cursor_left(void)
{
    if (editor.in_cmd) {
        if (editor.cmd_pos > 0) editor.cmd_pos -= 1;
        else editor.cmd_pos = 0;
    } else {
        if (editor.cx > 0) editor.cx -= 1;
        else editor.cx = 0;
    }
}

void move_cursor_right(void)
{
    if (editor.in_cmd) {
        if (editor.cmd_pos < editor.cmd.count-1) editor.cmd_pos += 1;
        else editor.cmd_pos = editor.cmd.count;
    } else {
        if (editor.cx < win_main.width-1) editor.cx += 1;
        else editor.cx = win_main.width-1;
    }
}

void builtin_move_cursor(Command *cmd, CommandArgs *args) 
{
    (void)cmd;

    if (args->count != 2) {
        enqueue_message("COMMAND ERROR: command %s takes 2 arguments, but got %zu", args->count);
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
    editor.cy--;
    editor.dirty++;
}

void builtin_move_line_down()
{
    size_t y = CURRENT_Y_POS;
    if (y == win_main.height-1) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y+1];
    editor.rows.items[y+1] = tmp;
    editor.cy++;
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

void execute_command(Command *cmd, CommandArgs *args); // forward declaration

void insert_char(char c)
{
    if (editor.in_cmd) {
        if (c == '\n') {
            s_push_null(&editor.cmd);
            char *cmd_str = editor.cmd.items;
            commands_history_add(cmd_str);
            editor.in_cmd = false;
            s_clear(&editor.cmd);
            editor.cmd_pos = 0;

            Command cmd_from_line = {
                .name = "command from line",
                .type = COMMAND_FROM_LINE,
                .n = 1 // TODO
            };
            CommandArgs runtime_args = {0};
            char *parse_error = parse_commands(cmd_str, &cmd_from_line, &runtime_args, NO_LOCATION);
            if (parse_error != NULL) {
                enqueue_message("ERROR: %s", parse_error);
                free(parse_error);
            } else {
                execute_command(&cmd_from_line, &runtime_args);
            }
            //free_commands(&cmds); // TODO
            //free_command_args(&args); // TODO
        } else {
            da_insert(&editor.cmd, c, editor.cmd_pos);
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
        if (editor.cy == win_main.height-1) editor.offset++;
        else editor.cy++;
        editor.cx = 0;
    } else {
        if (y >= editor.rows.count) {
            while (editor.rows.count <= y) {
                Row newrow = {0};
                da_push(&editor.rows, newrow);
            }
        }
        insert_char_at(CURRENT_ROW, x, c);
        editor.cx++;
    }
    editor.dirty++;
}

void builtin_insert(Command *cmd, CommandArgs *args)
{
    if (!expect_n_arguments(cmd, args, 1)) return;
    char *str = args->items[0].string_value;
    size_t len = strlen(str);
    for (size_t j = 0; j < len; j++) {
        insert_char(str[j]);
    }
}

void builtin_date(Command *cmd, CommandArgs *args)
{
    (void)cmd;
    (void)args;

    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date[64];
    // TODO: error message if it fails, not an assert
    assert(strftime(date, sizeof(date), "%c", tm));
    log_this("%s", date);
    for (size_t i = 0; i < strlen(date); i++) {
        insert_char(date[i]);
    }
}

void builtin_goto_line(Command *cmd, CommandArgs *args)
{
    if (!expect_n_arguments(cmd, args, 1)) return;
    size_t line = args->items[0].uint_value;
    enqueue_message("TODO: goto line %zu", line);
}

int read_key(); // Forward declaration
void load_config()
{
    char *home = getenv("HOME");
    if (home == NULL) {
        print_error_and_exit("Env variable HOME not set\n");
    }

    const size_t default_quit_times = 3;
    const size_t default_msg_lifetime = 3;
    const ConfigLineNumbers default_line_numbers = LN_NO;

    const char *valid_values_field_bool[] = {"true", "false", NULL};
    (void)valid_values_field_bool;

    Strings valid_values_line_numbers = {0};
    {
        char *values[] = {"no", "absolute", "relative"};
        da_push_many(&valid_values_line_numbers, values, 3);
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
            goto load_config_fail;
        }

        fprintf(config_file, "set quit_times = %zu\t// times you need to press CTRL-q before exiting without saving\n",
                default_quit_times); // TODO: make the description a macro/const
        fprintf(config_file, "set msg_lifetime = %zu\t// time in seconds of the duration of a message\n",
                default_msg_lifetime);
        fprintf(config_file, "set line_numbers = %s\t// display line numbers at_all, absolute or relative to the current line\n",
                valid_values_line_numbers.items[default_line_numbers]);

        s_push_fstr(&config_log, "NOTE: default config file has been created\n\n");
        rewind(config_file);
    }

    ConfigFields remaining_fields = {0};

    da_push(&remaining_fields, create_field_uint("quit_times", &config.quit_times, default_quit_times));
    da_push(&remaining_fields, create_field_uint("msg_lifetime", (size_t *)&config.msg_lifetime, default_msg_lifetime));
    da_push(&remaining_fields, create_field_limited_string("line_numbers", (LimitedStringIndex *)&config.line_numbers,
                default_line_numbers, valid_values_line_numbers));
    
    if (DEBUG) {
        for (size_t i = 0; i < remaining_fields.count; i++) {
            const ConfigField *field = &remaining_fields.items[i];
            s_push_fstr(&config_log, "Field { name: `%s`, type: `%s` }\n",
                    field->name, fieldtype_to_string(field->type));
        }
        s_push(&config_log, '\n');
    }

    ConfigFields inserted_fields = {0};

    commands = (Commands){0};
    static_assert(BUILTIN_CMDS_COUNT == 14, "Add all builtin commands in commands");
    add_builtin_command(SAVE,              BUILTIN_SAVE,              builtin_save,              NULL);
    add_builtin_command(QUIT,              BUILTIN_QUIT,              builtin_quit,              NULL);
    add_builtin_command(SAVE_AND_QUIT,     BUILTIN_SAVE_AND_QUIT,     builtin_save_and_quit,     NULL);
    add_builtin_command(FORCE_QUIT,        BUILTIN_FORCE_QUIT,        builtin_force_quit,        NULL);

    CommandArgs baked_args = {0};
    da_push(&baked_args, make_command_arg_int(0,  0, "x"));
    da_push(&baked_args, make_command_arg_int(-1, 1, "y"));
    add_builtin_command(MOVE_CURSOR_UP, BUILTIN_MOVE_CURSOR_UP, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    da_push(&baked_args, make_command_arg_int(0, 0, "x"));
    da_push(&baked_args, make_command_arg_int(1, 1, "y"));
    add_builtin_command(MOVE_CURSOR_DOWN, BUILTIN_MOVE_CURSOR_DOWN, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    da_push(&baked_args, make_command_arg_int(-1, 0, "x"));
    da_push(&baked_args, make_command_arg_int(0,  1, "y"));
    add_builtin_command(MOVE_CURSOR_LEFT, BUILTIN_MOVE_CURSOR_LEFT, builtin_move_cursor, &baked_args);

    da_clear(&baked_args);
    da_push(&baked_args, make_command_arg_int(1, 0, "x"));
    da_push(&baked_args, make_command_arg_int(0, 1, "y"));
    add_builtin_command(MOVE_CURSOR_RIGHT, BUILTIN_MOVE_CURSOR_RIGHT, builtin_move_cursor, &baked_args);

    add_builtin_command(MOVE_LINE_UP,      BUILTIN_MOVE_LINE_UP,      builtin_move_line_up,      NULL);
    add_builtin_command(MOVE_LINE_DOWN,    BUILTIN_MOVE_LINE_DOWN,    builtin_move_line_down,    NULL);
    add_builtin_command(INSERT,            BUILTIN_INSERT,            builtin_insert,            NULL);
    add_builtin_command(DATE,              BUILTIN_DATE,              builtin_date,              NULL);
    add_builtin_command(GOTO_LINE,         BUILTIN_GOTO_LINE,         builtin_goto_line,         NULL);

    free_command_args(&baked_args);

    Tokens tokens = lex_file(full_config_path);

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
                                        removed_field.bool_default ? "true" : "false");
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
                                    s_push_fstr(&config_log, LOC_FMT"- ERROR: empty string\n");
                                } else if (is_not_valid) {
                                    s_push_fstr(&config_log, LOC_FMT"- ERROR: string not valid\n");
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
                        s_push_fstr(&config_log, LOC_FMT"- ERROR: redeclaration of field `%s`\n\n",
                                LOC_ARG(token_field_name.loc), field_name);
                        found = true;
                    } else k++;
                }
            }

            if (!found) {
                s_push_fstr(&config_log, LOC_FMT"- ERROR: unknown field `%s`\n\n",
                        LOC_ARG(token_field_name.loc), field_name);
            }
        } break;

        case TOKEN_VAR: {
            i++;
            Token token_var_name = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_var_name, TOKEN_IDENT, &config_log)) continue;
            char *var_name = token_var_name.string_value;
            bool found = false;
            for (size_t j = 0; j < vars.count; j++) {
                if (streq(var_name, vars.items[i].name)) {
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
                s_push_fstr(&config_log, LOC_FMT"- TODO: assign identifier to variable '%s'",
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
            da_push(&vars, var);
        } break;

        case TOKEN_DEF: {
            i++;
            Token token_command_name = tokens.items[i];
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
            if (!expect_token_to_be_of_type_extra_newline(token_command_name, TOKEN_IDENT, &config_log)) continue;
            i++;
            Token token_colon = tokens.items[i];
            if (!expect_token_to_be_of_type_extra_newline(token_colon, ':', &config_log)) continue;

            Command user_defined_cmd = {0};
            i++;
            Token tok_it = tokens.items[i];
            while (tok_it.type != TOKEN_NEWLINE) {
                size_t n = 1; (void)n; // TODO
                if (tok_it.type == TOKEN_NUMBER) {
                    if (tok_it.number_value <= 0) {
                        s_push_fstr(&config_log, LOC_FMT"\n- ERROR: multiplicity number should be > 0 (got %d)\n\n",
                                LOC_ARG(tok_it.loc), tok_it.number_value);
                        break;
                    }
                    n = tok_it.number_value;
                    i++;
                    tok_it = tokens.items[i];
                }
                if (!expect_token_to_be_of_type_extra_newline(tok_it, TOKEN_IDENT, &config_log)) break;

                i++;
                tok_it = tokens.items[i];
            }
            user_defined_cmd.name = strdup(command_name);
            add_user_command(user_defined_cmd);
        } break;

        case TOKEN_NEWLINE: break;
            
        case TOKEN_EOF: 
            assert(i == tokens.count-1);
            break;

        default:
            char *tokstrval = token_type_and_value_as_str(first_token);
            s_push_fstr(&config_log, LOC_FMT"\n- ERROR: unexpected %s\n\n", LOC_ARG(first_token.loc), tokstrval);
            free(tokstrval);
        }
    }

    free_tokens(&tokens);

    //char *colon = NULL;
    //if (*line == '#') {
    //    line++;
    //    loc.col++;
    //    colon = strchr(line, ':');
    //    if (colon == NULL) {
    //        s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
    //        s_push_fstr(&config_log, "ERROR: invalid command, it should be of the form #name: commands...\n");
    //    } else {
    //        *colon = '\0';
    //        char *cmd_name = line;
    //        bool already_defined = false;
    //        da_enumerate(commands, Command, i, command) {
    //            if (streq(cmd_name, command->name)) {
    //                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
    //                s_push_fstr(&config_log, "ERROR: redeclaration of %s command `%s`\n", 
    //                        i < BUILTIN_CMDS_COUNT ? "builtin" : "user defined", cmd_name); 
    //                already_defined = true;
    //                break;
    //            }
    //        }
    //        if (already_defined) {
    //            loc.col = 0;
    //            loc.row++;
    //            continue;
    //        }
    //        loc.col += strlen(cmd_name) + 1;

    //        char *cmds = colon+1;
    //        Command user_defined_cmd = {
    //            .name = strdup(cmd_name),
    //        };
    //        CommandArgs cmd_args = {0};
    //        char *parse_error = parse_commands(cmds, &user_defined_cmd, &cmd_args, &loc);
    //        if (parse_error != NULL) {
    //            s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
    //            s_push_fstr(&config_log, "ERROR: %s\n", parse_error);
    //            free(parse_error);
    //        } else {
    //            // TODO: che ci faccio con cmd_args?
    //            add_user_command(user_defined_cmd);
    //            s_push_fstr(&config_log, "added command `%s`\n", cmd_name);
    //        }
    //    }
    //} else if ((colon = strchr(line, ':')) != NULL) {
    //    *colon = '\0';
    //    char *field_name = line;
    //    char *field_value = colon+1;
    //    while(isspace(*field_value)) {
    //        loc.col++;
    //        field_value++;
    //    }

    if (remaining_fields.count > 0) {
        s_push_cstr(&config_log, "\nWARNING: the following fields have not been set:\n");
        da_foreach(remaining_fields, ConfigField, field) {
            s_push_fstr(&config_log, " -> %s (type: %s, default: ", field->name, fieldtype_to_string(field->type));
            switch (field->type)
            {
                case FIELD_BOOL:
                    s_push_fstr(&config_log, "`%s`)\n", field->bool_default ? "true" : "false");
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
                    s_push_fstr(&config_log, "`%s`)\n", field->limited_string_default);
                    *field->limited_string_ptr = field->limited_string_default;
                    // TODO: magari elencare anche qua i possibili valori
                    break;
                default:
                    print_error_and_exit("Unreachable field type in load_config\n");
            }
        }
    }

    if (DEBUG) {
        s_push_fstr(&config_log, "\nDefined commands:\n");
        da_enumerate (commands, Command, i, command) {
            s_push_fstr(&config_log, "%s: `%s`\n", i < BUILTIN_CMDS_COUNT ? "Builtin" : "User", command->name);
        }
    }

load_config_fail:
    if (s_is_empty(config_log)) return;

    char tmp_filename[] = "/tmp/editor_config_log_XXXXXX";
    int fd = mkstemp(tmp_filename);
    if (fd == -1) print_error_and_exit("Could not create temporary file\n");
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        print_error_and_exit("Could not open temporary file\n"); 
    }

    s_push_null(&config_log);
    fputs(config_log.items, f);
    s_free(&config_log);
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

    clear();
    printw("- Press ENTER to continue ignoring errors and warnings\n");
    printw("- Press ESC to exit\n");
    int key;
    while (true) {
        key = getch();
        if (key == ENTER) break;
        else if (key == ESC) exit(1);
    }
}

/// END Config 

/* Pairs */
typedef enum
{
    DEFAULT_EDITOR_PAIR = 0,
    DEFAULT_EDITOR_PAIR_INV,
    DEFAULT_STATUS_PAIR,
    DEFAULT_MESSAGE_PAIR
} EditorPair;

/* Windows */
static inline void get_screen_size(void) { getmaxyx(stdscr, editor.screen_rows, editor.screen_cols); }

Window create_window(int h, int w, int y, int x, int pair)
{
    Window win = {0};
    win.win = newwin(h, w, y, x);
    win.height = h;
    win.width = w;
    win.start_y = y;
    win.start_x = x;
    wattron(win.win, COLOR_PAIR(pair));
    return win;
}

void create_windows(void)
{
    win_main = create_window(editor.screen_rows-1, editor.screen_cols-LINE_NUMBERS_SPACE, 0, LINE_NUMBERS_SPACE,
            DEFAULT_EDITOR_PAIR);
    win_line_numbers = create_window(editor.screen_rows-1, LINE_NUMBERS_SPACE, 0, 0, DEFAULT_EDITOR_PAIR);
    win_message      = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_EDITOR_PAIR_INV);
    win_command      = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_EDITOR_PAIR_INV);
    win_status       = create_window(1, editor.screen_cols, editor.screen_rows-1, 0, DEFAULT_EDITOR_PAIR);
}

void destroy_windows(void)
{
    delwin(win_main.win);
    delwin(win_line_numbers.win);
    delwin(win_message.win);
    delwin(win_command.win);
    delwin(win_status.win);
}

void ncurses_end(void)
{
    curs_set(1);
    endwin();
    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n");
}

void ncurses_init(void)
{
    initscr();

    raw(); //cbreak();
    noecho();
    nonl();
    nodelay(stdscr, TRUE);
    set_escdelay(25);
    keypad(stdscr, TRUE);

    // TODO: fix damn colors
    if (has_colors()) {
        //start_color();

        init_color(ED_COLOR_DEFAULT_BACKGROUND, 18, 18, 18);
        init_color(ED_COLOR_DEFAULT_FOREGROUND, 5, 20, 5);
        init_color(ED_COLOR_YELLOW, 178, 181, 0);

        init_pair(DEFAULT_EDITOR_PAIR, ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_DEFAULT_BACKGROUND);
        init_pair(DEFAULT_EDITOR_PAIR_INV, ED_COLOR_DEFAULT_BACKGROUND, ED_COLOR_DEFAULT_FOREGROUND);
        init_pair(DEFAULT_MESSAGE_PAIR, COLOR_WHITE, COLOR_RED);
        //init_pair(DEFAULT_STATUS_PAIR, ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_DEFAULT_BACKGROUND);

        //use_default_colors();
    }

    atexit(ncurses_end);
}

int read_key()
{
    int c = getch();
    switch (c) {
        case ESC:
            int next = getch();
            if (next == ERR) return ESC;

            if (getch() != ERR) return ESC;
            switch (next) { // ALT-X sequence
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

                case 'k'          : return ALT_k;
                case 'K'          : return ALT_K;
                case 'j'          : return ALT_j;
                case 'J'          : return ALT_J;
                case 'h'          : return ALT_h;
                case 'H'          : return ALT_H;
                case 'l'          : return ALT_l;
                case 'L'          : return ALT_L;
                case 'M'          : return ALT_M;
                case KEY_BACKSPACE: return ALT_BACKSPACE;
                case ':'          : return ALT_COLON;
                default           : return ESC;
            }
        default: return c;
    }
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error false is returned, on success the position of the
 * cursor is stored at *rows and *cols and true is returned. */
bool get_cursor_position(size_t *rows, size_t *cols)
{
    /* Report cursor location */
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return false;

    char buf[32];
    size_t i = 0;
    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(STDIN_FILENO, buf+i, 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return false;
    if (sscanf(buf+2, "%zu;%zu", rows, cols) != 2) return false;
    return true;
}

/* Try to get the number of columns in the current terminal. If the ioctl()
 * call fails the function will try to query the terminal itself.
 * Returns true on success, false on error. */
bool get_window_size(size_t *rows, size_t *cols)
{
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        //int orig_row, orig_col, res;

        ///* Get the initial position so we can restore it later. */
        //res = get_cursor_position(&orig_row,&orig_col);
        //if (res == -1) return false;
        if (write(STDOUT_FILENO, ANSI_SAVE_CURSOR, 4) != 4) return false;

        /* Go to right/bottom margin and get position. */
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return false;
        if (!get_cursor_position(rows, cols)) return false;

        /* Restore position. */
        if (write(STDOUT_FILENO, ANSI_RESTORE_CURSOR, 4) != 4) return false;
        //char seq[32];
        //snprintf(seq,32,"\x1b[%d;%dH",orig_row,orig_col);
        //if (write(ofd,seq,strlen(seq)) == -1) {
        //    /* Can't recover... */
        //}
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
    return true;
}

void update_window_main(void)
{
    for (size_t i = editor.offset; i < editor.offset+win_main.height; i++) {
        if (i >= editor.rows.count) {
            wprintw(win_main.win, "~\n");
            continue;
        }
        Row *row = ROW(i);
        wprintw(win_main.win, S_FMT"\n", S_ARG(row->content));
    }
}

void update_window_line_numbers(void)
{
    for (size_t i = 0; i < win_line_numbers.height; i++) {
        wprintw(win_line_numbers.win, "%zu\n", i+1);
    }
}

// TODO: non funziona
void update_window_message(void)
{
    if (editor.in_cmd || !are_there_pending_messages()) return;

    if (time(NULL)-editor.current_msg_time > config.msg_lifetime)
        next_message();

    if (!are_there_pending_messages()) return;

    wprintw(win_message.win, editor.messages.items[0]);
}

void update_window_command(void)
{
    wprintw(win_command.win, "Command: ");
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
    if (editor.N != N_DEFAULT) wprintw(win_status.win, "%d", editor.N);
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
    else if (are_there_pending_messages()) update_window(message);
    update_window(status);

    //Row *row;
    //for (size_t y = editor.offset; y < editor.offset+win_main.height; y++) {
    //    // TODO: si puo' fattorizzare la funzione che aggiunge gli spazi per keep_cursor
    //    bool is_current_line = y == editor.cy-editor.offset;

    //    if (y >= editor.rows.count) {
    //        if (is_current_line && editor.in_cmd && editor.cx == 0) s_push_cstr(&screen_buf, ANSI_INVERSE);
    //        s_push(&screen_buf, '~'); // TODO: poi lo voglio togliere, forse, ma ora lo uso per debuggare
    //        if (is_current_line && editor.in_cmd && editor.cx == 0) s_push_cstr(&screen_buf, ANSI_RESET);
    //        if (is_current_line && editor.in_cmd && editor.cx > 0) {
    //            for (size_t x = 1; x < editor.cx; x++) {
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
    //            for (size_t x = 0; x < editor.cx; x++) {
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
    //        bool keep_cursor = is_current_line && editor.in_cmd && x == editor.cx-LINE_NUMBERS_SPACE;
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
    //    if (is_current_line && editor.in_cmd && len <= editor.cx) {
    //        for (size_t x = len; x < editor.cx; x++) {
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
    // * at which the cursor is displayed may be different compared to 'editor.cx'
    // * because of TABs. */
    //size_t cx = editor.cx+1;
    //size_t cy = editor.cy+1;
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
    size_t cy = editor.cy;
    size_t cx = editor.cx;
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
    
    if (editor.cy >= win_main.height) editor.cy = win_main.height - 1;
    if (editor.cx >= win_main.width) editor.cx = win_main.width - 1;
}

void editor_init()
{
    editor = (Editor){0};

    get_screen_size();
    editor.current_quit_times = config.quit_times;
    editor.N = N_DEFAULT;
    commands_history.index = -1;

    signal(SIGWINCH, handle_sigwinch);
}

bool open_file(char *filename)
{
    if (editor.filename) free(editor.filename);
    da_clear(&editor.rows);

    if (filename == NULL) {
        editor.filename = strdup("new");
        return true;
    }

    if (strchr(filename, '/')) {
        char *tmp = filename + strlen(filename) - 1;
        while (*tmp != '/') tmp--;
        filename = tmp+1;
    }
    editor.filename = strdup(filename);

    FILE *file = fopen(filename, "r");
    if (file == NULL) return false;

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
    editor.cx = 0;
    return true;
}

void scroll_up() // TODO: cy va oltre
{ 
    if (editor.offset > N_OR_DEFAULT(0)) {
        editor.offset -= N_OR_DEFAULT(1);
        editor.cy += N_OR_DEFAULT(1);
    } else editor.offset = 0;
}

void scroll_down() // TODO: cy fa cose strane
{
    if (editor.offset < editor.rows.count-N_OR_DEFAULT(0)-1) {
        editor.offset += N_OR_DEFAULT(1);
        editor.cy -= N_OR_DEFAULT(1);
    } else editor.offset = editor.rows.count-1;
}

void move_page_up() // TODO: non funziona benissimo
{
    if (editor.page > N_OR_DEFAULT(0)) editor.page -= N_OR_DEFAULT(1);
    else editor.page = 0;
    editor.offset = editor.page*win_main.height + editor.cy;
}             

void move_page_down() // TODO: non funziona benissimo
{
    if (editor.page < N_PAGES-N_OR_DEFAULT(0)-1) editor.page += N_OR_DEFAULT(1);
    else editor.page = N_PAGES-1;
    editor.offset = editor.page*win_main.height + editor.cy;
}           

static inline void move_cursor_begin_of_screen() { editor.cy = 0; }

static inline void move_cursor_end_of_screen() { editor.cy = win_main.height-1; }

void move_cursor_begin_of_file()
{
    editor.page = 0;
    editor.offset = 0;
    editor.cy = 0;
}     

void move_cursor_end_of_file() 
{
    editor.page = N_PAGES-1;
    editor.offset = editor.rows.count-win_main.height;
    editor.cy = win_main.height-1;
}

static inline void move_cursor_begin_of_line() { editor.cx = 0; }
static inline void move_cursor_end_of_line() { editor.cx = win_main.width-1; }

void move_cursor_first_non_space()
{
    size_t count = CURRENT_ROW->content.count;
    if (count == 0) return;
    editor.cx = 0;
    while (editor.cx < count && isspace(CURRENT_CHAR))
        editor.cx++;
}

void move_cursor_last_non_space()
{
    size_t count = CURRENT_ROW->content.count;
    if (count == 0) {
        editor.cx = 0;
        return;
    }
    editor.cx = count - 1;
    while (editor.cx > 0 && isspace(CURRENT_CHAR))
        editor.cx--;
    if (editor.cx != win_main.width-1) editor.cx++;
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

    if (cmd->type >= 0 && cmd->type < BUILTIN_CMDS_COUNT) {
        CommandArgs final_args = {0}; // TODO: maybe it can be just an array
        for (size_t i = 0; i < cmd->baked_args.count; i++) {
            log_this("baked argument %zu", i);
            CommandArg arg = cmd->baked_args.items[i];
            if (arg.type == PISQUY_ARG_PLACEHOLDER) {
                if (runtime_args && arg.placeholder_index < runtime_args->count) {
                    da_push(&final_args, runtime_args->items[arg.placeholder_index]);
                } else {
                    enqueue_message("ERROR: Missing argument $%zu for %s", arg.index, cmd->name);
                    return; 
                }
            } else da_push(&final_args, arg);
        }
        if (cmd->baked_args.count == 0 && runtime_args)
            da_push_many(&final_args, cmd->baked_args.items, cmd->baked_args.count);
        log_this("execute %p", cmd->execute);
        assert(cmd->execute);
        for (size_t i = 0; i < cmd->n; i++)
            cmd->execute(cmd, &final_args);
        if (final_args.count > 0) free(final_args.items);
    } else if (cmd->type == COMMAND_FROM_LINE || cmd->type >= USER_DEFINED) {
        da_foreach(cmd->subcmds, Command, subcmd)
            execute_command(subcmd, runtime_args);
    } else if (cmd->type == UNKNOWN) {
        enqueue_message("Unknown command `%s`", cmd->name);
    } else {
        print_error_and_exit("Unreachable command `%s` (%u) in execute_command\n",
                cmd->name ? cmd->name : "", cmd->type);
    }
}

void insert_newline_and_keep_pos()
{
    enqueue_message("TODO: insert_newline_and_keep_pos");
}

void delete_char_at(Row *row, size_t at)
{
    if (row->content.count <= at) return;
    s_remove(&row->content, at);
}

void delete_char()
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
        if (editor.cy == 0) editor.offset--;
        else editor.cy--;
        editor.cx = x;
        if (editor.cx >= win_main.width) {
            int shift = (win_main.width-editor.cx)+1;
            editor.cx -= shift;
        }
    } else {
        delete_char_at(row, x-1);
        if (editor.cx > 0) editor.cx--;
    }
    editor.dirty++;
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
    editor.cx = x;
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

        case ALT_k: N_TIMES move_cursor_up();    break;
        case ALT_j: N_TIMES move_cursor_down();  break;
        case ALT_h: N_TIMES move_cursor_left();  break;
        case ALT_l: N_TIMES move_cursor_right(); break;

        case ALT_K: move_cursor_begin_of_screen(); break;
        case ALT_J: move_cursor_end_of_screen();   break;
        case ALT_H: move_cursor_first_non_space(); break;
        case ALT_L: move_cursor_last_non_space();  break;

        case ALT_M: next_message(); break;

        case CTRL_P: commands_history_previous(); break;
        case CTRL_N: commands_history_next();     break;

        //case CTRL_K: N_TIMES scroll_up();                          break;
        //case CTRL_J: N_TIMES scroll_down();                        break;
        //case CTRL_H: enqueue_message("TODO: CTRL-H"); break;
        //case CTRL_L: enqueue_message("TODO: CTRL-L"); break;

        //case ALT_k: N_TIMES move_page_up();                      break; 
        //case ALT_j: N_TIMES move_page_down();                    break;
        //case ALT_h: enqueue_message("TODO: ALT-h"); break;
        //case ALT_l: enqueue_message("TODO: ALT-l"); break;
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
                enqueue_message("TODO: insert TAB");
                //insert_char('\t');
                N_TIMES {
                    // TODO: tab to spaces + numero di spazi (config)
                    for (int i = 0; i < 4; i++) insert_char(' ');
                }
            }
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

        //case PAGE_UP:
        //case PAGE_DOWN:
        //    if (c == PAGE_UP && E.cy != 0)
        //        E.cy = 0;
        //    else if (c == PAGE_DOWN && E.cy != E.screen_rows-1)
        //        E.cy = E.screen_rows-1;
        //    {
        //    int times = E.screen_rows;
        //    while(times--)
        //        editorMoveCursor(c == PAGE_UP ? ARROW_UP:
        //                                        ARROW_DOWN);
        //    }
        //    break;

        case ESC:
            if (editor.in_cmd) {
                editor.in_cmd = false;
                editor.cmd_pos = 0;
                // TODO: maybe save now the command to the history
                s_clear(&editor.cmd);
            }
            break;

        default:
            if (isprint(key)) N_TIMES insert_char(key);
            break;
    }
    if (!has_inserted_number) editor.N = N_DEFAULT;
    editor.current_quit_times = config.quit_times;
}

int main(int argc, char **argv)
{
    if (argc <= 0 || argc >= 3) {
        printw("TODO: usage\n");
        return 1;
    }

    char *filename = argc == 2 ? argv[1] : NULL;

    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");

    ncurses_init();
    load_config();
    editor_init();
    create_windows();

    if (!open_file(filename)) {
        if (filename) print_error_and_exit("Could not open file `%s`. %s.\n", filename, errno ? strerror(errno) : "");
        else          print_error_and_exit("Could not open new file. %s.\n", errno ? strerror(errno) : "");
    }

    while (true) {
        process_pressed_key();
        update_windows();
        update_cursor();
        doupdate();
    }

    return 0;
}
