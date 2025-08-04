/* TODO:
    - needs_redraw technology (ma prima deve funzionare tutto il resto)
    - capire come leggere ctrl-shift-x
    - config:
      > aggiungere una descrizione ai campi da mostrare in caso di errore/assenza del campo
      > possibilita' di definire nel config file nuovi comandi come composizioni di quelli builtin (4 lu ld significa 4 volte lu poi ld, o pensare ad un'altra sintassi)
      > possibilita' di assegnare comandi a combinazioni di tasti (questo sembra essere molto difficile, non ho idea di come si possa fare)
      > fare i comandi per le azioni base (salva, esci, spostati, ecc...)
    - sistema di registrazione macro (a cui magari si puo' dare un nome e le si esegue come comandi o con shortcut)
    - funzioni separate per quando si scrive il comando (altrimenti non si capisce niente)
      > cambiare anche process_pressed_key in modo da prendere solo gli input che valgono anche per il comando (altrimenti dovrei controllare in ogni funzione se si e' in_cmd e poi fare return
    - gestire con max_int o come si chiama il limite di editor.N

    - ref: https://viewsourcecode.org/snaptoken/kilo/index.html
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

#define STRINGS_IMPLEMNTATION
#include "../libs/strings.h"

#define DEBUG true

bool streq(const char *s1, const char *s2) { return strcmp(s1, s2) == 0; }
bool strneq(const char *s1, const char *s2, size_t n) { return strncmp(s1, s2, n) == 0; }

typedef struct
{
    size_t i;
    String content;
} Row;

da_decl(Row, Rows);

typedef struct
{
    char *filename;
    Rows rows;
    int dirty; // TODO: quando si esegue un'azione che modifica lo editor si fa una copia (o comunque si copia parte dello editor) che si inserisce in una struttura dati che permette di ciclare sulle varie 'versioni' dello editor. dirty tiene conto di quale editor si sta guardando e quante modifiche sono state fatte
    bool rawmode;

    size_t cx;
    size_t cy;
    size_t rowoff;
    size_t coloff;
    size_t screenrows;
    size_t screencols;
    size_t page;

    String message;
    time_t msg_time;

    String cmd;
    size_t cmd_pos;
    bool in_cmd;

    size_t current_quit_times;

    int N; /* multiplicity for the command */
} Editor;

typedef struct
{
    size_t quit_times;
    time_t msg_lifetime;
    char *prova;
} Config;

const size_t default_quit_times = 3;
const size_t default_msg_lifetime = 3;
const char * default_prova = "prova";

#define N_DEFAULT -1 
#define N_OR_DEFAULT(n) ((size_t)(editor.N == N_DEFAULT ? (n) : editor.N))
#define N_TIMES for (size_t i = 0; i < N_OR_DEFAULT(1); i++)

#define CRNL "\r\n"
#define CURRENT_Y_POS (editor.rowoff+editor.cy) 
#define CURRENT_X_POS (editor.coloff+editor.cx) 
#define ROW(i) (&editor.rows.items[i])
#define CHAR(row, i) (ROW(row)->content.items[i])
#define CURRENT_ROW ROW(CURRENT_Y_POS)
#define CURRENT_CHAR CHAR(CURRENT_X_POS)
#define N_PAGES (editor.rows.count/editor.screenrows + 1)

/* ANSI escape sequences */
#define ANSI_GO_HOME_CURSOR "\x1B[H"
#define ANSI_SHOW_CURSOR "\x1b[?25h"
#define ANSI_HIDE_CURSOR "\x1b[?25l"
#define ANSI_ERASE_LINE_FROM_CURSOR "\x1b[K"
#define ANSI_CLEAR_SCREEN "\x1b[2J"
#define ANSI_INVERSE "\x1b[7m"
#define ANSI_RESET "\x1b[0m"
#define ANSI_SAVE_CURSOR "\x1b[s"
#define ANSI_RESTORE_CURSOR "\x1b[u"
///

typedef enum
{
    KEY_NULL  = 0,
    TAB       = 9,
    ENTER     = 13,
    CTRL_H    = 8,
    CTRL_J    = 10,
    CTRL_K    = 11,
    CTRL_L    = 12,
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

    ALT_BACKSPACE,
    ALT_COLON,
} Key;


// Global variables ////////////////////////////// 
int DISCARD_CHAR_RETURN = 0;
Editor editor = {0};
Config config = {0};
String screen_buf = {0};
//////////////////////////////////////////////////

const char *logpath = "./log.txt";
void log_this(char *format, ...)
{
    if (!DEBUG) return;

    FILE *logfile = fopen(logpath, "a");
    if (logfile == NULL) {
        fprintf(stderr, "Could not open log file at `%s`\n", logpath);
        exit(1);
    }
    va_list fmt; 

    va_start(fmt, format);
    vfprintf(logfile, format, fmt);
    fprintf(logfile, "\n");

    va_end(fmt);
    fclose(logfile);
}

void set_message(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024] = {0};
    vsnprintf(buf, editor.screencols, fmt, ap);
    s_clear(&editor.message);
    s_push_cstr(&editor.message, buf);
    editor.msg_time = time(NULL);
    va_end(ap);
}

struct termios term_old;
void reset_terminal(void)
{
    if (editor.rawmode) {
        term_old.c_iflag |= ICRNL;
        tcsetattr(STDIN_FILENO ,TCSAFLUSH, &term_old);
        editor.rawmode = false;
    }
}

void at_exit(void)
{
    s_free(&screen_buf);
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_GO_HOME_CURSOR);
    reset_terminal();
}

bool enable_raw_mode(void)
{
    if (editor.rawmode) return true;
    if (!isatty(STDIN_FILENO)) return false;
    atexit(at_exit);
    if (tcgetattr(STDIN_FILENO, &term_old) == -1) return false;

    struct termios term_raw = term_old;
    term_raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    term_raw.c_oflag &= ~(OPOST);
    term_raw.c_cflag |= (CS8);
    term_raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    term_raw.c_cc[VMIN] = 0;
    term_raw.c_cc[VTIME] = 0; // TODO: vedere come cambia la reattività per le ALT-sequences

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_raw) < 0) return false;
    editor.rawmode = true;
    return true;
}

int read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread == -1) exit(1); // TODO: report error (stdin probably failed)

    while (true) { // TODO: perché?
        switch (c) {
        case ESC:    /* escape sequence */
            int seq[3] = {0};

            /* If this is just an ESC, we'll timeout here. */
            if (read(STDIN_FILENO, seq, 1) == 0) return ESC;

            if (read(STDIN_FILENO, seq+1, 1) == 0) {
                switch (seq[0]) { /* ALT-X sequence */
                    case '0'      : return ALT_0;
                    case '1'      : return ALT_1;
                    case '2'      : return ALT_2;
                    case '3'      : return ALT_3;
                    case '4'      : return ALT_4;
                    case '5'      : return ALT_5;
                    case '6'      : return ALT_6;
                    case '7'      : return ALT_7;
                    case '8'      : return ALT_8;
                    case '9'      : return ALT_9;

                    case 'k'      : return ALT_k;
                    case 'K'      : return ALT_K;
                    case 'j'      : return ALT_j;
                    case 'J'      : return ALT_J;
                    case 'h'      : return ALT_h;
                    case 'H'      : return ALT_H;
                    case 'l'      : return ALT_l;
                    case 'L'      : return ALT_L;
                    case BACKSPACE: return ALT_BACKSPACE;
                    case ':'      : return ALT_COLON;
                    default       : return ESC;
                }
            } else return ESC;

            // TODO: probabilmente non userò queste sequenze, ma il codice lo tengo, non si sa mai
            /* ESC [ sequences. */
            //if (seq[0] == '[') {
            //    if (isdigit(seq[1])) {
            //        /* Extended escape, read additional byte. */
            //        if (read(STDIN_FILENO, seq+2, 1) == 0) return ESC;
            //        log_this("2: %d", seq[2]);
            //        if (seq[2] == '~') {
            //            //set_message("Pressed: crazy ESC ~ sequence");
            //            log_this("Pressed: crazy ESC ~ sequence");
            //            return '\0';
            //            //switch (seq[1]) {
            //            //case '3': return DEL_KEY;
            //            //case '5': return PAGE_UP;
            //            //case '6': return PAGE_DOWN;
            //            //}
            //        }
            //    } else {
            //        //set_message("Pressed: crazy ESC sequence");
            //        log_this("Pressed: crazy ESC sequence");
            //        return '\0';
            //        //switch (seq[1]) {
            //        //case 'A': return ARROW_UP;
            //        //case 'B': return ARROW_DOWN;
            //        //case 'C': return ARROW_RIGHT;
            //        //case 'D': return ARROW_LEFT;
            //        //case 'H': return HOME_KEY;
            //        //case 'F': return END_KEY;
            //        //}
            //    }
            //}

            ///* ESC O sequences. */
            //else if (seq[0] == 'O') {
            //    //set_message("Pressed: crazy ESC O sequence");
            //    log_this("Pressed: crazy ESC O sequence");
            //    return '\0';
            //    //switch(seq[1]) {
            //    //case 'H': return HOME_KEY;
            //    //case 'F': return END_KEY;
            //    //}
            //}
        default: return c;
        }
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
void update_window_size(void)
{
    if (!get_window_size(&editor.screenrows, &editor.screencols)) {
        perror("Unable to query the screen for size (columns / rows)");
        exit(1);
    }
    editor.screenrows -= 2;
}

void refresh_screen(void)
{
    s_clear(&screen_buf);
    s_push_cstr(&screen_buf, ANSI_HIDE_CURSOR);
    s_push_cstr(&screen_buf, ANSI_GO_HOME_CURSOR);

    Row *row;
    size_t filerow;
    for (filerow = editor.rowoff; filerow < editor.rowoff+editor.screenrows; filerow++) {
        if (filerow >= editor.rows.count) { // TODO: poi lo voglio togliere, ora lo uso per debuggare
            s_push_cstr(&screen_buf,"~" ANSI_ERASE_LINE_FROM_CURSOR CRNL);
            continue;
        }

        row = ROW(filerow);

        size_t len = row->content.count - editor.coloff;
        if (len > 0) {
            if (len > editor.screencols) len = editor.screencols; // TODO: viene troncata la riga
            int c;
            for (size_t j = 0; j < len; j++) {
                c = row->content.items[j];
                if (!isprint(c)) {
                    s_push_cstr(&screen_buf, ANSI_INVERSE);
                    if (c <= 26) {
                        s_push(&screen_buf, '^');
                        s_push(&screen_buf, '@'+c);
                    } else s_push(&screen_buf, '?');
                    s_push_cstr(&screen_buf, ANSI_RESET);
                } else s_push(&screen_buf, c);
            }
        }
        s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR CRNL);
    }

    /* Create a two rows status. First row: */
    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    s_push_cstr(&screen_buf, ANSI_INVERSE); // displayed with inversed colours

    char perc_buf[4];
    if (CURRENT_Y_POS == 0) sprintf(perc_buf, "%s", "Top");
    else if (CURRENT_Y_POS >= editor.rows.count-1) sprintf(perc_buf, "%s", "Bot");
    else sprintf(perc_buf, "%d%%", (int)(((float)(CURRENT_Y_POS)/(editor.rows.count))*100));

    char status[80];
    size_t len = snprintf(status, sizeof(status), " %s %c (%zu, %zu) | %zu lines (%s) | page %zu/%zu", 
            editor.filename == NULL ? "unnamed" : editor.filename, 
            editor.dirty ? '+' : '-',
            editor.cx+1, 
            CURRENT_Y_POS+1, 
            editor.rows.count, 
            perc_buf,
            editor.page+1,
            N_PAGES
    );
    if (len > editor.screencols) len = editor.screencols;
    s_push_str(&screen_buf, status, len);

    char rstatus[80];
    size_t rlen = editor.N == N_DEFAULT ? 0 : snprintf(rstatus, sizeof(rstatus), "%d", editor.N);
    while (len < editor.screencols && editor.screencols - (len+1) != rlen) {
        s_push(&screen_buf, ' ');
        len++;
    }
    if (editor.N != N_DEFAULT) s_push_str(&screen_buf, rstatus, rlen);
    s_push(&screen_buf, ' ');
    s_push_cstr(&screen_buf, ANSI_RESET CRNL);

    /* Second row depends on editor.message and the message remaining time. */
    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    if (editor.in_cmd) {
        s_push_cstr(&screen_buf, "Command: ");
        s_push_str(&screen_buf, editor.cmd.items, editor.cmd.count);
    } else {
        size_t msglen = editor.message.count;
        if (msglen && time(NULL)-editor.msg_time < config.msg_lifetime)
            s_push_str(&screen_buf, editor.message.items, msglen);
    }

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'editor.cx'
     * because of TABs. */
    size_t cx = editor.cx+1;
    size_t cy = editor.cy+1;
    if (editor.in_cmd) {
        cx = strlen("Command: ")+editor.cmd_pos+1;
        cy = editor.screencols-1;
    } else {
        filerow = CURRENT_Y_POS;
        if (filerow < editor.rows.count) {
            Row *row = ROW(filerow);
            size_t rowlen = strlen(row->content.items);
            for (size_t j = editor.coloff; j < (CURRENT_X_POS); j++) {
                if (j < rowlen && row->content.items[j] == TAB) cx += 4;
            }
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", cy, cx);
    s_push_str(&screen_buf, buf, strlen(buf));

    s_push_cstr(&screen_buf, ANSI_SHOW_CURSOR);
    s_push_null(&screen_buf);
    write(STDOUT_FILENO, screen_buf.items, screen_buf.count);
    s_clear(&screen_buf);
}


void handle_sigwinch(int signo)
{
    (void)signo;
    update_window_size();
    if (editor.cy > editor.screenrows) editor.cy = editor.screenrows - 1;
    if (editor.cx > editor.screencols) editor.cx = editor.screencols - 1;
    refresh_screen();
}

typedef enum
{
    UINT,
    //STRING
} FieldType;

char *fieldtype_to_string(FieldType type)
{
    switch (type)
    {
        case UINT: return "uint";
        //case STRING: return "string";
        default:
            fprintf(stderr, "Unreachable\n");
            abort();
    }
}

typedef struct
{
    const char *name;
    FieldType type;
    void *ptr;
    const void *thefault;
} ConfigField;

da_decl(ConfigField, ConfigFields);

ConfigField create_field(const char *name, FieldType type, void *ptr, const void *thefault)
{
    return (ConfigField){
        .name=name,
        .type=type,
        .ptr=ptr,
        .thefault=thefault
    };
}

#define ADD_CONFIG_FIELD(name, type)                                                            \
    do {                                                                                        \
        __field = create_field(#name, (type), (void *)&config.name, (void *)&default_ ## name); \
        da_push(&remaining_fields, __field);                                                    \
    } while (0);

void print_config()
{
    printf("Config {\n");
    printf("    quit_times: %zu\n", config.quit_times);
    printf("    msg_lifetime: %zu\n", config.msg_lifetime);
    printf("}\n");
}

#define CONFIG_PATH ".config/editor/config"
void load_config()
{
    char *home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "Env variable HOME not set\n");
        exit(1);
    }
    String config_log = s_new_empty();
    char full_config_path[256];
    sprintf(full_config_path, "%s/%s", home, CONFIG_PATH);
    FILE *config_file = fopen(full_config_path, "r");
    if (config_file == NULL) {
        s_push_fstr(&config_log, "WARNING: config file not found at %s\n", full_config_path);
        config_file = fopen(full_config_path, "w+");
        if (config_file == NULL) {
            s_push_fstr(&config_log, "ERROR: could not create config file\n");
            s_print(config_log);
            exit(1);
        }

        fprintf(config_file, "quit_times: %zu\n", default_quit_times);
        fprintf(config_file, "msg_lifetime: %zu\n", default_msg_lifetime);

        s_push_fstr(&config_log, "NOTE: default config file has been created\n\n");
        rewind(config_file);
    }

    ConfigFields remaining_fields = {0};
    da_default(&remaining_fields);
    ConfigField __field;

    ADD_CONFIG_FIELD(quit_times,   UINT);
    ADD_CONFIG_FIELD(msg_lifetime, UINT);

    //if (DEBUG) {
    //    for (size_t i = 0; i < remaining_fields.count; i++) {
    //        const ConfigField *field = &remaining_fields.items[i];
    //        printf("Field { name: `%s`, type: `%s` }\n", field->name, fieldtype_to_string(field->type));
    //    }
    //}

    ConfigFields inserted_fields = {0};
    da_default(&inserted_fields);

    ssize_t res; 
    size_t len;
    char *full_line = NULL;
    while ((res = getline(&full_line, &len, config_file)) != -1) {
        res--;
        if (res == 0) continue;
        char *line = full_line;
        line[res] = '\0';
        while (isspace(*line)) {
            line++;
            res--;
        }
        if (res == 0) continue;
        char *end = line+res-1;
        while (isspace(*end)) {
            end--;
            res--;
        }
        *(end+1) = '\0';

        char *colon = NULL;
        if ((colon = strchr(line, ':')) != NULL) {
            *colon = '\0';
            char *field_name = line;
            char *field_value = colon+1;
            while(isspace(*field_value)) field_value++;

            size_t i = 0;
            bool found = false;
            while (!found && i < remaining_fields.count) {
                if (streq(field_name, remaining_fields.items[i].name)) {
                    ConfigField removed_field;
                    da_remove(&remaining_fields, (int)i, removed_field);
                    da_push(&inserted_fields, removed_field);
                    switch (removed_field.type)
                    {
                        case UINT: {
                            size_t value = atoi(field_value);
                            if (value == 0) {
                                s_push_fstr(&config_log, "ERROR: field `%s` expects an integer greater than 0, but got `%s`\n", field_name, field_value);
                                s_push_fstr(&config_log, "NOTE: defaulted to `%zu`\n", *((size_t *)removed_field.thefault));
                                *((size_t *)removed_field.ptr) = *((size_t *)removed_field.thefault);
                            } else {
                                *((size_t *)removed_field.ptr) = value;
                                //printf("CONFIG: field `%s` set to `%zu`\n", field_name, value);
                            }
                        } break;
                        //case STRING: {
                        //    char *value = field_value;
                        //    if (strlen(value) == 0) {
                        //        fprintf(stderr, "ERROR: field `%s` expects a non empty string value\n", field_name);
                        //        fprintf(stderr, "NOTE: defaulted to `%s`\n", *((char **)removed_field.thefault));
                        //        *((char **)removed_field.ptr) = strdup(*((char **)removed_field.thefault));
                        //    } else {
                        //        *((char **)removed_field.ptr) = strdup(value);
                        //        //printf("CONFIG: field `%s` set to `%s`\n", field_name, value);
                        //    }
                        //} break;
                        default:
                            fprintf(stderr, "Unreachable field type in load_config\n");
                            abort();
                    }
                    found = true;
                } else i++;
            }

            if (!found) {
                found = false;
                i = 0;
                while (!found && i < inserted_fields.count) {
                    if (streq(field_name, inserted_fields.items[i].name)) {
                        s_push_fstr(&config_log, "ERROR: redeclaration of field `%s`\n", field_name);
                        found = true;
                    } else i++;
                }
            }

            if (!found) s_push_fstr(&config_log, "ERROR: unknown field `%s`\n", field_name);

        } else if (*line == '#') {
            line++;
            s_push_fstr(&config_log, "TOOD: define command `%s`\n", line);
        } else {
            s_push_fstr(&config_log, "ERROR: I don't know what to do with `%s`\n", line);
            // TODO: track and report location
        }
    }
    free(full_line);

    if (remaining_fields.count > 0) {
        s_push_cstr(&config_log, "\nWARNING: the following fields have not been set:\n");
        for (size_t i = 0; i < remaining_fields.count; i++) {
            const ConfigField *field = &remaining_fields.items[i];
            s_push_fstr(&config_log, " -> %s (type: %s, default: ", field->name, fieldtype_to_string(field->type));
            switch (field->type)
            {
                case UINT:
                    s_push_fstr(&config_log, "`%zu`)\n", *(size_t *)field->thefault);
                    *((size_t *)field->ptr) = *((size_t *)field->thefault);
                    break;
                //case STRING:
                //    fprintf(stderr, "`%s`)\n", *(char **)field->thefault);
                //    *((char **)field->ptr) = strdup(*((char **)field->thefault));
                //    break;
                default:
                    fprintf(stderr, "Unreachable field type in load_config\n");
                    abort();
            }
        }
    }

    if (!s_is_empty(config_log)) {
        s_push_null(&config_log);
        s_print(config_log);
        printf("\n-- Press ENTER to continue --\n");
        printf(ANSI_HIDE_CURSOR"\n");
        while (getc(stdin) != '\n')
            ;
        printf(ANSI_SHOW_CURSOR"\n");
    }
}

void initialize()
{
    screen_buf = (String){0};
    da_default(&screen_buf);

    editor.filename = NULL;

    editor.rows = (Rows){0};
    da_default(&editor.rows);

    editor.dirty = 0;
    editor.rawmode = false;

    editor.cx = 0;
    editor.cy = 0;
    editor.rowoff = 0;
    editor.coloff = 0;
    editor.page = 0;
    editor.current_quit_times = config.quit_times;

    editor.cmd = (String){0};
    da_default(&editor.cmd);
    editor.cmd_pos = 0;
    editor.in_cmd = false;

    da_default(&editor.message);
    editor.msg_time = (time_t)0;

    editor.N = N_DEFAULT;

    update_window_size();
    signal(SIGWINCH, handle_sigwinch);
}

void open_file(char *filename)
{
    if (editor.filename) free(editor.filename);
    da_clear(&editor.rows);

    if (filename == NULL) {
        editor.filename = strdup("new");
        return;
    }

    FILE *file = fopen(filename, "r");

    if (strchr(filename, '/')) {
        char *tmp = filename + strlen(filename) - 1;
        while (*tmp != '/') tmp--;
        filename = tmp+1;
    }
    editor.filename = strdup(filename); // TODO: rember to free (and check at the beginning of this function)

    if (file == NULL) return;

    ssize_t res; 
    size_t len;
    char *line = NULL;
    while ((res = getline(&line, &len, file)) != -1) {
        Row row = {0};
        da_default(&row.content);
        s_push_str(&row.content, line, strlen(line)-1);
        da_push(&editor.rows, row);
    }
    free(line);
}

//void draw_rows()
//{
//    for (size_t i = 0; i < editor.screenrows; i++) {
//        if (i < editor.rows.count) {
//            Row *row = ROW(editor.page*editor.screenrows+editor.rowoff+i);
//            s_push_str(&screen_buf, row->content.items, strlen(row->content.items));    
//        }
//        s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR CRNL);
//    }
//}

//int curr_row_percentile() { return (int)(((float)editor.rowoff/(editor.rows.count))*100); }

// TODO: la status bar viene ridisegnata quando needs_redraw e' true, forse devo distinguere diverse variabili (quindi inserire status_bar_needs_redraw)
//void draw_status_bar()
//{
//    s_push_cstr(&screen_buf, "\x1b[7m");
//
//    char perc_buf[32] = {0};
//    if (editor.rowoff == 0) sprintf(perc_buf, "%s", "Top");
//    else if (editor.rowoff == editor.rows.count-1) sprintf(perc_buf, "%s", "Bot");
//    else sprintf(perc_buf, "%d%%", curr_row_percentile());
//
//    char status[128] = {0};
//    size_t len = snprintf(status, sizeof(status), " %s - (%zu, %zu) - %zu lines (%s) - [page %zu/%zu]", 
//            editor.filename == NULL ? "unnamed" : editor.filename, 
//            editor.cx+1, 
//            editor.rowoff+1, 
//            editor.rows.count, 
//            perc_buf,
//            editor.page+1, 
//            editor.pages
//    ); // TODO: percentuale
//    if (len > editor.screencols) len = editor.screencols;
//    s_push_str(&screen_buf, status, len);
//    while (len < editor.screencols) {
//        s_push_cstr(&screen_buf, " ");
//        len++;
//    }
//    s_push_cstr(&screen_buf, "\x1b[m");
//}

// TODO: now it's trivial
void move_cursor_up() 
{
    if (editor.cy == 0 && editor.page != 0) editor.rowoff--; // TODO: deve cambiare anche la pagina
    if (editor.cy > 0) editor.cy -= 1;
    else editor.cy = 0;
    //if (editor.rowoff > N_OR_DEFAULT(0)) editor.rowoff -= N_OR_DEFAULT(1);
    //else editor.rowoff = 0;
}

// TODO: now it's trivial
void move_cursor_down()
{ 
    //if ((editor.rowoff+N_OR_DEFAULT(1) / N_PAGES) > editor.page) editor.page += (editor.rowoff+N_OR_DEFAULT(1)) / N_PAGES; // TODO: No, non funziona cosi' devo modificare l'rowoff in questo caso
    if (editor.cy == editor.screenrows-1 && editor.page != N_PAGES-1) editor.rowoff++; // TODO: deve cambiare anche la pagina
    if (editor.cy < editor.screenrows-1) editor.cy += 1; 
    else editor.cy = editor.screenrows-1;
    //if (editor.rowoff < editor.rows.count-N_OR_DEFAULT(0)-1) editor.rowoff += N_OR_DEFAULT(1); 
    //else editor.rowoff = editor.rows.count-1;
}

// TODO: now it's trivial
void move_cursor_left()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos > 0) editor.cmd_pos -= 1;
        else editor.cmd_pos = 0;
    } else {
        if (editor.cx > 0) editor.cx -= 1;
        else editor.cx = 0;
    }
}

// TODO: now it's trivial
void move_cursor_right()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos < editor.cmd.count-1) editor.cmd_pos += 1;
        else editor.cmd_pos = editor.cmd.count;
    } else {
        if (editor.cx < editor.screencols-1) editor.cx += 1;
        else editor.cx = editor.screencols-1;
    }
}

void scroll_up() // TODO: cy va oltre
{ 
    if (editor.rowoff > N_OR_DEFAULT(0)) {
        editor.rowoff -= N_OR_DEFAULT(1);
        editor.cy += N_OR_DEFAULT(1);
    } else editor.rowoff = 0;
}             

void scroll_down() // TODO: cy fa cose strane
{
    if (editor.rowoff < editor.rows.count-N_OR_DEFAULT(0)-1) {
        editor.rowoff += N_OR_DEFAULT(1);
        editor.cy -= N_OR_DEFAULT(1);
    } else editor.rowoff = editor.rows.count-1;
}

void move_page_up() // TODO: non funziona benissimo
{
    if (editor.page > N_OR_DEFAULT(0)) editor.page -= N_OR_DEFAULT(1);
    else editor.page = 0;
    editor.rowoff = editor.page*editor.screenrows + editor.cy;
}             

void move_page_down() // TODO: non funziona benissimo
{
    if (editor.page < N_PAGES-N_OR_DEFAULT(0)-1) editor.page += N_OR_DEFAULT(1);
    else editor.page = N_PAGES-1;
    editor.rowoff = editor.page*editor.screenrows + editor.cy;
}           

void move_cursor_begin_of_screen() { editor.cy = 0; }        

void move_cursor_end_of_screen() { editor.cy = editor.screenrows-1; }

void move_cursor_begin_of_file()
{
    editor.page = 0;
    editor.rowoff = 0;
    editor.cy = 0;
}        

void move_cursor_end_of_file() 
{
    editor.page = N_PAGES-1;
    editor.rowoff = editor.rows.count-editor.screenrows;
    editor.cy = editor.screenrows-1;
}

void move_cursor_begin_of_line() { editor.cx = 0; }

void move_cursor_end_of_line()   { editor.cx = editor.screencols-1; } // TODO: coloff

void move_cursor_first_non_space()
{
    Row *row = CURRENT_ROW;
    char *str = row->content.items;
    editor.cx = 0;
    while (*str != '\0' && isspace(*str)) {
        editor.cx++;
        str++;
    }
}

void move_cursor_last_non_space()
{
    Row *row = CURRENT_ROW;
    char *str = row->content.items;
    size_t len = strlen(str);
    if (len == 0) return;
    int i = len - 1;
    while (i >= 0 && isspace(str[i]))
        i--;
    editor.cx = (size_t)i == editor.screencols-1 ? i : i+1;
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
    da_default(&save_buf);
    // TODO: build save_buf from editor.rows
    Row *row;
    for (size_t i = 0; i < editor.rows.count; i++) {
        row = ROW(i);
        s_push_str(&save_buf, row->content.items, row->content.count);
        s_push(&save_buf, '\n');
    }

    // TODO: make the user decide, in the status line, the name of the file if not set
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
    set_message("%zu bytes written on disk", len);
    return 0;

writeerr:
    s_free(&save_buf);
    if (fd != -1) close(fd);
    set_message("Can't save! I/O error: %s", strerror(errno));
    return 1;
}

bool quit(void)
{
    if (editor.dirty && editor.current_quit_times) {
        set_message("Session is not saved. If you really want to quit press CTRL-q %zu more time%s.", editor.current_quit_times, editor.current_quit_times == 1 ? "" : "s");
        editor.current_quit_times--;
        return false;
    }
    return true;
}

void insert_char_at(Row *row, size_t at, int c)
{
    if (at > row->content.count) {
        size_t padlen = at-row->content.count;
        for (size_t i = 0; i < padlen; i++)
            da_push(&row->content, ' ');
        da_push(&row->content, c);
    } else {
        da_insert(&row->content, c, (int)at);
        //if (row->content.count == 0) { // TODO: qui si puo' fare solo insert?
        //    da_push(&row->content, c);
        //} else {
        //    da_insert(&row->content, c, (int)at);
        //}
    }
}

typedef enum
{
    MOVE_CURSOR_UP,
    MOVE_CURSOR_DOWN,
    MOVE_CURSOR_LEFT,
    MOVE_CURSOR_RIGHT,
    MOVE_LINE_UP,
    MOVE_LINE_DOWN,
    UNKNOWN,
    CMDS_COUNT
} Command;

Command parse_cmd()
{
    char *cmd = editor.cmd.items;
    static_assert(CMDS_COUNT == 7, "Parse all commands in parse_cmd");
    if      (streq(cmd, "u"))   return MOVE_CURSOR_UP;
    else if (streq(cmd, "d"))   return MOVE_CURSOR_DOWN;
    else if (streq(cmd, "l"))   return MOVE_CURSOR_LEFT;
    else if (streq(cmd, "r"))   return MOVE_CURSOR_RIGHT;
    else if (streq(cmd, "lnu")) return MOVE_LINE_UP;
    else if (streq(cmd, "lnd")) return MOVE_LINE_DOWN;
    else                        return UNKNOWN;
}

void move_line_up()
{
    size_t y = CURRENT_Y_POS;
    if (y == 0) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y-1];
    editor.rows.items[y-1] = tmp;
    editor.cy--;
}

void move_line_down()
{
    size_t y = CURRENT_Y_POS;
    if (y == editor.screenrows-1) return;
    Row tmp = editor.rows.items[y];
    editor.rows.items[y] = editor.rows.items[y+1];
    editor.rows.items[y+1] = tmp;
    editor.cy++;
}

void execute_cmd() // TODO: al posto di N_TIMES parsare i primi caratteri e se sono un numero usare quel numero come moltiplicità, cosi' si puo' scrivere 3lnd e spostera' la riga giu' di 3
                   // - lasciare anche il cursore dov'era nell'editor invece di spostarlo solamente nella command line
                   // - argomenti dei comandi, ad esempio `i=ciao` inserisce "ciao" (cmd=arg1,arg2), vanno parsati e quindi Command sara' una struttura piu' complessa con un da di stringhe
                   // - se separati da uno spazio sono due comandi concatenati
                   // - storia dei comandi usati, si scorre con ALT_k e ALT_j
{
    static_assert(CMDS_COUNT == 7, "Implement all commands in execute_cmd");
    Command cmd = parse_cmd();
    switch (cmd)
    {
        case MOVE_CURSOR_UP   : N_TIMES move_cursor_up();    break;
        case MOVE_CURSOR_DOWN : N_TIMES move_cursor_down();  break; 
        case MOVE_CURSOR_LEFT : N_TIMES move_cursor_left();  break;
        case MOVE_CURSOR_RIGHT: N_TIMES move_cursor_right(); break;
        case MOVE_LINE_UP     : N_TIMES move_line_up();      break;
        case MOVE_LINE_DOWN   : N_TIMES move_line_down();    break;

        case UNKNOWN:
        default:
            set_message("Unknown command `%s`", editor.cmd.items); 
    }
}

void insert_char(char c)
{
    if (editor.in_cmd) {
        if (c == '\n') {
            s_push_null(&editor.cmd);
            editor.in_cmd = false;
            execute_cmd();
        } else {
            da_insert(&editor.cmd, c, (int)(editor.cmd_pos));
            editor.cmd_pos++;
        }
        return;
    }

    size_t y = CURRENT_Y_POS;
    size_t x = CURRENT_X_POS;

    if (c == '\n') {
        if (y == editor.rows.count) {
            Row newrow = {0};
            da_default(&newrow.content);
            da_push(&editor.rows, newrow);
        } else {
            Row *row = CURRENT_ROW;
            if (x >= row->content.count) x = row->content.count;
            if (x == 0) {
                Row newrow = {0};
                da_default(&newrow.content);
                da_insert(&editor.rows, newrow, (int)y);
            } else {
                /* We are in the middle of a line. Split it between two rows. */
                Row newrow = {0};
                da_default(&newrow.content);
                s_push_str(&newrow.content, row->content.items+x, row->content.count-x);
                da_insert(&editor.rows, newrow, (int)y+1);
                row->content.count = x;
            }
        }
        if (editor.cy == editor.screenrows-1) editor.rowoff++;
        else editor.cy++;
        editor.cx = 0;
        editor.coloff = 0;
    } else {
        if (y >= editor.rows.count) {
            while (editor.rows.count <= y) {
                Row newrow = {0};
                da_default(&newrow.content);
                da_push(&editor.rows, newrow);
            }
        }
        insert_char_at(CURRENT_ROW, x, c);
        if (editor.cx == editor.screencols-1) editor.coloff++; // TODO: funzione/macro per `editor.cx == editor.screencols-1`
        else editor.cx++;
        editor.dirty++;
    }
}

void insert_newline_and_keep_pos()
{
    set_message("TODO: insert_newline_and_keep_pos");
}

void delete_char_at(Row *row, size_t at)
{
    if (row->content.count <= at) return;
    da_remove(&row->content, (int)at, DISCARD_CHAR_RETURN);
}

void delete_char()
{
    if (editor.in_cmd) {
        if (editor.cmd.count <= 0) return;
        if (editor.cmd_pos == 0) return;
        da_remove(&editor.cmd, (int)editor.cmd_pos-1, DISCARD_CHAR_RETURN);
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
        Row _;
        (void)_;
        da_remove(&editor.rows, (int)y, _);
        if (editor.cy == 0) editor.rowoff--;
        else editor.cy--;
        editor.cx = x;
        if (editor.cx >= editor.screencols) {
            int shift = (editor.screencols-editor.cx)+1;
            editor.cx -= shift;
            editor.coloff += shift;
        }
    } else {
        delete_char_at(row, x-1);
        if (editor.cx == 0 && editor.coloff) editor.coloff--;
        else editor.cx--;
    }
    editor.dirty++;
}

void delete_word()
{
    if (editor.in_cmd) {
        while (editor.cmd_pos > 0 && isspace(editor.cmd.items[editor.cmd_pos])) {
            da_remove(&editor.cmd, (int)editor.cmd_pos-1, DISCARD_CHAR_RETURN);
            editor.cmd_pos--;
        }
        while (editor.cmd_pos > 0 && !isspace(editor.cmd.items[editor.cmd_pos])) {
            da_remove(&editor.cmd, (int)editor.cmd_pos-1, DISCARD_CHAR_RETURN);
            editor.cmd_pos--;
        }
        return;
    }

    size_t y = CURRENT_Y_POS;
    if (y >= editor.rows.count) return;
    size_t x = CURRENT_X_POS;
    if (x == 0 && y == 0) return;
    Row *row = CURRENT_ROW;
    while (x > 0 && isspace(CHAR(CURRENT_Y_POS, x))) {
        delete_char_at(row, x-1);
        x--;
        editor.cx--;
    }
    while (x > 0 && !isspace(CHAR(CURRENT_Y_POS, x))) {
        delete_char_at(row, x-1);
        x--;
        editor.cx--;
    }
}

void command()
{
    s_clear(&editor.cmd);
    editor.cmd_pos = 0;
    editor.in_cmd = true;
}

void process_pressed_key(void)
{
    int key = read_key();
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
        case ALT_9: // TODO: function
            int digit = key - ALT_0;
            if (editor.N == N_DEFAULT) {
                if (digit == 0) break;
                else editor.N = digit;
            } else {
                editor.N *= 10;
                editor.N += digit;
            }
            char bufN[64] = {0};
            itoa(editor.N, bufN);
            has_inserted_number = true;
            break;

        case ALT_k: N_TIMES move_cursor_up();    break;
        case ALT_j: N_TIMES move_cursor_down();  break;
        case ALT_h: N_TIMES move_cursor_left();  break;
        case ALT_l: N_TIMES move_cursor_right(); break;

        case ALT_K: move_cursor_begin_of_screen(); break;
        case ALT_J: move_cursor_end_of_screen();   break;
        case ALT_H: move_cursor_first_non_space(); break;
        case ALT_L: move_cursor_last_non_space();  break;

        //case CTRL_K: N_TIMES scroll_up();                          break;
        //case CTRL_J: N_TIMES scroll_down();                        break;
        //case CTRL_H: set_message("TODO: CTRL-H"); break;
        //case CTRL_L: set_message("TODO: CTRL-L"); break;

        //case ALT_k: N_TIMES move_page_up();                      break; 
        //case ALT_j: N_TIMES move_page_down();                    break;
        //case ALT_h: set_message("TODO: ALT-h"); break;
        //case ALT_l: set_message("TODO: ALT-l"); break;
        //            
        //case ALT_K: move_cursor_begin_of_file(); break;
        //case ALT_J: move_cursor_end_of_file();   break;
        //case ALT_H: move_cursor_begin_of_line(); break;
        //case ALT_L: move_cursor_end_of_line();   break; 

        case ALT_COLON: command(); break;
        case ALT_BACKSPACE: N_TIMES delete_word(); break;

        case TAB:
            if (editor.in_cmd) {
                // TODO: autocomplete command
            } else {
                set_message("TODO: insert TAB");
                //insert_char('\t');
            }
            break;

        case ENTER: // TODO: se si e' in_cmd si esegue execute_cmd (che fa anche il resto)
            N_TIMES insert_char('\n');
            break;

        case CTRL_Q:
            if (quit()) exit(0);
            else return;

        case CTRL_S:
            save();
            break;

        case BACKSPACE:
            N_TIMES { delete_char(); }
            break;

        //case PAGE_UP:
        //case PAGE_DOWN:
        //    if (c == PAGE_UP && E.cy != 0)
        //        E.cy = 0;
        //    else if (c == PAGE_DOWN && E.cy != E.screenrows-1)
        //        E.cy = E.screenrows-1;
        //    {
        //    int times = E.screenrows;
        //    while(times--)
        //        editorMoveCursor(c == PAGE_UP ? ARROW_UP:
        //                                        ARROW_DOWN);
        //    }
        //    break;

        case ESC:
            if (editor.in_cmd) editor.in_cmd = false;
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
    if (argc <= 0 || argc >= 3 ) {
        fprintf(stderr, "TODO: usage\n");
        exit(1);
    }

    char *filename = argc == 2 ? argv[1] : NULL;

    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    load_config();
    initialize();
    open_file(filename);
    enable_raw_mode();

    while (true) {
        refresh_screen();
        process_pressed_key();
    }

    return 0;
}
