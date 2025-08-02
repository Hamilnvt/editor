/* TODO:
    - needs_redraw technology (ma prima deve funzionare tutto il resto)
    - capire come leggere ctrl-shift-x
    - fare dei config che vengono caricati all'inizio (quit_times, message_max_time...)
    - funzioni separate per quando si scrive il comando (altrimenti non si capisce niente)
      > cambiare anche process_pressed_key in modo da prendere solo gli input che valgono anche per il comando (altrimenti dovrei controllare in ogni funzione se si e' in_cmd e poi fare return

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
    size_t quit_times;

    //char message[256];
    String message;
    time_t message_time;
    time_t message_max_time;

    String cmd;
    size_t cmd_pos;
    bool in_cmd;

    int N; /* multiplicity for the command */
} Editor;

#define N_DEFAULT -1 
#define N_OR_DEFAULT(n) ((size_t)(editor.N == N_DEFAULT ? (n) : editor.N))
#define QUIT_DEFAULT 3

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

    ALT_k     = 1000,
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
int DISCARD_CHAR_RETURN;
Editor editor = {0};
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
    editor.message_time = time(NULL);
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
    size_t len = snprintf(status, sizeof(status), " %s - (%zu, %zu) - %zu lines (%s) - [page %zu/%zu]", 
            editor.filename == NULL ? "unnamed" : editor.filename, 
            editor.cx+1, 
            CURRENT_Y_POS+1, 
            editor.rows.count, 
            perc_buf,
            editor.page+1,
            N_PAGES
    ); // TODO: percentuale
    //size_t len = snprintf(status, sizeof(status), "%.20s - %zu lines %s",
    //                      editor.filename, editor.rows.count, editor.dirty ? "(modified)" : "");
    char rstatus[80];
    size_t rlen = snprintf(rstatus, sizeof(rstatus), "%zu/%zu", CURRENT_Y_POS+1, editor.rows.count);
    if (len > editor.screencols) len = editor.screencols;
    s_push_str(&screen_buf, status, len);
    while (len < editor.screencols) {
        if (editor.screencols - (len+1) == rlen) {
            break;
        } else {
            s_push(&screen_buf, ' ');
            len++;
        }
    }
    s_push_str(&screen_buf, rstatus, rlen);
    s_push(&screen_buf, ' ');
    s_push_cstr(&screen_buf, ANSI_RESET CRNL);

    /* Second row depends on editor.message and the message remaining time. */
    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    if (editor.in_cmd) {
        s_push_cstr(&screen_buf, "Command: ");
        s_push_str(&screen_buf, editor.cmd.items, editor.cmd.count);
    } else {
        size_t msglen = editor.message.count;
        if (msglen && time(NULL)-editor.message_time < 3) // TODO: make 3 a variable, max seconds to show the msg
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

void init()
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
    editor.quit_times = QUIT_DEFAULT;

    editor.cmd = (String){0};
    da_default(&editor.cmd);
    editor.cmd_pos = 0;
    editor.in_cmd = false;

    da_default(&editor.message);
    editor.message_time = (time_t)0;
    editor.message_max_time = (time_t)3;

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
    if (editor.cy > N_OR_DEFAULT(0)) editor.cy -= N_OR_DEFAULT(1);
    else editor.cy = 0;
    //if (editor.rowoff > N_OR_DEFAULT(0)) editor.rowoff -= N_OR_DEFAULT(1);
    //else editor.rowoff = 0;
}

// TODO: now it's trivial
void move_cursor_down()
{ 
    //if ((editor.rowoff+N_OR_DEFAULT(1) / N_PAGES) > editor.page) editor.page += (editor.rowoff+N_OR_DEFAULT(1)) / N_PAGES; // TODO: No, non funziona cosi' devo modificare l'rowoff in questo caso
    if (editor.cy == editor.screenrows-1 && editor.page != N_PAGES-1) editor.rowoff++; // TODO: deve cambiare anche la pagina
    if (editor.cy < editor.screenrows-N_OR_DEFAULT(0)-1) editor.cy += N_OR_DEFAULT(1); 
    else editor.cy = editor.screenrows-1;
    //if (editor.rowoff < editor.rows.count-N_OR_DEFAULT(0)-1) editor.rowoff += N_OR_DEFAULT(1); 
    //else editor.rowoff = editor.rows.count-1;
}

// TODO: now it's trivial
void move_cursor_left()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos > N_OR_DEFAULT(0)) editor.cmd_pos -= N_OR_DEFAULT(1);
        else editor.cmd_pos = 0;
    } else {
        if (editor.cx > N_OR_DEFAULT(0)) editor.cx -= N_OR_DEFAULT(1);
        else editor.cx = 0;
    }
}

// TODO: now it's trivial
void move_cursor_right()
{
    if (editor.in_cmd) {
        if (editor.cmd_pos < editor.cmd.count-N_OR_DEFAULT(0)-1) editor.cmd_pos += N_OR_DEFAULT(1);
        else editor.cmd_pos = editor.cmd.count;
    } else {
        if (editor.cx < editor.screencols-N_OR_DEFAULT(0)-1) editor.cx += N_OR_DEFAULT(1);
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
    if (editor.dirty && editor.quit_times) {
        set_message("Session is not saved. If you really want to quit press CTRL-q %zu more time%s.", editor.quit_times, editor.quit_times == 1 ? "" : "s");
        editor.quit_times--;
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
    CMD_NULL,
    UNKNOWN,
    MOVE_LINE_UP,
    MOVE_LINE_DOWN,
    CMDS_COUNT
} Command;

void execute_cmd()
{
    static_assert(CMDS_COUNT == 4, "Implement command in execute_cmd");
    set_message("TODO: parse and execute command `%s`", editor.cmd.items);
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

    if (isdigit(key)) {
        if (editor.N == N_DEFAULT) {
            if (key == '0') {
                editor.N = N_DEFAULT;
                return;
            } else editor.N = key - '0';
        } else {
            editor.N *= 10;
            editor.N += key - '0';
        }
        char bufN[64] = {0};
        itoa(editor.N, bufN);
        char msg[64];
        sprintf(msg, "number: %s", bufN);
        set_message(msg);
    } else {
        switch (key) {
            case ALT_k: move_cursor_up();    break;
            case ALT_j: move_cursor_down();  break;
            case ALT_h: move_cursor_left();  break;
            case ALT_l: move_cursor_right(); break;

            case ALT_K: move_cursor_begin_of_screen(); break;
            case ALT_J: move_cursor_end_of_screen();   break;
            case ALT_H: move_cursor_first_non_space(); break;
            case ALT_L: move_cursor_last_non_space();  break;

            //case CTRL_K: scroll_up();                          break;
            //case CTRL_J: scroll_down();                        break;
            //case CTRL_H: set_message("TODO: CTRL-H"); break;
            //case CTRL_L: set_message("TODO: CTRL-L"); break;

            //case ALT_k: move_page_up();                      break; 
            //case ALT_j: move_page_down();                    break;
            //case ALT_h: set_message("TODO: ALT-h"); break;
            //case ALT_l: set_message("TODO: ALT-l"); break;
            //            
            //case ALT_K: move_cursor_begin_of_file(); break;
            //case ALT_J: move_cursor_end_of_file();   break;
            //case ALT_H: move_cursor_begin_of_line(); break;
            //case ALT_L: move_cursor_end_of_line();   break; 

            case ALT_COLON: command(); break;
            case ALT_BACKSPACE: delete_word(); break;

            case TAB:
                if (editor.in_cmd) {
                    // TODO: autocomplete command
                } else {
                    set_message("TODO: insert TAB");
                    //insert_char('\t');
                }
                break;
            case ENTER: // TODO: se si e' in_cmd si esegue execute_cmd (che fa anche il resto)
                insert_char('\n');
                break;
            case CTRL_Q:
                if (quit()) exit(0);
                else return;
            case CTRL_S:
                save();
                break;
            case BACKSPACE:
                delete_char();
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
                if (isprint(key)) insert_char(key);
                break;
        }
        editor.N = N_DEFAULT;
    }
    editor.quit_times = QUIT_DEFAULT; /* Reset it to the original value. */
}

int main(int argc, char **argv)
{
    if (argc <= 0 || argc >= 3 ) {
        fprintf(stderr, "TODO: usage\n");
        exit(1);
    }

    char *filename = argc == 2 ? argv[1] : NULL;

    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
    init();
    open_file(filename);
    enable_raw_mode();

    while (true) {
        refresh_screen();
        process_pressed_key();
    }

    return 0;
}
