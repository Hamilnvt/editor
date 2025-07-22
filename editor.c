/* TODO:
    - needs_redraw tecnology (ma prima deve funzionare tutto il resto)

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
    size_t pages;
    size_t quit_times;

    char statusmsg[256];
    time_t statusmsg_time;
    time_t statusmsg_max_time;

    int N; /* multiplicity for the command */
} Editor;

#define N_DEFAULT -1 
#define N_OR_DEFAULT_VALUE(n) ((size_t)(editor.N == N_DEFAULT ? (n) : editor.N))
#define QUIT_DEFAULT 3

#define CRNL "\r\n"
#define GET_ROW(i) (&editor.rows.items[i])
#define GET_CURR_ROW (&editor.rows.items[editor.rowoff+editor.cy])

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
    CTRL_Q    = 17,
    CTRL_S    = 19,
    ESC       = 27,
    BACKSPACE = 127,
} Key;


// Global variables ////////////////////////////// 
Editor editor = {0};
String screen_buf = {0};
//////////////////////////////////////////////////

void editor_set_statusmsg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(editor.statusmsg, sizeof(editor.statusmsg), fmt, ap);
    va_end(ap);
    editor.statusmsg_time = time(NULL);
}

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

struct termios term_old;
void reset_terminal(void)
{
    if (editor.rawmode) {
        tcsetattr(STDIN_FILENO ,TCSAFLUSH, &term_old);
        editor.rawmode = false;
    }
}

void editor_at_exit(void)
{
    s_free(&screen_buf);
    reset_terminal();
    log_this("Exit");
    log_this("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
}

bool enable_raw_mode(void)
{
    if (editor.rawmode) return true;
    if (!isatty(STDIN_FILENO)) return false;
    atexit(editor_at_exit);
    if (tcgetattr(STDIN_FILENO, &term_old) == -1) return false;

    struct termios term_raw = term_old;
    term_raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    term_raw.c_oflag &= ~(OPOST);
    term_raw.c_cflag |= (CS8);
    term_raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    term_raw.c_cc[VMIN] = 0;
    term_raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &term_raw) < 0) return false;
    editor.rawmode = true;
    return true;
}

int editor_read_key()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread == -1) exit(1); // TODO: report error (stdin probably failed)

    while (true) {
        switch (c) {
        case ESC:    /* escape sequence */
            int seq[3];
            /* If this is just an ESC, we'll timeout here. */
            if (read(STDIN_FILENO, seq,   1) == 0) return ESC;
            if (read(STDIN_FILENO, seq+1, 1) == 0) return ESC;

            /* ESC [ sequences. */
            if (seq[0] == '[') {
                if (isdigit(seq[1])) {
                    /* Extended escape, read additional byte. */
                    if (read(STDIN_FILENO, seq+2, 1) == 0) return ESC;
                    if (seq[2] == '~') {
                        //editor_set_statusmsg("Pressed: crazy ESC ~ sequence");
                        log_this("Pressed: crazy ESC ~ sequence");
                        return '\0';
                        //switch (seq[1]) {
                        //case '3': return DEL_KEY;
                        //case '5': return PAGE_UP;
                        //case '6': return PAGE_DOWN;
                        //}
                    }
                } else {
                    //editor_set_statusmsg("Pressed: crazy ESC sequence");
                    log_this("Pressed: crazy ESC sequence");
                    return '\0';
                    //switch (seq[1]) {
                    //case 'A': return ARROW_UP;
                    //case 'B': return ARROW_DOWN;
                    //case 'C': return ARROW_RIGHT;
                    //case 'D': return ARROW_LEFT;
                    //case 'H': return HOME_KEY;
                    //case 'F': return END_KEY;
                    //}
                }
            }

            /* ESC O sequences. */
            else if (seq[0] == 'O') {
                //editor_set_statusmsg("Pressed: crazy ESC O sequence");
                log_this("Pressed: crazy ESC O sequence");
                return '\0';
                //switch(seq[1]) {
                //case 'H': return HOME_KEY;
                //case 'F': return END_KEY;
                //}
            }
            break;
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

void editor_refresh_screen(void)
{
    s_clear(&screen_buf);
    s_push_cstr(&screen_buf, ANSI_HIDE_CURSOR);
    s_push_cstr(&screen_buf, ANSI_GO_HOME_CURSOR);

    Row *row;
    size_t filerow;
    for (filerow = editor.rowoff; filerow < editor.rowoff+editor.screenrows; filerow++) {
        if (filerow >= editor.rows.count) {
            s_push_cstr(&screen_buf,"~" ANSI_ERASE_LINE_FROM_CURSOR CRNL);
            continue;
        }

        row = GET_ROW(filerow);

        size_t len = strlen(row->content.items) - editor.coloff;
        if (len > 0 && len > editor.screencols) len = editor.screencols; // TODO: viene troncata la riga
        s_push_str(&screen_buf, row->content.items, len);
        s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR CRNL);
    }

    /* Create a two rows status. First row: */
    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    s_push_cstr(&screen_buf, ANSI_INVERSE); // displayed with inversed colours
    char status[80];
    size_t len = snprintf(status, sizeof(status), "%.20s - %zu lines %s",
                          editor.filename, editor.rows.count, editor.dirty ? "(modified)" : "");
    log_this("status: `%s`", status);
    char rstatus[80];
    size_t rlen = snprintf(rstatus, sizeof(rstatus), "%zu/%zu", editor.rowoff+editor.cy+1, editor.rows.count);
    log_this("rstatus: `%s`", rstatus);
    if (len > editor.screencols) len = editor.screencols;
    s_push_str(&screen_buf, status, len);
    while (len < editor.screencols) {
        if (editor.screencols - len == rlen) {
            break;
        } else {
            s_push(&screen_buf, ' ');
            len++;
        }
    }
    s_push_str(&screen_buf, rstatus, rlen);
    s_push_cstr(&screen_buf, ANSI_RESET CRNL);

    /* Second row depends on editor.statusmsg and the status message update time. */
    s_push_cstr(&screen_buf, ANSI_ERASE_LINE_FROM_CURSOR);
    size_t msglen = strlen(editor.statusmsg);
    if (msglen && time(NULL)-editor.statusmsg_time < 3) // TODO: make 3 a variable, max seconds to show the msg
        s_push_str(&screen_buf, editor.statusmsg, msglen <= editor.screencols ? msglen : editor.screencols);
    log_this("message: `%s`", editor.statusmsg);

    /* Put cursor at its current position. Note that the horizontal position
     * at which the cursor is displayed may be different compared to 'editor.cx'
     * because of TABs. */
    size_t j;
    size_t cx = 1;
    filerow = editor.rowoff+editor.cy;
    row = (filerow >= editor.rows.count) ? NULL : GET_ROW(filerow);
    if (row) {
        size_t rowlen = strlen(row->content.items);
        for (j = editor.coloff; j < (editor.cx+editor.coloff); j++) {
            if (j < rowlen && row->content.items[j] == TAB) cx += 7-((cx)%8);
            cx++;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%zu;%zuH", editor.cy+1, cx);
    s_push_str(&screen_buf, buf, strlen(buf));
    log_this("(cx, cy): (%zu, %zu)", editor.cy+1, cx);

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
    editor_refresh_screen();
}

void editor_init()
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
    editor.pages = 0;
    editor.quit_times = QUIT_DEFAULT;

    memset(editor.statusmsg, 0, sizeof(editor.statusmsg));
    editor.statusmsg_time = (time_t)0;
    editor.statusmsg_max_time = (time_t)3;

    editor.N = N_DEFAULT;

    update_window_size();
    signal(SIGWINCH, handle_sigwinch);
}

void editor_open_file(char *filename)
{
    if (editor.filename) free(editor.filename);
    da_clear(&editor.rows);
    editor.pages = 0;

    if (filename == NULL) {
        editor.filename = strdup("new file");
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
        s_push_null(&row.content);
        da_push(&editor.rows, row);
    }
    editor.pages = editor.rows.count / editor.screenrows;
    free(line);
}

//void draw_rows()
//{
//    for (size_t i = 0; i < editor.screenrows; i++) {
//        if (i < editor.rows.count) {
//            Row *row = GET_ROW(editor.page*editor.screenrows+editor.rowoff+i);
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
//    else if (editor.rowoff == editor.rows.count-1) sprintf(perc_buf, "%s", "Bottom");
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
    //if (editor.cy == 0 && editor.page != 0) editor.rowoff--; // TODO: gestisci meglio
    if (editor.cy > N_OR_DEFAULT_VALUE(0)) editor.cy -= N_OR_DEFAULT_VALUE(1);
    else editor.cy = 0;
    //if (editor.rowoff > N_OR_DEFAULT_VALUE(0)) editor.rowoff -= N_OR_DEFAULT_VALUE(1);
    //else editor.rowoff = 0;
}

// TODO: now it's trivial
void move_cursor_down()
{ 
    //if ((editor.rowoff+N_OR_DEFAULT_VALUE(1) / editor.pages) > editor.page) editor.page += (editor.rowoff+N_OR_DEFAULT_VALUE(1)) / editor.pages; // TODO: No, non funziona cosi' devo modificare l'rowoff in questo caso
    //if (editor.cy == editor.screenrows-1 && editor.page != editor.pages-1) editor.rowoff++; // TODO: gestisci meglio
    if (editor.cy < editor.screenrows-N_OR_DEFAULT_VALUE(0)-1) editor.cy += N_OR_DEFAULT_VALUE(1); 
    else editor.cy = editor.screenrows-1;
    //if (editor.rowoff < editor.rows.count-N_OR_DEFAULT_VALUE(0)-1) editor.rowoff += N_OR_DEFAULT_VALUE(1); 
    //else editor.rowoff = editor.rows.count-1;
}

// TODO: now it's trivial
void move_cursor_left()
{
    if (editor.cx > N_OR_DEFAULT_VALUE(0)) editor.cx -= N_OR_DEFAULT_VALUE(1);
    else editor.cx = 0;
}

// TODO: now it's trivial
void move_cursor_right()
{
    if (editor.cx < editor.screencols-N_OR_DEFAULT_VALUE(0)-1) editor.cx += N_OR_DEFAULT_VALUE(1);
    else editor.cx = editor.screencols-1;
}

//void scroll_up() // TODO: aggiornare current row e cy
//{ 
//    if (editor.rowoff > N_OR_DEFAULT_VALUE(0)) editor.rowoff -= N_OR_DEFAULT_VALUE(1);
//}             

//void scroll_down() // TODO: aggiornare current row e cy
//{
//    if (editor.rowoff < editor.rows.count-N_OR_DEFAULT_VALUE(0)-1) editor.rowoff += N_OR_DEFAULT_VALUE(1);
//}

//void move_page_up()
//{
//    if (editor.page > N_OR_DEFAULT_VALUE(0)) editor.page -= N_OR_DEFAULT_VALUE(1);
//    else editor.page = 0;
//    editor.rowoff = editor.page*editor.pages + editor.cy;
//}             

//void move_page_down()
//{
//    if (editor.page < editor.pages-N_OR_DEFAULT_VALUE(0)-1) editor.page += N_OR_DEFAULT_VALUE(1);
//    else editor.page = editor.pages-1;
//    editor.rowoff = editor.page*editor.pages + editor.cy;
//}           

//void move_cursor_begin_of_screen()
//{
//    editor_set_statusmsg("TODO: move_cursor_begin_of_screen");
//}        

//void move_cursor_end_of_screen() 
//{
//    editor_set_statusmsg("TODO: move_cursor_end_of_screen");
//}

//void move_cursor_begin_of_file()
//{
//    editor.page = 0;
//    editor.rowoff = 0;
//    editor.cy = 0;
//}        

//void move_cursor_end_of_file() 
//{
//    editor.page = editor.pages-1;
//    editor.rowoff = editor.rows.count-1;
//    editor.cy = editor.screenrows-1;
//}

//void move_cursor_begin_of_line() { editor.cx = 0; }
//void move_cursor_end_of_line()   { editor.cx = editor.screencols-1; }  

//void move_cursor_first_non_space()
//{
//    Row *row = GET_CURR_ROW;
//    char *str = row->content.items;
//    while (*str != '\0' && isspace(*str)) {
//        editor.cx++;
//        str++;
//    }
//}

//void move_cursor_last_non_space()
//{
//    Row *row = GET_CURR_ROW;
//    char *str = row->content.items;
//    size_t len = strlen(str);
//    int i = len - 1;
//    while (i >= 0 && isspace(str[i]))
//        i--;
//    editor.cx = i;
//}

char *key_to_name(char key, char *buf)
{
    if (key == ESC)       return strcpy(buf, "ESC");
    if (key == 32)        return strcpy(buf, "SPACE");
    if (key == BACKSPACE) return strcpy(buf, "BACKSPACE");
    if (key == 16)        return strcpy(buf, "SHIFT?");

    if (isgraph((unsigned char) key)) sprintf(buf, "%c", key);
    else sprintf(buf, "%d", key);
    return strdup(buf);
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

//bool editor_save(void)
//{
//    int len;
//    char *buf = editorRowsToString(&len);
//    int fd = open(E.filename,O_RDWR|O_CREAT,0644);
//    if (fd == -1) goto writeerr;
//
//    /* Use truncate + a single write(2) call in order to make saving
//     * a bit safer, under the limits of what we can do in a small editor. */
//    if (ftruncate(fd,len) == -1) goto writeerr;
//    if (write(fd,buf,len) != len) goto writeerr;
//
//    close(fd);
//    free(buf);
//    E.dirty = 0;
//    editorSetStatusMessage("%d bytes written on disk", len);
//    return 0;
//
//writeerr:
//    free(buf);
//    if (fd != -1) close(fd);
//    editorSetStatusMessage("Can't save! I/O error: %s",strerror(errno));
//    return 1;
//}

bool editor_quit(void)
{
    if (editor.dirty && editor.quit_times) {
        editor_set_statusmsg("Session is not saved. If you really want to quit press CTRL-q %zu more time%s.", editor.quit_times, editor.quit_times == 1 ? "" : "s");
        editor.quit_times--;
        return false;
    }
    return true;
}

// TODO: process number as in process_key
void editor_process_key_press(void)
{
    int key = editor_read_key();

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
        editor_set_statusmsg(msg);
    } else {
        switch (key) {
            case 'j': move_cursor_down();  break;
            case 'k': move_cursor_up();    break;
            case 'h': move_cursor_left();  break;
            case 'l': move_cursor_right(); break;
            case ENTER:
                //editorInsertNewline();
                editor_set_statusmsg("TODO: ENTER");
                break;
            case CTRL_Q:
                if (editor_quit()) exit(0);
                else return;
            case CTRL_S:
                editor_set_statusmsg("TODO: SAVE");
                //editor_save();
                break;
            case BACKSPACE:
                editor_set_statusmsg("TODO: BACKSPACE");
                //editorDelChar();
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
                //case ARROW_UP:
                //case ARROW_DOWN:
                //case ARROW_LEFT:
                //case ARROW_RIGHT:
                //    editorMoveCursor(c);
                //    break;
                //case ESC:
                //    /* Nothing to do for ESC in this mode. */
                //    break;
            default:
                char key_buf[32];
                key_to_name(key, key_buf);
                editor_set_statusmsg("key: %s", key_buf);
                //editorInsertChar(c);
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
    editor_init();
    log_this("size = (%zu, %zu)", editor.screenrows, editor.screencols);
    editor_open_file(filename);
    log_this("Open file %s", editor.filename);
    enable_raw_mode();
    editor_set_statusmsg("Pronti per cominciare");

    while (true) {
        editor_refresh_screen();
        editor_process_key_press();
    }

    return 0;
}
