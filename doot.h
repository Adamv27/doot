#ifndef DOOT_H
#define DOOT_H

#include <stddef.h>
#include <stdint.h>
#include <termios.h>
#include <time.h>

/* Key constants */
enum {
    KEY_NONE = 0,
    KEY_ARROW_UP = 1000,
    KEY_ARROW_DOWN,
    KEY_ARROW_LEFT,
    KEY_ARROW_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_BACKSPACE = 127,
};

/* Ctrl key macro */
#define CTRL_KEY(k) ((k) & 0x1f)

/* Line-number gutter: 4 digit columns + 1 space separator */
#define GUTTER_WIDTH 5

/* Vim modes */
typedef enum {
    MODE_NORMAL,
    MODE_INSERT,
    MODE_VISUAL,
    MODE_VISUAL_LINE,
    MODE_COMMAND,
} VimMode;

/* Operator pending */
typedef enum {
    OP_NONE,
    OP_DELETE,
    OP_CHANGE,
    OP_YANK,
} VimOperator;

/* Piece table source */
typedef enum { SRC_ORIGINAL, SRC_ADD } PieceSource;

typedef struct {
    PieceSource source;
    size_t      offset;
    size_t      length;
} Piece;

typedef struct {
    /* Original file (mmap'd, read-only) */
    const char *orig_data;
    size_t      orig_size;
    int         orig_fd;

    /* Add buffer (append-only, heap-allocated) */
    char       *add_data;
    size_t      add_len;
    size_t      add_cap;

    /* Piece array */
    Piece      *pieces;
    int         piece_count;
    int         piece_cap;

    /* Metadata */
    char       *filename;
    int         dirty;
    size_t      total_bytes;
} Buffer;

/* Line cache entry */
typedef struct {
    size_t line_num;
    int    piece_idx;
    size_t piece_offset;
    size_t byte_offset;
} LineCacheEntry;

typedef struct {
    LineCacheEntry *entries;
    int             count;
    int             cap;
} LineCache;

/* Editor state */
typedef struct {
    Buffer      buf;
    LineCache   lcache;

    /* Cursor position (logical) */
    size_t      cursor_line;
    size_t      cursor_col;

    /* Viewport */
    size_t      row_offset;
    size_t      col_offset;

    /* Terminal dimensions */
    int         screen_rows;
    int         screen_cols;

    /* Status / message bar */
    char        status_msg[256];
    time_t      status_msg_time;

    /* Render buffer */
    char       *render_buf;
    size_t      render_len;
    size_t      render_cap;

    /* Original terminal state */
    struct termios orig_termios;
    int            raw_mode_active;

    /* Vim state */
    VimMode     mode;
    VimOperator pending_op;
    int         g_pending;      /* 'g' prefix received, waiting for second key */
    int         count;          /* repeat count (0 = no count given) */
    int         count_active;   /* whether user has started typing a count */

    /* Visual mode selection anchor */
    size_t      visual_anchor_line;
    size_t      visual_anchor_col;

    /* Yank register */
    char       *yank_buf;
    size_t      yank_len;
    int         yank_linewise;  /* was yanked with line-wise operation */

    /* Command-line buffer (for : prompts) */
    char        cmd_buf[256];
    size_t      cmd_len;

    /* Search */
    char        search_pattern[256];
    int         search_direction; /* 1 = forward, -1 = backward */

    /* Quit confirmation */
    int         quit_times;
} EditorState;

/* Global editor state */
extern EditorState E;

/* === buffer.c === */
int    buf_open(Buffer *buf, const char *filename);
void   buf_free(Buffer *buf);
size_t buf_byte_offset(Buffer *buf, size_t line, size_t col);
char   buf_byte_at(Buffer *buf, size_t pos);
size_t buf_read_range(Buffer *buf, size_t pos, size_t len, char *out);
int    buf_insert(Buffer *buf, size_t byte_pos, const char *text, size_t len);
int    buf_delete(Buffer *buf, size_t byte_pos, size_t len);
int    buf_save(Buffer *buf);

/* === buffer.c — line cache === */
void   lcache_init(LineCache *lc);
void   lcache_free(LineCache *lc);
void   lcache_invalidate_from(LineCache *lc, size_t byte_offset);
size_t lcache_find_line_offset(LineCache *lc, Buffer *buf, size_t target_line);
size_t lcache_get_line_content(LineCache *lc, Buffer *buf, size_t line_num,
                               char *out, size_t max_len);
size_t lcache_line_length(LineCache *lc, Buffer *buf, size_t line_num);

/* === terminal.c === */
void   term_enable_raw(void);
void   term_disable_raw(void);
int    term_read_key(void);
void   term_get_window_size(int *rows, int *cols);

/* === terminal.c — rendering === */
void   render_append(const char *s, size_t len);
void   render_appendf(const char *fmt, ...);
void   render_screen(void);

/* === editor.c === */
void   editor_init(const char *filename);
void   editor_set_status(const char *fmt, ...);
void   editor_process_key(int key);

/* Vim helpers used by rendering */
void   editor_visual_range(size_t *start_line, size_t *start_col,
                           size_t *end_line, size_t *end_col);

#endif
