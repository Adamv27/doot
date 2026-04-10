#include "doot.h"

#include <ctype.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Global editor state */
EditorState E;

/* ── Status message ───────────────────────────────────────────────── */

void editor_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
    E.status_msg_time = time(NULL);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Effective repeat count: at least 1 */
static int vim_count(void)
{
    return E.count > 0 ? E.count : 1;
}

static void vim_reset(void)
{
    E.pending_op = OP_NONE;
    E.g_pending = 0;
    E.count = 0;
    E.count_active = 0;
}

/* Save undo snapshot if one hasn't been saved for the current edit group.
   Call this before the first buffer mutation in a command or insert session. */
static void undo_save(void)
{
    if (!E.undo_saved) {
        undo_push(&E.undo, &E.buf, E.cursor_line, E.cursor_col);
        undo_clear(&E.redo);
        E.undo_saved = 1;
    }
}

/* Clamp cursor column to line length. In normal mode, clamp to len-1
   (can't sit past the last char), in insert mode clamp to len. */
static void clamp_cursor(void)
{
    size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
    if (E.mode == MODE_NORMAL || E.mode == MODE_VISUAL || E.mode == MODE_VISUAL_LINE) {
        if (len > 0 && E.cursor_col >= len)
            E.cursor_col = len - 1;
        if (len == 0)
            E.cursor_col = 0;
    } else {
        if (E.cursor_col > len)
            E.cursor_col = len;
    }
}

/* Is the given line valid (exists in the buffer)? */
static int line_exists(size_t line)
{
    return lcache_find_line_offset(&E.lcache, &E.buf, line) != (size_t)-1;
}

/* ── Scrolling ────────────────────────────────────────────────────── */

static void editor_scroll(void)
{
    int file_rows = E.screen_rows - 2; /* status bar + command/message line */
    if (file_rows < 1) file_rows = 1;

    if (E.cursor_line < E.row_offset)
        E.row_offset = E.cursor_line;
    if (E.cursor_line >= E.row_offset + (size_t)file_rows)
        E.row_offset = E.cursor_line - (size_t)file_rows + 1;
    int text_cols = E.screen_cols - GUTTER_WIDTH;
    if (text_cols < 1) text_cols = 1;
    if (E.cursor_col < E.col_offset)
        E.col_offset = E.cursor_col;
    if (E.cursor_col >= E.col_offset + (size_t)text_cols)
        E.col_offset = E.cursor_col - (size_t)text_cols + 1;
}

/* ── Visual mode range ────────────────────────────────────────────── */

void editor_visual_range(size_t *start_line, size_t *start_col,
                         size_t *end_line, size_t *end_col)
{
    size_t al = E.visual_anchor_line, ac = E.visual_anchor_col;
    size_t cl = E.cursor_line, cc = E.cursor_col;

    if (al < cl || (al == cl && ac <= cc)) {
        *start_line = al; *start_col = ac;
        *end_line = cl;   *end_col = cc;
    } else {
        *start_line = cl; *start_col = cc;
        *end_line = al;   *end_col = ac;
    }
}

/* ── Yank register ────────────────────────────────────────────────── */

static void yank_store(const char *data, size_t len, int linewise)
{
    free(E.yank_buf);
    E.yank_buf = malloc(len);
    memcpy(E.yank_buf, data, len);
    E.yank_len = len;
    E.yank_linewise = linewise;
}

/* Yank a range of bytes from the buffer */
static void yank_range(size_t from, size_t to, int linewise)
{
    if (to <= from) return;
    size_t len = to - from;
    char *tmp = malloc(len);
    buf_read_range(&E.buf, from, len, tmp);
    yank_store(tmp, len, linewise);
    free(tmp);
}

/* ── Word motion helpers ──────────────────────────────────────────── */

static int is_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

/* Move forward to start of next word. Returns new byte offset. */
static size_t word_forward(size_t pos)
{
    if (pos >= E.buf.total_bytes) return pos;
    char c = buf_byte_at(&E.buf, pos);
    /* Skip current word chars or current non-word non-space chars */
    if (is_word_char(c)) {
        while (pos < E.buf.total_bytes && is_word_char(buf_byte_at(&E.buf, pos)))
            pos++;
    } else if (!isspace((unsigned char)c)) {
        while (pos < E.buf.total_bytes && !is_word_char(buf_byte_at(&E.buf, pos))
               && !isspace((unsigned char)buf_byte_at(&E.buf, pos)))
            pos++;
    }
    /* Skip whitespace */
    while (pos < E.buf.total_bytes && isspace((unsigned char)buf_byte_at(&E.buf, pos)))
        pos++;
    return pos;
}

/* Move backward to start of previous word. */
static size_t word_backward(size_t pos)
{
    if (pos == 0) return 0;
    pos--;
    /* Skip whitespace backward */
    while (pos > 0 && isspace((unsigned char)buf_byte_at(&E.buf, pos)))
        pos--;
    if (pos == 0 && isspace((unsigned char)buf_byte_at(&E.buf, 0)))
        return 0;
    /* Skip word chars or non-word non-space chars backward */
    char c = buf_byte_at(&E.buf, pos);
    if (is_word_char(c)) {
        while (pos > 0 && is_word_char(buf_byte_at(&E.buf, pos - 1)))
            pos--;
    } else {
        while (pos > 0 && !is_word_char(buf_byte_at(&E.buf, pos - 1))
               && !isspace((unsigned char)buf_byte_at(&E.buf, pos - 1)))
            pos--;
    }
    return pos;
}

/* Move forward to end of word. */
static size_t word_end(size_t pos)
{
    if (pos >= E.buf.total_bytes) return pos;
    pos++;
    /* Skip whitespace */
    while (pos < E.buf.total_bytes && isspace((unsigned char)buf_byte_at(&E.buf, pos)))
        pos++;
    if (pos >= E.buf.total_bytes) return E.buf.total_bytes > 0 ? E.buf.total_bytes - 1 : 0;
    /* Skip word chars or non-word non-space chars */
    char c = buf_byte_at(&E.buf, pos);
    if (is_word_char(c)) {
        while (pos + 1 < E.buf.total_bytes && is_word_char(buf_byte_at(&E.buf, pos + 1)))
            pos++;
    } else {
        while (pos + 1 < E.buf.total_bytes && !is_word_char(buf_byte_at(&E.buf, pos + 1))
               && !isspace((unsigned char)buf_byte_at(&E.buf, pos + 1)))
            pos++;
    }
    return pos;
}

/* Convert byte offset to line/col */
static void byte_to_linecol(size_t byte_pos, size_t *line, size_t *col)
{
    /* Scan from beginning counting newlines. We use the line cache. */
    size_t l = 0;
    size_t line_start = 0;
    /* Try to use binary search on line cache for efficiency */
    for (;;) {
        size_t next_off = lcache_find_line_offset(&E.lcache, &E.buf, l + 1);
        if (next_off == (size_t)-1 || next_off > byte_pos) {
            /* byte_pos is on line l */
            size_t this_off = lcache_find_line_offset(&E.lcache, &E.buf, l);
            if (this_off == (size_t)-1) this_off = 0;
            *line = l;
            *col = byte_pos - this_off;
            return;
        }
        line_start = next_off;
        l++;
    }
    (void)line_start;
}

/* ── Search ───────────────────────────────────────────────────────── */

static int search_forward(size_t from, size_t *result_byte)
{
    size_t qlen = strlen(E.search_pattern);
    if (qlen == 0) return 0;

    char buf[4096];
    /* Search from 'from' to end, then wrap from 0 to 'from' */
    for (int pass = 0; pass < 2; pass++) {
        size_t start = (pass == 0) ? from : 0;
        size_t end = (pass == 0) ? E.buf.total_bytes : from;
        size_t pos = start;

        while (pos < end) {
            size_t chunk = sizeof(buf);
            if (chunk > end - pos) chunk = end - pos;
            size_t got = buf_read_range(&E.buf, pos, chunk, buf);
            if (got == 0) break;
            for (size_t i = 0; i + qlen <= got; i++) {
                if (memcmp(buf + i, E.search_pattern, qlen) == 0) {
                    *result_byte = pos + i;
                    return 1;
                }
            }
            if (got > qlen)
                pos += got - qlen + 1;
            else
                pos += got;
        }
    }
    return 0;
}

static int search_backward(size_t from, size_t *result_byte)
{
    size_t qlen = strlen(E.search_pattern);
    if (qlen == 0) return 0;

    /* Simple approach: search backward by scanning forward in chunks,
       keeping track of the last match before 'from'. Do two passes. */
    for (int pass = 0; pass < 2; pass++) {
        size_t region_start = (pass == 0) ? 0 : from;
        size_t region_end = (pass == 0) ? from : E.buf.total_bytes;

        size_t last_match = (size_t)-1;
        size_t pos = region_start;
        char buf[4096];

        while (pos < region_end) {
            size_t chunk = sizeof(buf);
            if (chunk > region_end - pos) chunk = region_end - pos;
            size_t got = buf_read_range(&E.buf, pos, chunk, buf);
            if (got == 0) break;
            for (size_t i = 0; i + qlen <= got; i++) {
                if (memcmp(buf + i, E.search_pattern, qlen) == 0)
                    last_match = pos + i;
            }
            if (got > qlen)
                pos += got - qlen + 1;
            else
                pos += got;
        }

        if (last_match != (size_t)-1) {
            *result_byte = last_match;
            return 1;
        }
    }
    return 0;
}

static void do_search_next(int direction)
{
    if (E.search_pattern[0] == '\0') {
        editor_set_status("No previous search");
        return;
    }
    size_t cur = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
    size_t result;
    int found;
    if (direction == 1)
        found = search_forward(cur + 1, &result);
    else
        found = search_backward(cur > 0 ? cur - 1 : E.buf.total_bytes, &result);

    if (found) {
        byte_to_linecol(result, &E.cursor_line, &E.cursor_col);
        clamp_cursor();
    } else {
        editor_set_status("Pattern not found: %s", E.search_pattern);
    }
}

/* ── Operator execution (delete/change/yank over a range) ─────────── */

static void execute_op_range(VimOperator op, size_t from_byte, size_t to_byte)
{
    if (to_byte <= from_byte) return;
    size_t len = to_byte - from_byte;

    /* Always yank */
    yank_range(from_byte, to_byte, 0);

    if (op == OP_YANK) {
        editor_set_status("%zu bytes yanked", len);
        return;
    }

    undo_save();
    lcache_invalidate_from(&E.lcache, from_byte);
    buf_delete(&E.buf, from_byte, len);

    byte_to_linecol(from_byte < E.buf.total_bytes ? from_byte :
                    (E.buf.total_bytes > 0 ? E.buf.total_bytes - 1 : 0),
                    &E.cursor_line, &E.cursor_col);
    clamp_cursor();

    if (op == OP_CHANGE)
        E.mode = MODE_INSERT;
}

static void execute_op_lines(VimOperator op, size_t first_line, size_t last_line)
{
    size_t from = lcache_find_line_offset(&E.lcache, &E.buf, first_line);
    if (from == (size_t)-1) return;
    size_t to;
    size_t next = lcache_find_line_offset(&E.lcache, &E.buf, last_line + 1);
    if (next == (size_t)-1)
        to = E.buf.total_bytes;
    else
        to = next;

    if (to <= from) return;

    /* Yank as linewise */
    yank_range(from, to, 1);

    if (op == OP_YANK) {
        size_t n = last_line - first_line + 1;
        editor_set_status("%zu line%s yanked", n, n > 1 ? "s" : "");
        return;
    }

    undo_save();
    lcache_invalidate_from(&E.lcache, from);
    buf_delete(&E.buf, from, to - from);

    E.cursor_line = first_line;
    if (!line_exists(E.cursor_line) && E.cursor_line > 0)
        E.cursor_line--;
    E.cursor_col = 0;
    clamp_cursor();

    if (op == OP_CHANGE) {
        E.cursor_col = 0;
        E.mode = MODE_INSERT;
    }
}

/* ── Prompt (for : and / commands) ────────────────────────────────── */

/* This is called from render loop — we handle it differently now.
   Command-line input is handled in MODE_COMMAND with E.cmd_buf. */

/* ── Insert mode key handling ─────────────────────────────────────── */

static void handle_insert(int key)
{
    undo_save();

    switch (key) {
    case '\x1b': /* Escape — back to normal mode */
        E.mode = MODE_NORMAL;
        if (E.cursor_col > 0)
            E.cursor_col--;
        clamp_cursor();
        return;

    case '\r': {
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        lcache_invalidate_from(&E.lcache, byte_pos);
        buf_insert(&E.buf, byte_pos, "\n", 1);
        E.cursor_line++;
        E.cursor_col = 0;
        return;
    }

    case KEY_BACKSPACE:
    case CTRL_KEY('h'): {
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        if (byte_pos == 0) return;
        char c = buf_byte_at(&E.buf, byte_pos - 1);
        lcache_invalidate_from(&E.lcache, byte_pos - 1);
        buf_delete(&E.buf, byte_pos - 1, 1);
        if (c == '\n') {
            E.cursor_line--;
            E.cursor_col = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        } else {
            E.cursor_col--;
        }
        return;
    }

    case KEY_DELETE: {
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        if (byte_pos >= E.buf.total_bytes) return;
        lcache_invalidate_from(&E.lcache, byte_pos);
        buf_delete(&E.buf, byte_pos, 1);
        return;
    }

    case KEY_ARROW_UP:
        if (E.cursor_line > 0) E.cursor_line--;
        clamp_cursor();
        return;
    case KEY_ARROW_DOWN:
        if (line_exists(E.cursor_line + 1)) E.cursor_line++;
        clamp_cursor();
        return;
    case KEY_ARROW_LEFT:
        if (E.cursor_col > 0) E.cursor_col--;
        return;
    case KEY_ARROW_RIGHT: {
        size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        if (E.cursor_col < len) E.cursor_col++;
        return;
    }

    case KEY_HOME:
        E.cursor_col = 0;
        return;
    case KEY_END:
        E.cursor_col = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        return;

    default:
        if (key >= 32 && key < 127) {
            char c = (char)key;
            size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
            lcache_invalidate_from(&E.lcache, byte_pos);
            buf_insert(&E.buf, byte_pos, &c, 1);
            E.cursor_col++;
        }
        return;
    }
}

/* ── Command-line mode (:, /, ?) ──────────────────────────────────── */

static void execute_command(void)
{
    char *cmd = E.cmd_buf;

    /* :w — write */
    if (strcmp(cmd, "w") == 0) {
        if (E.buf.filename == NULL) {
            editor_set_status("No filename");
        } else if (buf_save(&E.buf) == 0) {
            editor_set_status("\"%s\" written, %zu bytes", E.buf.filename, E.buf.total_bytes);
        } else {
            editor_set_status("Error writing file!");
        }
    }
    /* :w <filename> */
    else if (strncmp(cmd, "w ", 2) == 0 && cmd[2] != '\0') {
        free(E.buf.filename);
        E.buf.filename = strdup(cmd + 2);
        if (buf_save(&E.buf) == 0) {
            editor_set_status("\"%s\" written, %zu bytes", E.buf.filename, E.buf.total_bytes);
        } else {
            editor_set_status("Error writing file!");
        }
    }
    /* :q — quit */
    else if (strcmp(cmd, "q") == 0) {
        if (E.buf.dirty) {
            editor_set_status("No write since last change (add ! to override)");
        } else {
            term_disable_raw();
            exit(0);
        }
    }
    /* :q! — force quit */
    else if (strcmp(cmd, "q!") == 0) {
        term_disable_raw();
        exit(0);
    }
    /* :wq — write and quit */
    else if (strcmp(cmd, "wq") == 0) {
        if (E.buf.filename == NULL) {
            editor_set_status("No filename");
        } else if (buf_save(&E.buf) == 0) {
            term_disable_raw();
            exit(0);
        } else {
            editor_set_status("Error writing file!");
        }
    }
    /* :x — like :wq but only writes if modified */
    else if (strcmp(cmd, "x") == 0) {
        if (E.buf.dirty) {
            if (E.buf.filename == NULL) {
                editor_set_status("No filename");
                return;
            }
            if (buf_save(&E.buf) != 0) {
                editor_set_status("Error writing file!");
                return;
            }
        }
        term_disable_raw();
        exit(0);
    }
    /* :<number> — goto line */
    else if (cmd[0] >= '0' && cmd[0] <= '9') {
        long line = strtol(cmd, NULL, 10);
        if (line < 1) {
            editor_set_status("Invalid line number");
        } else {
            size_t target = (size_t)(line - 1);
            if (line_exists(target)) {
                E.cursor_line = target;
                E.cursor_col = 0;
                clamp_cursor();
            } else {
                editor_set_status("Line %ld not found", line);
            }
        }
    }
    else {
        editor_set_status("Unknown command: %s", cmd);
    }
}

static void handle_command(int key)
{
    if (key == '\x1b') {
        E.mode = MODE_NORMAL;
        E.cmd_len = 0;
        editor_set_status("");
        return;
    }

    if (key == '\r') {
        E.cmd_buf[E.cmd_len] = '\0';
        E.mode = MODE_NORMAL;

        /* Check if this was a search prompt */
        if (E.cmd_buf[0] == '/' || E.cmd_buf[0] == '?') {
            char dir = E.cmd_buf[0];
            E.search_direction = (dir == '/') ? 1 : -1;
            if (E.cmd_len > 1) {
                strncpy(E.search_pattern, E.cmd_buf + 1, sizeof(E.search_pattern) - 1);
                E.search_pattern[sizeof(E.search_pattern) - 1] = '\0';
            }
            if (E.search_pattern[0] != '\0') {
                do_search_next(E.search_direction);
            }
        } else {
            execute_command();
        }

        E.cmd_len = 0;
        return;
    }

    if (key == KEY_BACKSPACE || key == CTRL_KEY('h')) {
        if (E.cmd_len > 0) {
            E.cmd_len--;
        } else {
            E.mode = MODE_NORMAL;
            editor_set_status("");
        }
        return;
    }

    if (key >= 32 && key < 127 && E.cmd_len < sizeof(E.cmd_buf) - 2) {
        E.cmd_buf[E.cmd_len++] = (char)key;
    }
}

/* ── Normal mode motions ──────────────────────────────────────────── */

/* Apply a motion and return the byte range [from, to).
   Returns 0 on success, -1 if motion is invalid.
   Moves the cursor as a side effect. */

static int do_motion(int key, size_t *from_byte, size_t *to_byte)
{
    int n = vim_count();
    size_t before_line = E.cursor_line;
    size_t before_col = E.cursor_col;
    size_t before_byte = buf_byte_offset(&E.buf, before_line, before_col);

    switch (key) {
    case 'h':
    case KEY_ARROW_LEFT:
        for (int i = 0; i < n && E.cursor_col > 0; i++)
            E.cursor_col--;
        break;

    case 'l':
    case KEY_ARROW_RIGHT: {
        size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        size_t max_col = (E.mode == MODE_INSERT) ? len : (len > 0 ? len - 1 : 0);
        for (int i = 0; i < n && E.cursor_col < max_col; i++)
            E.cursor_col++;
        break;
    }

    case 'j':
    case KEY_ARROW_DOWN:
        for (int i = 0; i < n; i++) {
            if (line_exists(E.cursor_line + 1))
                E.cursor_line++;
        }
        clamp_cursor();
        break;

    case 'k':
    case KEY_ARROW_UP:
        for (int i = 0; i < n; i++) {
            if (E.cursor_line > 0)
                E.cursor_line--;
        }
        clamp_cursor();
        break;

    case 'w': {
        size_t pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        for (int i = 0; i < n; i++)
            pos = word_forward(pos);
        byte_to_linecol(pos < E.buf.total_bytes ? pos :
                        (E.buf.total_bytes > 0 ? E.buf.total_bytes - 1 : 0),
                        &E.cursor_line, &E.cursor_col);
        clamp_cursor();
        break;
    }

    case 'b': {
        size_t pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        for (int i = 0; i < n; i++)
            pos = word_backward(pos);
        byte_to_linecol(pos, &E.cursor_line, &E.cursor_col);
        clamp_cursor();
        break;
    }

    case 'e': {
        size_t pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        for (int i = 0; i < n; i++)
            pos = word_end(pos);
        byte_to_linecol(pos, &E.cursor_line, &E.cursor_col);
        clamp_cursor();
        break;
    }

    case '0':
        if (E.count_active) return -1; /* digit, not motion */
        E.cursor_col = 0;
        break;

    case '^': {
        /* First non-blank character of line */
        char line_buf[4096];
        size_t len = lcache_get_line_content(&E.lcache, &E.buf, E.cursor_line,
                                              line_buf, sizeof(line_buf));
        E.cursor_col = 0;
        if (len != (size_t)-1) {
            for (size_t i = 0; i < len; i++) {
                if (!isspace((unsigned char)line_buf[i])) {
                    E.cursor_col = i;
                    break;
                }
            }
        }
        break;
    }

    case '$':
    case KEY_END: {
        for (int i = 1; i < n; i++) {
            if (line_exists(E.cursor_line + 1))
                E.cursor_line++;
        }
        size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        E.cursor_col = len > 0 ? len - 1 : 0;
        if (E.mode == MODE_INSERT || E.pending_op != OP_NONE)
            E.cursor_col = len;
        break;
    }

    case KEY_HOME:
        E.cursor_col = 0;
        break;

    case 'G': {
        /* Go to line N, or last line if no count */
        if (E.count > 0) {
            size_t target = (size_t)(E.count - 1);
            if (line_exists(target))
                E.cursor_line = target;
        } else {
            /* Find last line */
            size_t l = E.cursor_line;
            while (line_exists(l + 1)) l++;
            E.cursor_line = l;
        }
        E.cursor_col = 0;
        clamp_cursor();
        break;
    }

    case KEY_PAGE_DOWN:
    case CTRL_KEY('f'): {
        size_t rows = (size_t)(E.screen_rows - 2);
        for (size_t i = 0; i < rows; i++) {
            if (line_exists(E.cursor_line + 1))
                E.cursor_line++;
        }
        clamp_cursor();
        break;
    }

    case KEY_PAGE_UP:
    case CTRL_KEY('b'): {
        size_t rows = (size_t)(E.screen_rows - 2);
        for (size_t i = 0; i < rows && E.cursor_line > 0; i++)
            E.cursor_line--;
        clamp_cursor();
        break;
    }

    case CTRL_KEY('d'): {
        size_t rows = (size_t)(E.screen_rows - 2) / 2;
        for (size_t i = 0; i < rows; i++) {
            if (line_exists(E.cursor_line + 1))
                E.cursor_line++;
        }
        clamp_cursor();
        break;
    }

    case CTRL_KEY('u'): {
        size_t rows = (size_t)(E.screen_rows - 2) / 2;
        for (size_t i = 0; i < rows && E.cursor_line > 0; i++)
            E.cursor_line--;
        clamp_cursor();
        break;
    }

    default:
        return -1; /* not a recognized motion */
    }

    size_t after_byte = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
    if (before_byte <= after_byte) {
        *from_byte = before_byte;
        *to_byte = after_byte;
    } else {
        *from_byte = after_byte;
        *to_byte = before_byte;
    }
    return 0;
}

/* ── Normal mode ──────────────────────────────────────────────────── */

static void handle_normal(int key)
{
    E.undo_saved = 0;

    /* Accumulate count prefix */
    if (key >= '1' && key <= '9' && E.pending_op == OP_NONE) {
        E.count = E.count * 10 + (key - '0');
        E.count_active = 1;
        return;
    }
    if (key == '0' && E.count_active) {
        E.count = E.count * 10;
        return;
    }

    /* Handle 'g' prefix — second key arrives here */
    if (E.g_pending) {
        E.g_pending = 0;
        if (key == 'g') {
            size_t target = E.count > 0 ? (size_t)(E.count - 1) : 0;
            if (!line_exists(target)) target = 0;

            if (E.pending_op != OP_NONE) {
                /* Operator + gg: operate on lines from target to cursor */
                size_t first = target < E.cursor_line ? target : E.cursor_line;
                size_t last = target > E.cursor_line ? target : E.cursor_line;
                execute_op_lines(E.pending_op, first, last);
            } else {
                /* Plain gg: jump to line */
                E.cursor_line = target;
                E.cursor_col = 0;
                clamp_cursor();
            }
        }
        vim_reset();
        return;
    }

    /* If an operator is pending, the next key is a motion or a doubled-op */
    if (E.pending_op != OP_NONE) {
        /* Doubled operator: dd, cc, yy — act on N lines */
        if ((key == 'd' && E.pending_op == OP_DELETE) ||
            (key == 'c' && E.pending_op == OP_CHANGE) ||
            (key == 'y' && E.pending_op == OP_YANK)) {
            int n = vim_count();
            size_t first = E.cursor_line;
            size_t last = first + (size_t)n - 1;
            while (!line_exists(last) && last > first) last--;
            execute_op_lines(E.pending_op, first, last);
            vim_reset();
            return;
        }

        /* Operator + g prefix — wait for second key */
        if (key == 'g') {
            E.g_pending = 1;
            return;
        }

        /* Operator + motion */
        size_t from, to;
        if (do_motion(key, &from, &to) == 0) {
            /* For motions like w/e that should include the char under cursor
               when used with an operator, add 1 to 'to' */
            if (key == 'w' || key == 'e') {
                if (to < E.buf.total_bytes) to++;
            }
            execute_op_range(E.pending_op, from, to);
            vim_reset();

            /* Restore cursor to 'from' for delete/change */
            if (E.pending_op != OP_YANK) {
                byte_to_linecol(from < E.buf.total_bytes ? from :
                                (E.buf.total_bytes > 0 ? E.buf.total_bytes - 1 : 0),
                                &E.cursor_line, &E.cursor_col);
            }
            /* pending_op already reset above, but the op var is stale. ok. */
            clamp_cursor();
        } else {
            editor_set_status("Unknown motion");
            vim_reset();
        }
        return;
    }

    /* Try as a plain motion first */
    {
        size_t from, to;
        size_t save_line = E.cursor_line;
        size_t save_col = E.cursor_col;
        if (do_motion(key, &from, &to) == 0) {
            /* Motion applied — cursor already moved */
            vim_reset();
            return;
        }
        /* Restore if motion failed (key wasn't a motion) */
        E.cursor_line = save_line;
        E.cursor_col = save_col;
    }

    int n = vim_count();

    switch (key) {
    /* ── Operators ── */
    case 'd':
        E.pending_op = OP_DELETE;
        return; /* wait for motion */
    case 'c':
        E.pending_op = OP_CHANGE;
        return;
    case 'y':
        E.pending_op = OP_YANK;
        return;

    /* ── g prefix — wait for second key ── */
    case 'g':
        E.g_pending = 1;
        return;

    /* ── Insert mode entry ── */
    case 'i':
        E.mode = MODE_INSERT;
        vim_reset();
        return;

    case 'I':
        E.mode = MODE_INSERT;
        E.cursor_col = 0;
        /* Move to first non-blank */
        {
            char line_buf[4096];
            size_t len = lcache_get_line_content(&E.lcache, &E.buf, E.cursor_line,
                                                  line_buf, sizeof(line_buf));
            if (len != (size_t)-1) {
                for (size_t i = 0; i < len; i++) {
                    if (!isspace((unsigned char)line_buf[i])) {
                        E.cursor_col = i;
                        break;
                    }
                }
            }
        }
        vim_reset();
        return;

    case 'a': {
        E.mode = MODE_INSERT;
        size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        if (E.cursor_col < len)
            E.cursor_col++;
        vim_reset();
        return;
    }

    case 'A': {
        E.mode = MODE_INSERT;
        E.cursor_col = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
        vim_reset();
        return;
    }

    case 'o': {
        /* Open line below */
        undo_save();
        E.mode = MODE_INSERT;
        size_t eol = buf_byte_offset(&E.buf, E.cursor_line,
            lcache_line_length(&E.lcache, &E.buf, E.cursor_line));
        lcache_invalidate_from(&E.lcache, eol);
        buf_insert(&E.buf, eol, "\n", 1);
        E.cursor_line++;
        E.cursor_col = 0;
        vim_reset();
        return;
    }

    case 'O': {
        /* Open line above */
        undo_save();
        E.mode = MODE_INSERT;
        size_t bol = buf_byte_offset(&E.buf, E.cursor_line, 0);
        lcache_invalidate_from(&E.lcache, bol);
        buf_insert(&E.buf, bol, "\n", 1);
        E.cursor_col = 0;
        /* cursor_line stays the same — the new line is at cursor_line,
           the old line shifted down */
        vim_reset();
        return;
    }

    /* ── x — delete char under cursor ── */
    case 'x': {
        undo_save();
        for (int i = 0; i < n; i++) {
            size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
            if (byte_pos >= E.buf.total_bytes) break;
            char c = buf_byte_at(&E.buf, byte_pos);
            if (c == '\n') break;
            yank_store(&c, 1, 0);
            lcache_invalidate_from(&E.lcache, byte_pos);
            buf_delete(&E.buf, byte_pos, 1);
        }
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── X — delete char before cursor ── */
    case 'X': {
        undo_save();
        for (int i = 0; i < n; i++) {
            if (E.cursor_col == 0) break;
            size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
            if (byte_pos == 0) break;
            char c = buf_byte_at(&E.buf, byte_pos - 1);
            if (c == '\n') break;
            lcache_invalidate_from(&E.lcache, byte_pos - 1);
            buf_delete(&E.buf, byte_pos - 1, 1);
            E.cursor_col--;
        }
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── r — replace char ── */
    case 'r': {
        int ch = term_read_key();
        if (ch == '\x1b' || ch == KEY_NONE) { vim_reset(); return; }
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        if (byte_pos < E.buf.total_bytes) {
            char c = (char)ch;
            undo_save();
            lcache_invalidate_from(&E.lcache, byte_pos);
            buf_delete(&E.buf, byte_pos, 1);
            buf_insert(&E.buf, byte_pos, &c, 1);
        }
        vim_reset();
        return;
    }

    /* ── D — delete to end of line ── */
    case 'D': {
        size_t from = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        size_t eol = buf_byte_offset(&E.buf, E.cursor_line,
            lcache_line_length(&E.lcache, &E.buf, E.cursor_line));
        if (eol > from) {
            undo_save();
            yank_range(from, eol, 0);
            lcache_invalidate_from(&E.lcache, from);
            buf_delete(&E.buf, from, eol - from);
        }
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── C — change to end of line ── */
    case 'C': {
        size_t from = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        size_t eol = buf_byte_offset(&E.buf, E.cursor_line,
            lcache_line_length(&E.lcache, &E.buf, E.cursor_line));
        if (eol > from) {
            undo_save();
            yank_range(from, eol, 0);
            lcache_invalidate_from(&E.lcache, from);
            buf_delete(&E.buf, from, eol - from);
        }
        E.mode = MODE_INSERT;
        vim_reset();
        return;
    }

    /* ── Y — yank line (like yy) ── */
    case 'Y': {
        execute_op_lines(OP_YANK, E.cursor_line, E.cursor_line + (size_t)n - 1);
        vim_reset();
        return;
    }

    /* ── S — substitute line (like cc) ── */
    case 'S': {
        execute_op_lines(OP_CHANGE, E.cursor_line, E.cursor_line + (size_t)n - 1);
        vim_reset();
        return;
    }

    /* ── s — substitute char ── */
    case 's': {
        undo_save();
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        if (byte_pos < E.buf.total_bytes) {
            char c = buf_byte_at(&E.buf, byte_pos);
            if (c != '\n') {
                lcache_invalidate_from(&E.lcache, byte_pos);
                buf_delete(&E.buf, byte_pos, 1);
            }
        }
        E.mode = MODE_INSERT;
        vim_reset();
        return;
    }

    /* ── p/P — paste ── */
    case 'p': {
        if (!E.yank_buf || E.yank_len == 0) { vim_reset(); return; }
        undo_save();
        if (E.yank_linewise) {
            /* Paste below current line */
            size_t next = lcache_find_line_offset(&E.lcache, &E.buf, E.cursor_line + 1);
            size_t pos;
            if (next == (size_t)-1)
                pos = E.buf.total_bytes;
            else
                pos = next;
            /* If buffer doesn't end with newline, insert one first */
            if (pos == E.buf.total_bytes && E.buf.total_bytes > 0 &&
                buf_byte_at(&E.buf, E.buf.total_bytes - 1) != '\n') {
                lcache_invalidate_from(&E.lcache, pos);
                buf_insert(&E.buf, pos, "\n", 1);
                pos++;
            }
            lcache_invalidate_from(&E.lcache, pos);
            for (int i = 0; i < n; i++)
                buf_insert(&E.buf, pos + (size_t)i * E.yank_len, E.yank_buf, E.yank_len);
            E.cursor_line++;
            E.cursor_col = 0;
            clamp_cursor();
        } else {
            /* Paste after cursor */
            size_t pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
            if (E.buf.total_bytes > 0 && pos < E.buf.total_bytes)
                pos++;
            lcache_invalidate_from(&E.lcache, pos);
            for (int i = 0; i < n; i++)
                buf_insert(&E.buf, pos + (size_t)i * E.yank_len, E.yank_buf, E.yank_len);
            /* Put cursor at end of pasted text */
            size_t end_pos = pos + (size_t)n * E.yank_len;
            if (end_pos > 0) end_pos--;
            byte_to_linecol(end_pos, &E.cursor_line, &E.cursor_col);
            clamp_cursor();
        }
        vim_reset();
        return;
    }

    case 'P': {
        if (!E.yank_buf || E.yank_len == 0) { vim_reset(); return; }
        undo_save();
        if (E.yank_linewise) {
            size_t pos = buf_byte_offset(&E.buf, E.cursor_line, 0);
            lcache_invalidate_from(&E.lcache, pos);
            for (int i = 0; i < n; i++)
                buf_insert(&E.buf, pos + (size_t)i * E.yank_len, E.yank_buf, E.yank_len);
            E.cursor_col = 0;
            clamp_cursor();
        } else {
            size_t pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
            lcache_invalidate_from(&E.lcache, pos);
            for (int i = 0; i < n; i++)
                buf_insert(&E.buf, pos + (size_t)i * E.yank_len, E.yank_buf, E.yank_len);
            size_t end_pos = pos + (size_t)n * E.yank_len;
            if (end_pos > 0) end_pos--;
            byte_to_linecol(end_pos, &E.cursor_line, &E.cursor_col);
            clamp_cursor();
        }
        vim_reset();
        return;
    }

    /* ── J — join lines ── */
    case 'J': {
        undo_save();
        for (int i = 0; i < n; i++) {
            size_t eol = buf_byte_offset(&E.buf, E.cursor_line,
                lcache_line_length(&E.lcache, &E.buf, E.cursor_line));
            if (eol >= E.buf.total_bytes) break;
            /* Delete the newline and any leading whitespace on next line */
            lcache_invalidate_from(&E.lcache, eol);
            buf_delete(&E.buf, eol, 1); /* delete newline */
            /* Delete leading whitespace on what was the next line */
            while (eol < E.buf.total_bytes) {
                char c = buf_byte_at(&E.buf, eol);
                if (c != ' ' && c != '\t') break;
                buf_delete(&E.buf, eol, 1);
            }
            /* Insert a space if the joined position isn't at a space already */
            if (eol < E.buf.total_bytes && eol > 0) {
                buf_insert(&E.buf, eol, " ", 1);
            }
            E.cursor_col = eol > 0 ? eol - buf_byte_offset(&E.buf, E.cursor_line, 0) : 0;
        }
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── Visual mode ── */
    case 'v':
        E.mode = MODE_VISUAL;
        E.visual_anchor_line = E.cursor_line;
        E.visual_anchor_col = E.cursor_col;
        vim_reset();
        return;

    case 'V':
        E.mode = MODE_VISUAL_LINE;
        E.visual_anchor_line = E.cursor_line;
        E.visual_anchor_col = 0;
        vim_reset();
        return;

    /* ── Command / Search ── */
    case ':':
        E.mode = MODE_COMMAND;
        E.cmd_len = 0;
        editor_set_status("");
        vim_reset();
        return;

    case '/':
        E.mode = MODE_COMMAND;
        E.cmd_buf[0] = '/';
        E.cmd_len = 1;
        vim_reset();
        return;

    case '?':
        E.mode = MODE_COMMAND;
        E.cmd_buf[0] = '?';
        E.cmd_len = 1;
        vim_reset();
        return;

    case 'n':
        do_search_next(E.search_direction);
        vim_reset();
        return;

    case 'N':
        do_search_next(-E.search_direction);
        vim_reset();
        return;

    /* ── ZZ / ZQ ── */
    case 'Z': {
        int next = term_read_key();
        if (next == 'Z') {
            if (E.buf.dirty) {
                if (E.buf.filename && buf_save(&E.buf) == 0) {
                    term_disable_raw();
                    exit(0);
                }
                editor_set_status("No filename");
            } else {
                term_disable_raw();
                exit(0);
            }
        } else if (next == 'Q') {
            term_disable_raw();
            exit(0);
        }
        vim_reset();
        return;
    }

    /* ── u — undo ── */
    case 'u': {
        if (E.undo.count == 0) {
            editor_set_status("Already at oldest change");
            vim_reset();
            return;
        }
        /* Push current state to redo stack */
        undo_push(&E.redo, &E.buf, E.cursor_line, E.cursor_col);

        /* Pop from undo stack and restore */
        UndoEntry *e = &E.undo.entries[--E.undo.count];
        free(E.buf.pieces);
        E.buf.pieces = e->pieces;
        E.buf.piece_count = e->piece_count;
        E.buf.piece_cap = e->piece_count;
        E.buf.total_bytes = e->total_bytes;
        E.buf.add_len = e->add_len;
        E.buf.dirty = 1;
        E.cursor_line = e->cursor_line;
        E.cursor_col = e->cursor_col;
        /* Don't free e->pieces — ownership transferred to buf */

        lcache_free(&E.lcache);
        lcache_init(&E.lcache);
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── Ctrl-R — redo ── */
    case CTRL_KEY('r'): {
        if (E.redo.count == 0) {
            editor_set_status("Already at newest change");
            vim_reset();
            return;
        }
        /* Push current state to undo stack */
        undo_push(&E.undo, &E.buf, E.cursor_line, E.cursor_col);

        /* Pop from redo stack and restore */
        UndoEntry *e = &E.redo.entries[--E.redo.count];
        free(E.buf.pieces);
        E.buf.pieces = e->pieces;
        E.buf.piece_count = e->piece_count;
        E.buf.piece_cap = e->piece_count;
        E.buf.total_bytes = e->total_bytes;
        E.buf.add_len = e->add_len;
        E.buf.dirty = 1;
        E.cursor_line = e->cursor_line;
        E.cursor_col = e->cursor_col;

        lcache_free(&E.lcache);
        lcache_init(&E.lcache);
        clamp_cursor();
        vim_reset();
        return;
    }

    /* ── . — repeat (not implemented) ── */
    case '.':
        editor_set_status("Repeat not implemented");
        vim_reset();
        return;

    /* ── ~ — toggle case ── */
    case '~': {
        undo_save();
        size_t byte_pos = buf_byte_offset(&E.buf, E.cursor_line, E.cursor_col);
        if (byte_pos < E.buf.total_bytes) {
            char c = buf_byte_at(&E.buf, byte_pos);
            if (c != '\n') {
                char toggled = c;
                if (islower((unsigned char)c)) toggled = (char)toupper((unsigned char)c);
                else if (isupper((unsigned char)c)) toggled = (char)tolower((unsigned char)c);
                if (toggled != c) {
                    lcache_invalidate_from(&E.lcache, byte_pos);
                    buf_delete(&E.buf, byte_pos, 1);
                    buf_insert(&E.buf, byte_pos, &toggled, 1);
                }
                size_t len = lcache_line_length(&E.lcache, &E.buf, E.cursor_line);
                if (E.cursor_col + 1 < len)
                    E.cursor_col++;
            }
        }
        vim_reset();
        return;
    }

    case '\x1b':
        vim_reset();
        return;

    default:
        vim_reset();
        return;
    }
}

/* ── Visual mode ──────────────────────────────────────────────────── */

static void handle_visual(int key)
{
    /* Escape — cancel */
    if (key == '\x1b') {
        E.mode = MODE_NORMAL;
        vim_reset();
        return;
    }

    /* v/V — toggle or cancel visual mode */
    if (key == 'v') {
        if (E.mode == MODE_VISUAL) { E.mode = MODE_NORMAL; vim_reset(); return; }
        E.mode = MODE_VISUAL;
        return;
    }
    if (key == 'V') {
        if (E.mode == MODE_VISUAL_LINE) { E.mode = MODE_NORMAL; vim_reset(); return; }
        E.mode = MODE_VISUAL_LINE;
        return;
    }

    /* Motions — move cursor, extending selection */
    size_t from, to;
    size_t save_line = E.cursor_line;
    size_t save_col = E.cursor_col;
    if (do_motion(key, &from, &to) == 0) {
        /* Cursor was moved by do_motion */
        vim_reset();
        return;
    }
    E.cursor_line = save_line;
    E.cursor_col = save_col;

    /* Handle gg in visual mode */
    if (key == 'g') {
        int next = term_read_key();
        if (next == 'g') {
            E.cursor_line = 0;
            E.cursor_col = 0;
            clamp_cursor();
        }
        vim_reset();
        return;
    }

    /* Operators on visual selection */
    if (key == 'd' || key == 'c' || key == 'y' || key == 'x') {
        VimOperator op = (key == 'y') ? OP_YANK :
                         (key == 'c') ? OP_CHANGE : OP_DELETE;

        if (E.mode == MODE_VISUAL_LINE) {
            size_t sl, el;
            if (E.visual_anchor_line <= E.cursor_line) {
                sl = E.visual_anchor_line;
                el = E.cursor_line;
            } else {
                sl = E.cursor_line;
                el = E.visual_anchor_line;
            }
            E.mode = MODE_NORMAL;
            execute_op_lines(op, sl, el);
        } else {
            size_t s_line, s_col, e_line, e_col;
            editor_visual_range(&s_line, &s_col, &e_line, &e_col);
            size_t from_b = buf_byte_offset(&E.buf, s_line, s_col);
            size_t to_b = buf_byte_offset(&E.buf, e_line, e_col);
            /* Include the char at end of selection */
            if (to_b < E.buf.total_bytes) to_b++;
            E.mode = MODE_NORMAL;
            execute_op_range(op, from_b, to_b);
        }
        vim_reset();
        return;
    }

    /* > / < — indent/dedent (simple: just status message) */
    /* J — join */
    if (key == 'J') {
        undo_save();
        size_t sl, el;
        if (E.mode == MODE_VISUAL_LINE) {
            if (E.visual_anchor_line <= E.cursor_line) {
                sl = E.visual_anchor_line; el = E.cursor_line;
            } else {
                sl = E.cursor_line; el = E.visual_anchor_line;
            }
        } else {
            size_t s_line, s_col, e_line, e_col;
            editor_visual_range(&s_line, &s_col, &e_line, &e_col);
            sl = s_line; el = e_line;
        }
        E.mode = MODE_NORMAL;
        E.cursor_line = sl;
        for (size_t l = sl; l < el; l++) {
            size_t eol = buf_byte_offset(&E.buf, E.cursor_line,
                lcache_line_length(&E.lcache, &E.buf, E.cursor_line));
            if (eol >= E.buf.total_bytes) break;
            lcache_invalidate_from(&E.lcache, eol);
            buf_delete(&E.buf, eol, 1);
            while (eol < E.buf.total_bytes) {
                char c = buf_byte_at(&E.buf, eol);
                if (c != ' ' && c != '\t') break;
                buf_delete(&E.buf, eol, 1);
            }
            if (eol < E.buf.total_bytes && eol > 0)
                buf_insert(&E.buf, eol, " ", 1);
        }
        clamp_cursor();
        vim_reset();
        return;
    }

    vim_reset();
}

/* ── Top-level key dispatch ───────────────────────────────────────── */

void editor_process_key(int key)
{
    if (key == KEY_NONE)
        return;

    switch (E.mode) {
    case MODE_NORMAL:
        handle_normal(key);
        break;
    case MODE_INSERT:
        handle_insert(key);
        break;
    case MODE_VISUAL:
    case MODE_VISUAL_LINE:
        handle_visual(key);
        break;
    case MODE_COMMAND:
        handle_command(key);
        break;
    }
}

/* ── Signal handling ──────────────────────────────────────────────── */

static volatile sig_atomic_t got_sigwinch = 0;
static volatile sig_atomic_t got_sigtstp = 0;
static volatile sig_atomic_t got_sigcont = 0;

static void handle_sigwinch(int sig) { (void)sig; got_sigwinch = 1; }
static void handle_sigtstp(int sig)  { (void)sig; got_sigtstp = 1; }
static void handle_sigcont(int sig)  { (void)sig; got_sigcont = 1; }

static void setup_signals(void)
{
    struct sigaction sa;
    sa.sa_handler = handle_sigwinch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);
    sa.sa_handler = handle_sigtstp;
    sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = handle_sigcont;
    sigaction(SIGCONT, &sa, NULL);
}

/* ── Init ─────────────────────────────────────────────────────────── */

void editor_init(const char *filename)
{
    memset(&E, 0, sizeof(E));

    term_enable_raw();
    term_get_window_size(&E.screen_rows, &E.screen_cols);

    E.render_cap = 65536;
    E.render_buf = malloc(E.render_cap);
    E.render_len = 0;

    E.mode = MODE_NORMAL;
    E.search_direction = 1;

    lcache_init(&E.lcache);
    undo_init(&E.undo);
    undo_init(&E.redo);

    if (buf_open(&E.buf, filename) == -1) {
        term_disable_raw();
        fprintf(stderr, "Error opening file: %s\n", filename ? filename : "(null)");
        exit(1);
    }

    setup_signals();

    if (E.buf.total_bytes == 0 && filename == NULL)
        editor_set_status(":w <file> to save | :q to quit | i to insert");
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *filename = argc >= 2 ? argv[1] : NULL;
    editor_init(filename);

    for (;;) {
        if (got_sigwinch) {
            got_sigwinch = 0;
            term_get_window_size(&E.screen_rows, &E.screen_cols);
        }
        if (got_sigtstp) {
            got_sigtstp = 0;
            term_disable_raw();
            raise(SIGSTOP);
        }
        if (got_sigcont) {
            got_sigcont = 0;
            term_enable_raw();
            term_get_window_size(&E.screen_rows, &E.screen_cols);
        }

        editor_scroll();
        render_screen();

        int key = term_read_key();
        if (key != KEY_NONE)
            editor_process_key(key);
    }

    return 0;
}
