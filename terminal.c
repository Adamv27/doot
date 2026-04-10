#include "doot.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Suppress warn_unused_result for fire-and-forget writes to the terminal */
#pragma GCC diagnostic ignored "-Wunused-result"

/* ── Raw mode ─────────────────────────────────────────────────────── */

void term_disable_raw(void)
{
    if (!E.raw_mode_active)
        return;
    /* Restore main screen buffer */
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
    E.raw_mode_active = 0;
}

void term_enable_raw(void)
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
        perror("tcgetattr");
        exit(1);
    }
    atexit(term_disable_raw);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(unsigned)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(unsigned)(OPOST);
    raw.c_cflag |= (unsigned)(CS8);
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("tcsetattr");
        exit(1);
    }

    /* Switch to alternate screen buffer */
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
    E.raw_mode_active = 1;
}

/* ── Key reading ──────────────────────────────────────────────────── */

int term_read_key(void)
{
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0)
        return KEY_NONE;

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': return KEY_HOME;
                    case '3': return KEY_DELETE;
                    case '4': return KEY_END;
                    case '5': return KEY_PAGE_UP;
                    case '6': return KEY_PAGE_DOWN;
                    case '7': return KEY_HOME;
                    case '8': return KEY_END;
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return KEY_ARROW_UP;
                case 'B': return KEY_ARROW_DOWN;
                case 'C': return KEY_ARROW_RIGHT;
                case 'D': return KEY_ARROW_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            }
        }
        return '\x1b';
    }

    return (unsigned char)c;
}

/* ── Window size ──────────────────────────────────────────────────── */

void term_get_window_size(int *rows, int *cols)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col != 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return;
    }

    /* Fallback: move cursor to bottom-right and query position */
    write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12);
    write(STDOUT_FILENO, "\x1b[6n", 4);

    char buf[32];
    unsigned i = 0;
    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
            break;
        if (buf[i] == 'R')
            break;
        i++;
    }
    buf[i] = '\0';
    if (buf[0] == '\x1b' && buf[1] == '[')
        sscanf(&buf[2], "%d;%d", rows, cols);
}

/* ── Render buffer helpers ────────────────────────────────────────── */

void render_append(const char *s, size_t len)
{
    if (E.render_len + len > E.render_cap) {
        size_t new_cap = E.render_cap * 2;
        if (new_cap < E.render_len + len)
            new_cap = E.render_len + len + 4096;
        E.render_buf = realloc(E.render_buf, new_cap);
        E.render_cap = new_cap;
    }
    memcpy(E.render_buf + E.render_len, s, len);
    E.render_len += len;
}

void render_appendf(const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0)
        render_append(tmp, (size_t)n);
}

/* ── Visual selection helpers ──────────────────────────────────────── */

/* Check if column 'col' on 'line' is within the visual selection */
static int is_selected(size_t line, size_t col)
{
    if (E.mode != MODE_VISUAL && E.mode != MODE_VISUAL_LINE)
        return 0;

    if (E.mode == MODE_VISUAL_LINE) {
        size_t sl = E.visual_anchor_line, el = E.cursor_line;
        if (sl > el) { size_t t = sl; sl = el; el = t; }
        return line >= sl && line <= el;
    }

    /* Character-wise visual */
    size_t s_line, s_col, e_line, e_col;
    editor_visual_range(&s_line, &s_col, &e_line, &e_col);

    if (line < s_line || line > e_line) return 0;
    if (s_line == e_line)
        return col >= s_col && col <= e_col;
    if (line == s_line) return col >= s_col;
    if (line == e_line) return col <= e_col;
    return 1; /* middle line — fully selected */
}

/* ── Screen rendering ─────────────────────────────────────────────── */

void render_screen(void)
{
    E.render_len = 0;

    /* Hide cursor */
    render_append("\x1b[?25l", 6);
    /* Move to top-left */
    render_append("\x1b[H", 3);

    int file_rows = E.screen_rows - 2; /* reserve 2 rows: status bar + command line */
    if (file_rows < 1) file_rows = 1;

    int in_visual = (E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE);
    int text_cols = E.screen_cols - GUTTER_WIDTH;
    if (text_cols < 1) text_cols = 1;

    for (int y = 0; y < file_rows; y++) {
        size_t line_num = E.row_offset + (size_t)y;
        char line_buf[4096];
        size_t line_len = lcache_get_line_content(
            &E.lcache, &E.buf, line_num, line_buf, sizeof(line_buf));

        if (line_len == (size_t)-1) {
            /* Past end of file — show relative line number in gutter */
            size_t rel = (line_num > E.cursor_line)
                ? line_num - E.cursor_line
                : E.cursor_line - line_num;
            render_appendf("\x1b[2m%4zu\x1b[m ", rel);

            /* Logo on empty-file welcome screen */
            static const char *logo[] = {
                "  _____   ____   ____ _______ ",
                " |  __ \\ / __ \\ / __ \\__   __|",
                " | |  | | |  | | |  | | | |   ",
                " | |  | | |  | | |  | | | |   ",
                " | |__| | |__| | |__| | | |   ",
                " |_____/ \\____/ \\____/  |_|   ",
                "                               ",
                "                               ",
            };
            static const int logo_lines = 8;
            int logo_start = file_rows / 3 - logo_lines / 2;
            if (logo_start < 0) logo_start = 0;

            if (E.buf.total_bytes == 0 && y >= logo_start && y < logo_start + logo_lines) {
                const char *art = logo[y - logo_start];
                size_t alen = strlen(art);
                size_t padding = ((size_t)text_cols > alen)
                    ? ((size_t)text_cols - alen) / 2 : 0;
                for (size_t p = 0; p < padding; p++)
                    render_append(" ", 1);
                render_append(art, alen);
            }
        } else {
            /* Render line number gutter */
            size_t rel = (line_num > E.cursor_line)
                ? line_num - E.cursor_line
                : E.cursor_line - line_num;

            /* Dim color for relative numbers, bold for current line */
            if (line_num == E.cursor_line) {
                render_appendf("\x1b[1m%4zu\x1b[m ", line_num + 1);
            } else {
                render_appendf("\x1b[2m%4zu\x1b[m ", rel);
            }

            if (!in_visual) {
                /* No visual mode — fast path, no per-char highlighting */
                if (E.col_offset < line_len) {
                    size_t visible_len = line_len - E.col_offset;
                    if (visible_len > (size_t)text_cols)
                        visible_len = (size_t)text_cols;
                    render_append(line_buf + E.col_offset, visible_len);
                }
            } else {
                /* Visual mode — render char by char with selection highlighting */
                int in_sel = 0;
                size_t visible_start = E.col_offset;
                size_t visible_end = E.col_offset + (size_t)text_cols;
                if (visible_end > line_len) visible_end = line_len;

                for (size_t c = visible_start; c < visible_end; c++) {
                    int sel = is_selected(line_num, c);
                    if (sel && !in_sel) {
                        render_append("\x1b[7m", 4); /* reverse video */
                        in_sel = 1;
                    } else if (!sel && in_sel) {
                        render_append("\x1b[m", 3);
                        in_sel = 0;
                    }
                    render_append(&line_buf[c], 1);
                }
                if (in_sel)
                    render_append("\x1b[m", 3);
            }
        }

        /* Clear to end of line and newline */
        render_append("\x1b[K\r\n", 5);
    }

    /* Status bar (reverse video) */
    render_append("\x1b[7m", 4);

    const char *mode_str = " NORMAL";
    switch (E.mode) {
    case MODE_NORMAL:      mode_str = " NORMAL"; break;
    case MODE_INSERT:      mode_str = " INSERT"; break;
    case MODE_VISUAL:      mode_str = " VISUAL"; break;
    case MODE_VISUAL_LINE: mode_str = " V-LINE"; break;
    case MODE_COMMAND:     mode_str = " NORMAL"; break;
    }

    char left[256];
    int left_len = snprintf(left, sizeof(left), "%s | %.50s%s",
        mode_str,
        E.buf.filename ? E.buf.filename : "[new]",
        E.buf.dirty ? " [+]" : "");

    char right[64];
    int right_len = snprintf(right, sizeof(right), "%zu:%zu ",
        E.cursor_line + 1, E.cursor_col + 1);

    if (left_len > E.screen_cols)
        left_len = E.screen_cols;
    render_append(left, (size_t)left_len);

    int spaces = E.screen_cols - left_len - right_len;
    for (int i = 0; i < spaces; i++)
        render_append(" ", 1);
    if (spaces + left_len + right_len <= E.screen_cols)
        render_append(right, (size_t)right_len);

    render_append("\x1b[m\r\n", 5); /* reset + newline to command row */

    /* Command / message line (bottom row) */
    if (E.mode == MODE_COMMAND) {
        /* Show the command buffer being typed */
        char prefix = E.cmd_buf[0];
        if (prefix == '/' || prefix == '?') {
            /* Search prompt */
            render_append(&prefix, 1);
            if (E.cmd_len > 1)
                render_append(E.cmd_buf + 1, E.cmd_len - 1);
        } else {
            render_append(":", 1);
            render_append(E.cmd_buf, E.cmd_len);
        }
    } else if (E.status_msg[0] != '\0' && time(NULL) - E.status_msg_time < 5) {
        render_append(E.status_msg, strlen(E.status_msg));
    }
    render_append("\x1b[K", 3);

    /* Position cursor */
    if (E.mode == MODE_COMMAND) {
        /* Cursor on the command line */
        size_t cmd_cursor;
        if (E.cmd_buf[0] == '/' || E.cmd_buf[0] == '?')
            cmd_cursor = E.cmd_len; /* after the prefix char + typed text */
        else
            cmd_cursor = E.cmd_len + 1; /* after ':' + typed text */
        render_appendf("\x1b[%d;%zuH", E.screen_rows, cmd_cursor + 1);
    } else {
        render_appendf("\x1b[%zu;%zuH",
            E.cursor_line - E.row_offset + 1,
            E.cursor_col - E.col_offset + 1 + GUTTER_WIDTH);
    }

    /* Show cursor */
    render_append("\x1b[?25h", 6);

    /* Single write */
    write(STDOUT_FILENO, E.render_buf, E.render_len);
}
