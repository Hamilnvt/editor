// Reference: https://viewsourcecode.org/snaptoken/kilo/index.html

/* TODO:
    - commands_history_cap (config?)
    - capire come leggere ctrl-shift-x
    - comando per spostarsi ad una riga precisa
    - comando che stampa l'elenco dei comandi (builtin + user defined)
    - config:
      > aggiungere una descrizione ai campi da mostrare in caso di errore/assenza del campo
        - si genera nel config file di default e poi si mostra lo stesso errore, quindi deve stare da qualche parte
      > sintassi dei comandi user defined
        - serve un terminatore del comando? (ad esempio ';')
        - forse e' meglio che gli argomenti vengano passati tra parentesi
            ad esempio => #cmd: c1(arg1, arg2) c2 c3(nome="palle sudate")
        - gestione degli argomenti dei comandi definiti dall'utente, magari $0 oppure {0}
            > esempio 1. #cmd: ciao sono {} il {} => `cmd(tanica,tastiere)`
            > esempio 2. #cmd: daje({0}) => `cmd(roma)`
            > esempio 3. #cmd: ciao({nome}) come stai => `cmd(mario)` oppure `cmd(nome=mario)`
            > esempio 4. #cmd: ciao({nome=Tanica}) come stai => `cmd` == `cmd.Tanica`
      > possibilita' di assegnare comandi a combinazioni di tasti (questo sembra essere molto difficile, non ho idea di come si possa fare)
    - autocompletion dei comandi, con TAB vai all'argomento successivo
    - sistema di registrazione macro (comandi temporanei a cui magari si puo' dare un nome e le si esegue come comandi o con shortcut)
        > possibilita' di salvarli come comandi aggiungendoli direttamente al config
    - funzioni separate per la modifica della command line (altrimenti non si capisce niente)
      > cambiare anche process_pressed_key in modo da prendere solo gli input che valgono anche per il comando (altrimenti dovrei controllare in ogni funzione se si e' in_cmd e poi fare return
    - gestire con max_int o come si chiama il limite di editor.N
    - undo: undo dei comandi o versioni diverse delle variabili globali? Probabilmente la prima
    - emacs snippet thing (non ricordo il nome del package)
        > ifTAB => if (COND) {\n BODY \n}
        > con TAB ulteriori vai avanti nei vari blocchi
        > sintassi?
    - comando set(config_var, value)
    - forse per i comandi non servono i da: una volta che so la dimensione alloco la dimensione giusta e salvo il numero di argomenti e subcmds all'interno del comando
    - array di Window, si itera su questo array e chiama una funzione di callback (campo di Window)

*/

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
    int dirty; // TODO: quando si esegue un'azione che modifica lo editor si fa una copia (o comunque si copia parte dell'editor) che si inserisce in una struttura dati che permette di ciclare sulle varie 'versioni' dell'editor. dirty tiene conto di quale editor si sta guardando e quante modifiche sono state (fatte ciao miao gattin batifl)

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
#define N_PAGES (editor.rows.count/editor.screen_rows + 1)

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
    CTRL_N    = 14,
    CTRL_P    = 16,
    CTRL_Q    = 17,
    CTRL_S    = 19,
    ESC       = 27,
    BACKSPACE = 127,

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
    ALT_K,
    ALT_j,
    ALT_J,
    ALT_h,
    ALT_H,
    ALT_l,
    ALT_L,
    ALT_M,

    ALT_BACKSPACE,
    ALT_COLON,
} Key;

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
    vsnprintf(buf, editor.screen_cols, fmt, ap); // TODO: si puo' anche togliere la limitazione di 1024,
                                                 // tanto ora posso creare una finestra grande a piacere
                                                 // (ovviamente non oltre le dimensioni del terminale, per ora)
    da_push(&editor.messages, strdup(buf)); // NOTE: rember to free
    va_end(ap);
}

char *next_message()
{
    if (da_is_empty(&editor.messages)) return NULL;
    char *msg = editor.messages.items[0];
    da_remove_first(&editor.messages);
    free(msg);
    editor.current_msg_time = time(NULL);
    return da_is_empty(&editor.messages) ? NULL : editor.messages.items[0];
}

/// BEGIN Commands

typedef enum
{
    BUILTIN_SAVE,
    BUILTIN_QUIT,
    BUILTIN_SAVE_AND_QUIT,
    BUILTIN_FORCE_QUIT,
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
    CLI,
    USER_DEFINED,
} CommandType;

static_assert(BUILTIN_CMDS_COUNT == 13, "Associate a name to all builtin commands");
/* NOTE: name of the builtin commands */
#define SAVE              "s"
#define QUIT              "q"
#define SAVE_AND_QUIT     "sq"
#define FORCE_QUIT        "fq"
#define MOVE_CURSOR_UP    "u"
#define MOVE_CURSOR_DOWN  "d"
#define MOVE_CURSOR_LEFT  "l"
#define MOVE_CURSOR_RIGHT "r"
#define MOVE_LINE_UP      "lnu"
#define MOVE_LINE_DOWN    "lnd"
#define INSERT            "i"
#define DATE              "date"
#define GOTO_LINE         "goto"

typedef enum
{
    ARG_UINT,
    ARG_STRING
} CommandArgType;

// TODO: aggiungere name in modo da poterli chiamare con quel nome
typedef struct
{
    void *value;
    CommandArgType type;
    bool needed;
} CommandArg;

typedef struct
{
    CommandArg *items;
    size_t count;
    size_t capacity;
} CommandArgs;

typedef struct Command // TODO: forse cosi'
{
    char *name;
    CommandType type;
    CommandArgs args;
    struct Commands {
        struct Command *items;
        size_t count;
        size_t capacity;
    } subcmds;
    size_t n;
} Command;
typedef struct Commands Commands;

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
    log_this("Adding command in history: `%s`", cmd);
    da_push(&commands_history, strdup(cmd));
    commands_history.index = -1;
}

// TODO: se stai scrivendo un comando e fai previous ti salva quello che stavi scrivendo
// - magari solo come temporaneo, senza effettivamente aggiungerlo alla storia
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

bool expect_n_arguments(const Command *cmd, size_t n)
{
    if (cmd->args.count == n) return true;

    enqueue_message("ERROR: command `%s` expects %zu argument%s, but got %zu", cmd->name, n, n == 1 ? "" : "s", cmd->args.count);
    // TODO: mostrare anche quali sono i tipi e i nomi degli argomenti
    return false;
}

CommandType parse_cmdtype(char *type)
{
    static_assert(BUILTIN_CMDS_COUNT == 13, "Parse all commands in parse_cmd");
    if      (streq(type, SAVE))              return BUILTIN_SAVE;
    else if (streq(type, QUIT))              return BUILTIN_QUIT;
    else if (streq(type, SAVE_AND_QUIT))     return BUILTIN_SAVE_AND_QUIT;
    else if (streq(type, FORCE_QUIT))        return BUILTIN_FORCE_QUIT;
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

void add_builtin_command(char *name, CommandType type, CommandArgs args)
{
    Command cmd = {
        .name = name,
        .type = type,
        .args = args,
    };
    da_push(&commands, cmd);
}

void add_user_command(char *name, CommandArgs args, Commands subcmds)
{
    Command cmd = {
        .name = strdup(name),
        .type = USER_DEFINED + commands.count - BUILTIN_CMDS_COUNT,
        .args = args,
        .subcmds = subcmds,
        .n = 1
    };
    da_push(&commands, cmd);
}

int get_command_index(const Command *cmd)
{
    if (cmd->type >= USER_DEFINED) return cmd->type - USER_DEFINED + BUILTIN_CMDS_COUNT;
    else if (cmd->type >= 0 && cmd->type < BUILTIN_CMDS_COUNT) return cmd->type;
    else return -1;
}

//void free_commands(Commands *cmds)
//{
//    for (size_t i = 0; i < cmds->count; i++) {
//        Command *cmd = &cmds->items[i];
//        for (size_t j = 0; j < cmd->args.count; j++) {
//            free(cmd->args.items[j]);
//        }
//        if (cmd->args.count > 0) da_free(&cmd->args);
//    }
//    da_free(cmds);
//}

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
    size_t row;
    size_t col;
} Location;

//TODO:
// - dovrei probabilmente non usare strtok e fare tutto a mano
//   > funzioni come trim (left/right) sarebbero utili anche altrove tanto e poi ho String che e' molto utile
//   > questo mi aiuterebbe a parsare anche le nuove cose che voglio implementare (argomenti numerati e nominali)
// - track location
// - si puo' fare che se c'e' un errore continua a parsare e lo segnala solo (ritorna un da di errori)
char *parse_cmds(char *cmds_str, Commands *cmds, CommandArgs *cmd_args, Location *loc)
{
    *cmds = (Commands){0};
    char *saveptr_cmds;
    char *cmd_str = strtok_r(cmds_str, " \t", &saveptr_cmds);
    while (cmd_str != NULL) {
        Command cmd = {0};

        if (isdigit(*cmd_str)) {
            char *end_of_n;
            errno = 0;
            long n = strtol(cmd_str, &end_of_n, 10);
            if (loc) loc->col += end_of_n - cmd_str;
            if (n <= 0) {
                char *error = malloc(sizeof(char)*256);
                if (sprintf(error, "command multiplicity must be greater than 0, but got `%ld`", n) == -1) {
                    print_error_and_exit("Could not allocate memory, buy more RAM");
                } else return error;
            } else cmd.n = n;
            cmd_str = end_of_n;
        } else cmd.n = 1;

        Strings args = {0};
        *cmd_args = (CommandArgs){0};
        char *args_str = strchr(cmd_str, '.');
        if (args_str != NULL) {
            *args_str = '\0';
            args_str++;
            if (!strchr(args_str, ',')) {
                da_push(&args, strdup(args_str));
            } else {
                char *saveptr_args;
                char *arg = strtok_r(args_str, ",", &saveptr_args);
                while (arg != NULL) { // TODO: qui dovrei riportare errori del tipo: "cmd.arg1,"
                                      // - si presuppone che ci sia un altro argomento che in realta' non c'e'
                                      // - forse non posso usare strtok
                    da_push(&args, strdup(arg));
                    arg = strtok_r(NULL, ",", &saveptr_args);
                }
            }
        }
        cmd.type = parse_cmdtype(cmd_str);
        if (cmd.type == UNKNOWN) {
            char *error = malloc(sizeof(char)*256);
            // TODO: track location
            if (sprintf(error, "unknown command `%s`", cmd_str) == -1) {
                print_error_and_exit("Could not allocate memory, buy more RAM");
            } else return error;
        }

        int index = get_command_index(&cmd);
        if (index == -1) {
            print_error_and_exit("TODO: invalid command\n");
        }
        const Command *ref_cmd = &commands.items[index];
        cmd.name = ref_cmd->name;
        if (cmd_args) {
            cmd.args = (CommandArgs){0};
            CommandArg *ref_arg;
            char *arg;
            for (size_t i = 0; i < args.count; i++) {
                ref_arg = &ref_cmd->args.items[i];
                arg = args.items[i];
                if (ref_arg->needed) {
                    CommandArg actual_arg = *ref_arg;
                    switch (ref_arg->type)
                    {
                        case ARG_UINT: {
                            size_t value = atoi(arg);
                            if (value == 0 && !streq(arg, "0")) {
                                char *error = malloc(sizeof(char)*256);
                                // TODO: track location
                                if (sprintf(error, "expecting a number greater than 0, but got `%s`", arg) == -1) {
                                    print_error_and_exit("Could not allocate memory, buy more RAM");
                                } else return error;
                            }
                            actual_arg.value = malloc(sizeof(size_t)); // NOTE: rember to free
                            *(size_t *)actual_arg.value = value;
                        } break;
                        case ARG_STRING:
                            // TODO: allora, devo farlo a mano e devo prima cercare negli argomenti se ce ne sono ancora (caso "ciao,come,stai"), altrimenti continuo con la line (caso "ciao come stai"), poi salvo la fine in saveptr_cmds e da li' si continua con gli altri comandi
                            if (*arg == '\"') {
                                size_t arglen = strlen(arg);
                                if (arglen == 1 || arg[arglen-1] != '\"') {
                                    String str = {0};
                                    s_push_cstr(&str, arg);
                                    size_t j;
                                    bool done = false;
                                    for (j = i+1; j < args.count; j++) {
                                        if (strchr(args.items[j], '\"')) {
                                            char *popped_arg = args.items[j];
                                            da_remove(&args, j);
                                            s_push(&str, ',');
                                            s_push_cstr(&str, popped_arg);
                                            done = true;
                                            break;
                                        }
                                    }
                                    if (!done && j >= args.count) { // TODO: search after all the arguments char by char
                                        char *rest_of_str = saveptr_cmds;
                                        size_t len = strlen(rest_of_str);
                                        char *end = rest_of_str+len-1;
                                        while (isspace(*end)) end--;
                                        *(end+1) = '\0';
                                        size_t k = 0;
                                        while (k < len && *rest_of_str != '\"') {
                                            s_push(&str, *rest_of_str);
                                            rest_of_str++;
                                        }
                                        if (k >= len) {
                                            // TODO: track location
                                            return strdup("unclosed quoted string");
                                        } else s_push(&str, '\"');
                                        saveptr_cmds = rest_of_str+1;
                                    }
                                    s_push_null(&str);
                                    arg = str.items;
                                }
                                arg[strlen(arg)-1] = '\0';
                                arg++;
                            }
                            // TODO: add support for char '\n' in string arg
                            // TODO: add support for char '"' in string arg
                            actual_arg.value = strdup(arg); // NOTE: rember to free
                            break;
                        default:
                            print_error_and_exit("Unreachable command arg type %u in parse_cmds\n", ref_arg->type);
                    }
                    da_push(&cmd.args, actual_arg);
                } else da_push(&cmd.args, *ref_arg);
            }
        }

        da_push(cmds, cmd);
        cmd_str = strtok_r(NULL, " \t", &saveptr_cmds);
    }
    return NULL;
}

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

char *better_parse_cmds(char *cmds_str, Commands *cmds, CommandArgs *cmd_args, Location *loc)
{
    (void)cmd_args;

    if (loc) loc->col += trim_left(&cmds_str);
    log_this("- Parsing cmds from `%s`", cmds_str);
    *cmds = (Commands){0};
    size_t i = 0;
    size_t len = strlen(cmds_str);
    while (i < len && strlen(cmds_str) > 0) {
        Command cmd = {0};

        if (isdigit(*cmds_str)) {
            char *end_of_n;
            long n = strtol(cmds_str, &end_of_n, 10);
            if (n <= 0) {
                char *error = malloc(sizeof(char)*256);
                if (sprintf(error, "command multiplicity must be greater than 0, but got `%ld`", n) == -1) {
                    print_error_and_exit("Could not allocate memory, buy more RAM");
                } else return error;
            } else cmd.n = n;
            if (loc) loc->col += end_of_n - cmds_str;
            i += end_of_n - cmds_str;
            cmds_str = end_of_n;
        } else cmd.n = 1;
        log_this("multiplicity: %zu", cmd.n);

        char *end_of_cmd_name = strpbrk(cmds_str, " \t(");
        bool has_args = false;
        if (end_of_cmd_name != NULL) {
            if (*end_of_cmd_name == '(') has_args = true;
            *end_of_cmd_name = '\0';
        }
        cmd.type = parse_cmdtype(cmds_str);
        if (cmd.type == UNKNOWN) {
            char *error = malloc(sizeof(char)*256);
            if (sprintf(error, "unknown command `%s`", cmds_str) == -1) {
                print_error_and_exit("Could not allocate memory, buy more RAM");
            } else return error;
        } else log_this("name: `%s` => type: %d", cmds_str, cmd.type);

        int index = get_command_index(&cmd);
        if (index == -1) {
            print_error_and_exit("FATAL: unreachable command `%d`\n", cmd.type);
        }
        const Command *ref_cmd = &commands.items[index];
        cmd.name = ref_cmd->name;
        if (has_args && ref_cmd->args.count == 0) {
            char *error = malloc(sizeof(char)*256);
            if (sprintf(error, "command `%s` does not require arguments", cmd.name) == -1) {
                print_error_and_exit("Could not allocate memory, buy more RAM");
            } else return error;
        }
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

            log_this("  > Parsing arguments `%s` (%zu)", cmds_str, argslen);
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
                        if (sprintf(error, "Argument %zu not provided", args_count+1) == -1) {
                            print_error_and_exit("Could not allocate memory, buy more RAM");
                        } else return error;
                    }
                    log_this("    - TODO: parse argument `%.*s`", arglen, cmds_str);
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

        da_push(cmds, cmd);
        if (loc) loc->col += trim_left(&cmds_str);
    }

    return NULL;
}
/// END Commands

/// BEGIN Config

typedef enum
{
    FIELD_BOOL,
    FIELD_UINT,
    FIELD_STRING,
    FIELD_LIMITED_STRING,
    //FIELD_LIMITED_UINT,
    //FIELD_RANGE_UINT,
} FieldType;

char *fieldtype_to_string(FieldType type)
{
    switch (type)
    {
        case FIELD_BOOL:           return "bool";
        case FIELD_UINT:           return "uint";
        case FIELD_STRING:         return "string";
        case FIELD_LIMITED_STRING: return "limited string";
        default:
            print_error_and_exit("Unreachable\n");
    }
}

typedef struct
{
    const char *name;
    FieldType type;
    const void *ptr;
    const char **valid_values;
    const void *thefault;
} ConfigField;

typedef struct
{
    ConfigField *items;
    size_t count;
    size_t capacity;
} ConfigFields;

ConfigField create_field(const char *name, FieldType type, void *ptr, const char **valid_values, const void *thefault)
{
    return (ConfigField){
        .name=name,
        .type=type,
        .ptr=ptr,
        .valid_values = valid_values,
        .thefault=thefault
    };
}

#define ADD_CONFIG_FIELD(name, type, valid_values)                                \
    do {                                                                          \
        ConfigField __field = create_field(#name, (type),                         \
                (void *)&config.name, (valid_values), (void *)&default_ ## name); \
        da_push(&remaining_fields, __field);                                      \
    } while (0);

typedef enum { LN_NO, LN_ABS, LN_REL } ConfigLineNumbers;

typedef struct
{
    size_t quit_times;
    time_t msg_lifetime;
    ConfigLineNumbers line_numbers;
} Config;
Config config = {0};

int read_key(); // Forward declaration

void load_config()
{
    char *home = getenv("HOME");
    if (home == NULL) {
        print_error_and_exit("Env variable HOME not set\n");
    }

    const size_t default_quit_times = 3;
    const size_t default_msg_lifetime = 3;
    const char *default_line_numbers = "no";

    const char *valid_values_field_bool[] = {"true", "false", NULL};
    const char *valid_values_line_numbers[] = {"no", "absolute", "relative", NULL};

    String config_log = {0};

    const char *config_path = ".config/editor/config";
    char full_config_path[256] = {0};
    sprintf(full_config_path, "%s/%s", home, config_path);
    FILE *config_file = fopen(full_config_path, "r");
    if (config_file == NULL) {
        s_push_fstr(&config_log, "WARNING: config file not found at %s\n", full_config_path);
        config_file = fopen(full_config_path, "w+");
        if (config_file == NULL) {
            s_push_fstr(&config_log, "ERROR: could not create config file\n");
            s_print(config_log);
            exit(1);
        }

        fprintf(config_file, "quit_times: %zu\t// times you need to press CTRL-q before exiting without saving\n",
                default_quit_times); // TODO: make the description a macro/const
        fprintf(config_file, "msg_lifetime: %zu\t// time in seconds of the duration of a message\n",
                default_msg_lifetime);
        fprintf(config_file, "line_numbers: %s\t// display line numbers at_all, absolute or relative to the current line\n",
                default_line_numbers);

        s_push_fstr(&config_log, "NOTE: default config file has been created\n\n");
        rewind(config_file);
    }

    ConfigFields remaining_fields = {0};

    ADD_CONFIG_FIELD(quit_times,   FIELD_UINT, NULL);
    ADD_CONFIG_FIELD(msg_lifetime, FIELD_UINT, NULL);
    ADD_CONFIG_FIELD(line_numbers, FIELD_LIMITED_STRING, valid_values_line_numbers);
    //if (DEBUG) {
    //    for (size_t i = 0; i < remaining_fields.count; i++) {
    //        const ConfigField *field = &remaining_fields.items[i];
    //        printf("Field { name: `%s`, type: `%s` }\n", field->name, fieldtype_to_string(field->type));
    //    }
    //}

    ConfigFields inserted_fields = {0};

    // commands initialization
    commands = (Commands){0};
    static_assert(BUILTIN_CMDS_COUNT == 13, "Add all builtin commands in commands");
    add_builtin_command(SAVE,              BUILTIN_SAVE,              (CommandArgs){0}); // TODO: argomento per salvare il file con un nome
    add_builtin_command(QUIT,              BUILTIN_QUIT,              (CommandArgs){0});
    add_builtin_command(SAVE_AND_QUIT,     BUILTIN_SAVE_AND_QUIT,     (CommandArgs){0}); // TODO: argomento per salvare il file con un nome
    add_builtin_command(FORCE_QUIT,        BUILTIN_FORCE_QUIT,        (CommandArgs){0});
    add_builtin_command(MOVE_CURSOR_UP,    BUILTIN_MOVE_CURSOR_UP,    (CommandArgs){0});
    add_builtin_command(MOVE_CURSOR_DOWN,  BUILTIN_MOVE_CURSOR_DOWN,  (CommandArgs){0});
    add_builtin_command(MOVE_CURSOR_LEFT,  BUILTIN_MOVE_CURSOR_LEFT,  (CommandArgs){0});
    add_builtin_command(MOVE_CURSOR_RIGHT, BUILTIN_MOVE_CURSOR_RIGHT, (CommandArgs){0});
    add_builtin_command(MOVE_LINE_UP,      BUILTIN_MOVE_LINE_UP,      (CommandArgs){0});
    add_builtin_command(MOVE_LINE_DOWN,    BUILTIN_MOVE_LINE_DOWN,    (CommandArgs){0});

    CommandArgs cmd_args = {0};
    CommandArg cmd_arg;

    cmd_arg = (CommandArg){.value = NULL, .type=ARG_STRING, .needed=true};
    da_push(&cmd_args, cmd_arg);
    add_builtin_command(INSERT, BUILTIN_INSERT, cmd_args);
    da_clear(&cmd_args);

    add_builtin_command(DATE, BUILTIN_DATE, (CommandArgs){0});

    cmd_arg = (CommandArg){.value = NULL, .type=ARG_UINT, .needed=true};
    da_push(&cmd_args, cmd_arg);
    add_builtin_command(GOTO_LINE, BUILTIN_GOTO_LINE, cmd_args);
    da_clear(&cmd_args);

    Location loc = {0};
    ssize_t res; 
    size_t len;
    char *full_line = NULL;
    while ((res = getline(&full_line, &len, config_file)) != -1) {
        res--;
        if (res == 0) {
            loc.row++;
            loc.col = 0;
            continue;
        }
        char *line = full_line;
        line[res] = '\0';
        loc.col += trim_left(&line);
        if (res == 0) {
            loc.row++;
            loc.col = 0;
            continue;
        }

        char *comment = strstr(line, "//");
        if (comment != NULL) {
            if (comment == line) {
                loc.row++;
                loc.col = 0;
                continue;
            } else *comment = '\0';
        }

        char *colon = NULL;
        if (*line == '#') {
            line++;
            loc.col++;
            colon = strchr(line, ':');
            if (colon == NULL) {
                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                s_push_fstr(&config_log, "ERROR: invalid command, it should be of the form #name: commands...\n");
            } else {
                *colon = '\0';
                char *cmd_name = line;
                bool already_defined = false;
                for (size_t i = 0; i < commands.count; i++) {
                    if (streq(cmd_name, commands.items[i].name)) {
                        s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                        s_push_fstr(&config_log, "ERROR: redeclaration of %s command `%s`\n", 
                                i < BUILTIN_CMDS_COUNT ? "builtin" : "user defined", cmd_name); 
                        already_defined = true;
                        break;
                    }
                }
                if (already_defined) {
                    loc.col = 0;
                    loc.row++;
                    continue;
                }
                loc.col += strlen(cmd_name) + 1;

                char *cmds = colon+1;
                Commands cmd_def = {0};
                CommandArgs cmd_args = {0};
                // TODO: WIP
                //char *parse_error = parse_cmds(cmds, &cmd_def, &cmd_args, &loc);
                char *parse_error = better_parse_cmds(cmds, &cmd_def, &cmd_args, &loc);
                if (parse_error != NULL) {
                    s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                    s_push_fstr(&config_log, "ERROR: %s\n", parse_error);
                    free(parse_error);
                } else {
                    add_user_command(cmd_name, cmd_args, cmd_def);
                    s_push_fstr(&config_log, "added command `%s`\n", cmd_name);
                }
            }
        } else if ((colon = strchr(line, ':')) != NULL) {
            *colon = '\0';
            char *field_name = line;
            char *field_value = colon+1;
            while(isspace(*field_value)) {
                loc.col++;
                field_value++;
            }

            size_t i = 0;
            bool found = false;
            while (!found && i < remaining_fields.count) {
                if (streq(field_name, remaining_fields.items[i].name)) {
                    ConfigField removed_field = remaining_fields.items[i];
                    da_remove(&remaining_fields, i);
                    da_push(&inserted_fields, removed_field);
                    switch (removed_field.type)
                    {
                        case FIELD_BOOL: {
                            bool value = *(bool *)removed_field.thefault;
                            if (streq(field_value, "true")) value = true;
                            else if (streq(field_value, "false")) value = false;
                            else {
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "ERROR: field `%s` expects a boolean (`%s` or `%s`), but got `%s`\n", field_name, valid_values_field_bool[0], valid_values_field_bool[1], field_value);
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "NOTE: defaulted to `%s`\n", *(bool *)removed_field.thefault ? "true" : "false");
                            }
                            *((bool *)removed_field.ptr) = value;
                        } break;

                        case FIELD_UINT: {
                            size_t value = atoi(field_value);
                            if (value == 0) {
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "ERROR: field `%s` expects an integer greater than 0, but got `%s`\n", field_name, field_value);
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "NOTE: defaulted to `%zu`\n", *((size_t *)removed_field.thefault));
                                *((size_t *)removed_field.ptr) = *((size_t *)removed_field.thefault);
                            } else {
                                *((size_t *)removed_field.ptr) = value;
                                //printf("CONFIG: field `%s` set to `%zu`\n", field_name, value);
                            }
                        } break;

                        case FIELD_STRING: {
                            char *value = field_value;
                            if (strlen(value) == 0) {
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "ERROR: field `%s` expects a non empty string\n", field_name);
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "NOTE: defaulted to `%s`\n", *(char **)removed_field.thefault);
                                removed_field.ptr = strdup(*(char **)removed_field.thefault);
                            } else removed_field.ptr = strdup(*(char **)value);
                        } break;

                        case FIELD_LIMITED_STRING: {
                            size_t arrlen = 0;
                            for (; removed_field.valid_values[arrlen]; arrlen++)
                                ;
                            char *value = field_value;
                            bool is_str_empty = strlen(value) == 0;
                            bool is_valid = false;
                            size_t i = 0;
                            if (!is_str_empty) {
                                while (!is_valid && i < arrlen) {
                                    if (streq(removed_field.valid_values[i], value)) is_valid = true;
                                    else i++;
                                }
                            }
                            if (!is_valid || is_str_empty) {
                                is_valid = false;
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "ERROR: field `%s` expects one of the following values: ", field_name);
                                for (size_t j = 0; j < arrlen; j++) {
                                    s_push_fstr(&config_log, "`%s`", removed_field.valid_values[j]);
                                    if (arrlen > 2 && j < arrlen-2) s_push_cstr(&config_log, ", ");
                                    else if (j == arrlen-2) s_push_cstr(&config_log, " or ");
                                }
                                s_push_fstr(&config_log, ", but got ");
                                if (is_str_empty) s_push_fstr(&config_log, "an empty string ");
                                else s_push_fstr(&config_log, "`%s` ", value);
                                s_push_fstr(&config_log, "instead.\n");
                                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                                s_push_fstr(&config_log, "NOTE: defaulted to `%s`\n", *(char **)removed_field.thefault);

                                value = *(char **)removed_field.thefault;
                            }
                            if (!is_valid) i = 0;
                            if (streq(field_name, "line_numbers")) {
                                *((ConfigLineNumbers *)removed_field.ptr) = i;
                            } else { // NOTE: this is useful in case I forget to implement one
                                print_error_and_exit("Unreachable field name `%s` in load_config\n", field_name); 
                            }
                        } break;
                        default:
                            s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                            print_error_and_exit("Unreachable field type in load_config\n");
                    }
                    found = true;
                } else i++;
            }

            if (!found) {
                found = false;
                i = 0;
                while (!found && i < inserted_fields.count) {
                    if (streq(field_name, inserted_fields.items[i].name)) {
                        s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                        s_push_fstr(&config_log, "ERROR: redeclaration of field `%s`\n", field_name);
                        found = true;
                    } else i++;
                }
            }

            if (!found) {
                s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
                s_push_fstr(&config_log, "ERROR: unknown field `%s`\n", field_name);
            }
        } else {
            s_push_fstr(&config_log, "%s:%zu:%zu: ", full_config_path, loc.row+1, loc.col+1);
            s_push_fstr(&config_log, "ERROR: I don't know what to do with `%s`\n", line);
        }

        loc.row++;
        loc.col = 0;
    }
    free(full_line);

    if (remaining_fields.count > 0) {
        s_push_cstr(&config_log, "\nWARNING: the following fields have not been set:\n");
        for (size_t i = 0; i < remaining_fields.count; i++) {
            const ConfigField *field = &remaining_fields.items[i];
            s_push_fstr(&config_log, " -> %s (type: %s, default: ", field->name, fieldtype_to_string(field->type));
            switch (field->type)
            {
                case FIELD_BOOL:
                    s_push_fstr(&config_log, "`%s`)\n", *(bool *)field->thefault ? "true" : "false");
                    *((bool *)field->ptr) = *((bool *)field->thefault);
                    break;
                case FIELD_UINT:
                    s_push_fstr(&config_log, "`%zu`)\n", *(size_t *)field->thefault);
                    *((size_t *)field->ptr) = *((size_t *)field->thefault);
                    break;
                case FIELD_STRING:
                    s_push_fstr(&config_log, "`%s`)\n", (char *)field->thefault);
                    *((char **)field->ptr) = strdup(field->thefault);
                    break;
                case FIELD_LIMITED_STRING:
                    s_push_fstr(&config_log, "`%s`)\n", (char *)field->thefault);
                    *((char **)field->ptr) = strdup(field->thefault);
                    // TODO: magari elencare anche qua i possibili valori
                    break;
                default:
                    print_error_and_exit("Unreachable field type in load_config\n");
            }
        }
    }

    // TODO: magari se si lancia l'editor con -v stampa anche queste cose
    //s_push_fstr(&config_log, "\nDefined commands:\n");
    //for (size_t i = 0; i < commands.count; i++) {
    //    s_push_fstr(&config_log, "%s: `%s`\n", i < BUILTIN_CMDS_COUNT ? "Builtin" : "User", commands.items[i].name);
    //}

    if (!s_is_empty(config_log)) {
        s_push_null(&config_log);
        printw(config_log.items);
        s_free(&config_log);
        printw("\n");
        printw("- Press ENTER to continue ignoring errors and warnings\n");
        printw("- Press ESC to exit\n");
        int key;
        while (true) {
            key = getch();
            if (key == ENTER) break;
            else if (key == ESC) exit(1);
        }
    }
}

/// END Config 

/* Pairs */
#define DEFAULT_EDITOR_PAIR  0
#define DEFAULT_STATUS_PAIR  1
#define DEFAULT_MESSAGE_PAIR 2

/* Windows */
typedef struct
{
    WINDOW *win;
} Window;

static Window win_main = {0};
static Window win_line_numbers = {0};
static Window win_message = {0};
static Window win_command = {0};
static Window win_status = {0};

static inline void get_screen_size(void) { getmaxyx(stdscr, editor.screen_rows, editor.screen_cols); }

Window create_window(int h, int w, int y, int x, int pair)
{
    Window win = {0};
    win.win = newwin(h, w, y, x);
    wattron(win.win, COLOR_PAIR(pair));
    return win;
}

void create_windows(void)
{
    win_main = create_window(editor.screen_rows-1, editor.screen_cols-LINE_NUMBERS_SPACE, 0, LINE_NUMBERS_SPACE,
            DEFAULT_EDITOR_PAIR);
    win_line_numbers = create_window(editor.screen_rows-1, LINE_NUMBERS_SPACE, 0, 0, DEFAULT_EDITOR_PAIR);
    win_message = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_MESSAGE_PAIR);

    win_command = create_window(1, editor.screen_cols, editor.screen_rows-2, 0, DEFAULT_MESSAGE_PAIR);

    win_status = create_window(1, editor.screen_cols, editor.screen_rows-1, 0, DEFAULT_EDITOR_PAIR);
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

    if (has_colors()) {
        start_color();

        init_color(ED_COLOR_DEFAULT_BACKGROUND, 18, 18, 18);
        init_color(ED_COLOR_DEFAULT_FOREGROUND, 5, 20, 5);
        init_color(ED_COLOR_YELLOW, 178, 181, 0);

        init_pair(DEFAULT_EDITOR_PAIR, ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_DEFAULT_BACKGROUND);
        init_pair(DEFAULT_MESSAGE_PAIR, COLOR_WHITE, COLOR_RED);
        //init_pair(DEFAULT_STATUS_PAIR, ED_COLOR_DEFAULT_FOREGROUND, ED_COLOR_DEFAULT_BACKGROUND);

        use_default_colors();
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
    for (size_t i = editor.offset; i < editor.offset+editor.screen_rows; i++) {
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
    for (size_t i = 0; i < editor.screen_rows; i++) {
        wprintw(win_line_numbers.win, "%zu\n", i+1);
    }
}

void update_window_message(void)
{
    wprintw(win_message.win, "TODO: message\n");
}

void update_window_command(void)
{

}

void update_window_status(void)
{
    char perc_buf[5] = {0};
    if (editor.rows.count == 0) strcpy(perc_buf, "0%");
    else if (CURRENT_Y_POS == 0) strcpy(perc_buf, "Top");
    else if (CURRENT_Y_POS == editor.rows.count-1) strcpy(perc_buf, "Bot");
    else if (CURRENT_Y_POS > editor.rows.count-1)  strcpy(perc_buf, "Over");
    else sprintf(perc_buf, "%d%%", (int)(((float)(CURRENT_Y_POS)/(editor.rows.count))*100));

    // TODO: attention, it could overflow
    waddch(win_status.win, ' ');
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
    update_window(message);
    update_window(command);
    update_window(status);

    //Row *row;
    //for (size_t y = editor.offset; y < editor.offset+editor.screen_rows; y++) {
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

    if (editor.in_cmd) {
        cy = editor.screen_rows-1;
        cx = strlen("Command: ") + editor.cmd_pos;
    }

    wmove(win_main.win, cy, cx);
    wnoutrefresh(win_main.win);
}

void handle_sigwinch(int signo)
{
    (void)signo;
    get_screen_size();
    if (editor.cy > editor.screen_rows) editor.cy = editor.screen_rows - 1;
    if (editor.cx > editor.screen_cols) editor.cx = editor.screen_cols - 1;
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

void builtin_move_cursor_up() 
{
    if (editor.cy == 0 && editor.page != 0) {
        editor.offset--; // TODO: deve cambiare anche la pagina
    }
    if (editor.cy - N_OR_DEFAULT(1) > 0) editor.cy -= N_OR_DEFAULT(1);
    else editor.cy = 0;
    //if (editor.offset > N_OR_DEFAULT(0)) editor.offset -= N_OR_DEFAULT(1);
    //else editor.offset = 0;
}

void builtin_move_cursor_down()
{ 
    //if ((editor.offset+N_OR_DEFAULT(1) / N_PAGES) > editor.page) editor.page += (editor.offset+N_OR_DEFAULT(1)) / N_PAGES; // TODO: No, non funziona cosi' devo modificare l'offset in questo caso
    if (editor.cy == editor.screen_rows-1 && editor.page != N_PAGES-1) {
        editor.offset++; // TODO: deve cambiare anche la pagina
    }
    if (editor.cy+N_OR_DEFAULT(1) < editor.screen_rows-1) editor.cy += N_OR_DEFAULT(1);
    else editor.cy = editor.screen_rows-1;
    //if (editor.offset < editor.rows.count-N_OR_DEFAULT(0)-1) editor.offset += N_OR_DEFAULT(1); 
    //else editor.offset = editor.rows.count-1;
}

void builtin_move_cursor_left()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos > 0) editor.cmd_pos -= 1;
        else editor.cmd_pos = 0;
    } else {
        if (editor.cx > 0) editor.cx -= 1;
        else editor.cx = 0;
    }
}

void builtin_move_cursor_right()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos < editor.cmd.count-1) editor.cmd_pos += 1;
        else editor.cmd_pos = editor.cmd.count;
    } else {
        if (editor.cx < editor.screen_cols-1) editor.cx += 1;
        else editor.cx = editor.screen_cols-1;
    }
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
    editor.offset = editor.page*editor.screen_rows + editor.cy;
}             

void move_page_down() // TODO: non funziona benissimo
{
    if (editor.page < N_PAGES-N_OR_DEFAULT(0)-1) editor.page += N_OR_DEFAULT(1);
    else editor.page = N_PAGES-1;
    editor.offset = editor.page*editor.screen_rows + editor.cy;
}           

static inline void move_cursor_begin_of_screen() { editor.cy = 0; }

static inline void move_cursor_end_of_screen() { editor.cy = editor.screen_rows-1; }

void move_cursor_begin_of_file()
{
    editor.page = 0;
    editor.offset = 0;
    editor.cy = 0;
}     

void move_cursor_end_of_file() 
{
    editor.page = N_PAGES-1;
    editor.offset = editor.rows.count-editor.screen_rows;
    editor.cy = editor.screen_rows-1;
}

static inline void move_cursor_begin_of_line() { editor.cx = 0; }

void move_cursor_end_of_line()
{
    editor.cx = editor.screen_cols-1;
}

// TODO: ricontrolla
void move_cursor_first_non_space()
{
    size_t count = CURRENT_ROW->content.count;
    log_this("cy = %zu", CURRENT_Y_POS);
    log_this("`"S_FMT"` (%zu)\n", S_ARG(CURRENT_ROW->content), count);
    if (count == 0) return;
    editor.cx = 0;
    while (editor.cx < count && isspace(CURRENT_CHAR)) {
        editor.cx++;
    }
}

// TODO: ricontrolla
void move_cursor_last_non_space()
{
    Row *row = CURRENT_ROW;
    char *str = row->content.items;
    size_t count = row->content.count;
    if (count == 0) {
        editor.cx = 0;
        return;
    }
    size_t i = count - 1;
    while (i > 0 && isspace(str[i]))
        i--;
    editor.cx = (i == editor.screen_cols-1) ? i : i+1;
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

bool save(void)
{
    String save_buf = {0};
    Row *row;
    for (size_t i = 0; i < editor.rows.count; i++) {
        row = ROW(i);
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
    return 0;

writeerr:
    s_free(&save_buf);
    if (fd != -1) close(fd);
    enqueue_message("Can't save! I/O error: %s", strerror(errno));
    return 1;
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
    if (y == editor.screen_rows-1) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y+1];
    editor.rows.items[y+1] = tmp;
    editor.cy++;
    editor.dirty++;
}

void execute_cmd(const Command *cmd); // forward declaration

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

            Command cli_cmd = {
                .name = "cli command",
                .type = CLI,
                .n = 1 // TODO
            };
            Commands subcmds = {0};
            CommandArgs args = {0};
            char *parse_error = better_parse_cmds(cmd_str, &subcmds, &args, NULL);
            //char *parse_error = parse_cmds(cmd_str, &subcmds, &args, NULL);
            if (parse_error != NULL) {
                enqueue_message("ERROR: %s", parse_error);
                free(parse_error);
            } else {
                cli_cmd.subcmds = subcmds;
                cli_cmd.args = args;
                execute_cmd(&cli_cmd);
            }
            //free_commands(&cmds);
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
        if (editor.cy == editor.screen_rows-1) editor.offset++;
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

void builtin_insert(const Command *cmd)
{
    if (!expect_n_arguments(cmd, 1)) return;
    char *str = cmd->args.items[0].value;
    size_t len = strlen(str);
    for (size_t j = 0; j < len; j++) {
        insert_char(str[j]);
    }
}

void builtin_date(void)
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char date[64];
    assert(strftime(date, sizeof(date), "%c", tm));
    for (size_t i = 0; i < strlen(date); i++) {
        insert_char(date[i]);
    }
}

void builtin_goto_line(const Command *cmd)
{
    if (!expect_n_arguments(cmd, 1)) return;
    size_t line = *(size_t *)cmd->args.items[0].value;
    enqueue_message("TODO: goto line %zu", line);
}

void execute_cmd(const Command *cmd)
{
    static_assert(BUILTIN_CMDS_COUNT == 13, "Execute all commands in execute_cmd");
    for (size_t i = 0; i < cmd->n; i++ ) {
        switch (cmd->type)
        {
            case BUILTIN_SAVE             : save();                      break;
            case BUILTIN_QUIT             : if (can_quit()) quit();      break;
            case BUILTIN_SAVE_AND_QUIT    : save(); quit();              break;
            case BUILTIN_FORCE_QUIT       : quit();                      break;
            case BUILTIN_MOVE_CURSOR_UP   : builtin_move_cursor_up();    break;
            case BUILTIN_MOVE_CURSOR_DOWN : builtin_move_cursor_down();  break; 
            case BUILTIN_MOVE_CURSOR_LEFT : builtin_move_cursor_left();  break;
            case BUILTIN_MOVE_CURSOR_RIGHT: builtin_move_cursor_right(); break;
            case BUILTIN_MOVE_LINE_UP     : builtin_move_line_up();      break;
            case BUILTIN_MOVE_LINE_DOWN   : builtin_move_line_down();    break;
            case BUILTIN_INSERT           : builtin_insert(cmd);         break;
            case BUILTIN_DATE             : builtin_date();              break;
            case BUILTIN_GOTO_LINE        : builtin_goto_line(cmd);      break;
            case UNKNOWN: enqueue_message("Unknown command `%s`", cmd->name); break;
            case BUILTIN_CMDS_COUNT:
                print_error_and_exit("Unreachable command type `BUILTIN_CMDS_COUNT` in execute_cmd\n");
            case CLI:
                Command *subcmd = NULL;
                for (size_t i = 0; i < cmd->subcmds.count; i++) {
                    subcmd = &cmd->subcmds.items[i];
                    execute_cmd(subcmd);
                }
                break;
            case USER_DEFINED:
            default:
                if (cmd->type >= USER_DEFINED && cmd->type < USER_DEFINED + USER_CMDS_COUNT) {
                    Command *user_cmd = &commands.items[get_command_index(cmd)];
                    Command *subcmd = NULL;
                    for (size_t i = 0; i < user_cmd->subcmds.count; i++) {
                        subcmd = &user_cmd->subcmds.items[i];
                        execute_cmd(subcmd);
                    }
                    break;
                } else print_error_and_exit("Unreachable command type %u in execute_cmd\n", cmd->type);
        }
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
        if (editor.cx >= editor.screen_cols) {
            int shift = (editor.screen_cols-editor.cx)+1;
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
    log_this("new N: %d\n", editor.N);
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

        case ALT_k: N_TIMES builtin_move_cursor_up();    break;
        case ALT_j: N_TIMES builtin_move_cursor_down();  break;
        case ALT_h: N_TIMES builtin_move_cursor_left();  break;
        case ALT_l: N_TIMES builtin_move_cursor_right(); break;

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

        case ENTER: // TODO: se si e' in_cmd si esegue execute_cmd (che fa anche il resto)
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
