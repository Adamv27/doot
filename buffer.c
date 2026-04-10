#include "doot.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Piece helpers ────────────────────────────────────────────────── */

static void pieces_ensure_cap(Buffer *buf, int needed)
{
    if (buf->piece_count + needed <= buf->piece_cap)
        return;
    int new_cap = buf->piece_cap * 2;
    if (new_cap < buf->piece_count + needed)
        new_cap = buf->piece_count + needed + 16;
    buf->pieces = realloc(buf->pieces, (size_t)new_cap * sizeof(Piece));
    buf->piece_cap = new_cap;
}

static void add_ensure_cap(Buffer *buf, size_t needed)
{
    if (buf->add_len + needed <= buf->add_cap)
        return;
    size_t new_cap = buf->add_cap * 2;
    if (new_cap < buf->add_len + needed)
        new_cap = buf->add_len + needed + 4096;
    buf->add_data = realloc(buf->add_data, new_cap);
    buf->add_cap = new_cap;
}

/* Return pointer to the data for a piece */
static const char *piece_data(Buffer *buf, Piece *p)
{
    if (p->source == SRC_ORIGINAL)
        return buf->orig_data + p->offset;
    return buf->add_data + p->offset;
}

/* Find the piece containing byte_pos. Sets *offset_in_piece. Returns index. */
static int find_piece(Buffer *buf, size_t byte_pos, size_t *offset_in_piece)
{
    size_t accum = 0;
    for (int i = 0; i < buf->piece_count; i++) {
        if (byte_pos < accum + buf->pieces[i].length) {
            *offset_in_piece = byte_pos - accum;
            return i;
        }
        accum += buf->pieces[i].length;
    }
    /* Position at end of buffer */
    *offset_in_piece = 0;
    return buf->piece_count;
}

/* ── Buffer open / close ──────────────────────────────────────────── */

int buf_open(Buffer *buf, const char *filename)
{
    memset(buf, 0, sizeof(*buf));
    buf->orig_fd = -1;

    /* Allocate add buffer */
    buf->add_cap = 4096;
    buf->add_data = malloc(buf->add_cap);
    buf->add_len = 0;

    /* Allocate piece array */
    buf->piece_cap = 64;
    buf->pieces = malloc((size_t)buf->piece_cap * sizeof(Piece));
    buf->piece_count = 0;

    if (filename) {
        buf->filename = strdup(filename);
        int fd = open(filename, O_RDONLY);
        if (fd == -1) {
            if (errno == ENOENT) {
                /* New file — will be created on save */
                return 0;
            }
            return -1;
        }

        struct stat st;
        if (fstat(fd, &st) == -1) {
            close(fd);
            return -1;
        }

        if (st.st_size > 0) {
            void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map == MAP_FAILED) {
                /* Fallback: read into malloc'd buffer */
                char *data = malloc((size_t)st.st_size);
                if (!data) {
                    close(fd);
                    return -1;
                }
                ssize_t total = 0;
                while (total < st.st_size) {
                    ssize_t n = read(fd, data + total, (size_t)(st.st_size - total));
                    if (n <= 0) {
                        free(data);
                        close(fd);
                        return -1;
                    }
                    total += n;
                }
                buf->orig_data = data;
                buf->orig_size = (size_t)st.st_size;
                buf->orig_fd = -1; /* no mmap, will free() instead */
                close(fd);
            } else {
                madvise(map, (size_t)st.st_size, MADV_RANDOM);
                buf->orig_data = map;
                buf->orig_size = (size_t)st.st_size;
                buf->orig_fd = fd;
            }

            /* Single piece covering entire original file */
            buf->pieces[0] = (Piece){ SRC_ORIGINAL, 0, buf->orig_size };
            buf->piece_count = 1;
            buf->total_bytes = buf->orig_size;
        } else {
            close(fd);
        }
    }

    return 0;
}

void buf_free(Buffer *buf)
{
    if (buf->orig_fd >= 0) {
        munmap((void *)buf->orig_data, buf->orig_size);
        close(buf->orig_fd);
    } else if (buf->orig_data) {
        free((void *)buf->orig_data);
    }
    free(buf->add_data);
    free(buf->pieces);
    free(buf->filename);
    memset(buf, 0, sizeof(*buf));
}

/* ── Read bytes from logical position ─────────────────────────────── */

size_t buf_read_range(Buffer *buf, size_t pos, size_t len, char *out)
{
    if (pos >= buf->total_bytes)
        return 0;
    if (pos + len > buf->total_bytes)
        len = buf->total_bytes - pos;

    size_t written = 0;
    size_t off_in_piece;
    int idx = find_piece(buf, pos, &off_in_piece);

    while (written < len && idx < buf->piece_count) {
        Piece *p = &buf->pieces[idx];
        const char *data = piece_data(buf, p);
        size_t avail = p->length - off_in_piece;
        size_t to_copy = len - written;
        if (to_copy > avail)
            to_copy = avail;
        memcpy(out + written, data + off_in_piece, to_copy);
        written += to_copy;
        off_in_piece = 0;
        idx++;
    }

    return written;
}

char buf_byte_at(Buffer *buf, size_t pos)
{
    char c = 0;
    buf_read_range(buf, pos, 1, &c);
    return c;
}

/* ── Insert ───────────────────────────────────────────────────────── */

int buf_insert(Buffer *buf, size_t byte_pos, const char *text, size_t len)
{
    if (len == 0)
        return 0;

    /* Append text to add buffer */
    add_ensure_cap(buf, len);
    size_t add_offset = buf->add_len;
    memcpy(buf->add_data + buf->add_len, text, len);
    buf->add_len += len;

    /* Coalescing: if inserting at the point right after the last add-buffer piece,
       and that piece ends at the old add_len, just extend it */
    if (buf->piece_count > 0) {
        size_t accum = 0;
        for (int i = 0; i < buf->piece_count; i++)
            accum += buf->pieces[i].length;

        /* Check if inserting at end and last piece is extendable */
        Piece *last = &buf->pieces[buf->piece_count - 1];
        if (byte_pos == accum &&
            last->source == SRC_ADD &&
            last->offset + last->length == add_offset) {
            last->length += len;
            buf->total_bytes += len;
            buf->dirty = 1;
            return 0;
        }

        /* Check if inserting right after a piece boundary that's an ADD piece */
        accum = 0;
        for (int i = 0; i < buf->piece_count; i++) {
            accum += buf->pieces[i].length;
            if (accum == byte_pos && buf->pieces[i].source == SRC_ADD &&
                buf->pieces[i].offset + buf->pieces[i].length == add_offset) {
                buf->pieces[i].length += len;
                buf->total_bytes += len;
                buf->dirty = 1;
                return 0;
            }
        }
    }

    /* General case: split piece at byte_pos and insert new piece */
    size_t off_in_piece;
    int idx = find_piece(buf, byte_pos, &off_in_piece);

    Piece new_piece = { SRC_ADD, add_offset, len };

    if (idx >= buf->piece_count) {
        /* Inserting at end of buffer */
        pieces_ensure_cap(buf, 1);
        buf->pieces[buf->piece_count] = new_piece;
        buf->piece_count++;
    } else if (off_in_piece == 0) {
        /* Inserting at start of a piece */
        pieces_ensure_cap(buf, 1);
        memmove(&buf->pieces[idx + 1], &buf->pieces[idx],
                (size_t)(buf->piece_count - idx) * sizeof(Piece));
        buf->pieces[idx] = new_piece;
        buf->piece_count++;
    } else {
        /* Split the piece */
        Piece orig = buf->pieces[idx];
        Piece before = { orig.source, orig.offset, off_in_piece };
        Piece after = { orig.source, orig.offset + off_in_piece,
                        orig.length - off_in_piece };

        pieces_ensure_cap(buf, 2);
        /* Make room for 2 extra pieces (replacing 1 with 3) */
        memmove(&buf->pieces[idx + 3], &buf->pieces[idx + 1],
                (size_t)(buf->piece_count - idx - 1) * sizeof(Piece));
        buf->pieces[idx] = before;
        buf->pieces[idx + 1] = new_piece;
        buf->pieces[idx + 2] = after;
        buf->piece_count += 2;
    }

    buf->total_bytes += len;
    buf->dirty = 1;
    return 0;
}

/* ── Delete ───────────────────────────────────────────────────────── */

int buf_delete(Buffer *buf, size_t byte_pos, size_t len)
{
    if (len == 0 || byte_pos >= buf->total_bytes)
        return 0;
    if (byte_pos + len > buf->total_bytes)
        len = buf->total_bytes - byte_pos;

    size_t del_end = byte_pos + len;
    size_t off_in_piece;
    int start_idx = find_piece(buf, byte_pos, &off_in_piece);

    /* Walk through pieces, trimming/removing as needed */
    size_t accum = byte_pos - off_in_piece;
    int i = start_idx;

    while (i < buf->piece_count && len > 0) {
        Piece *p = &buf->pieces[i];
        size_t piece_start = accum;
        size_t piece_end = accum + p->length;
        size_t del_start_in_piece = byte_pos > piece_start ? byte_pos - piece_start : 0;
        size_t del_end_in_piece = del_end < piece_end ? del_end - piece_start : p->length;
        size_t del_len = del_end_in_piece - del_start_in_piece;

        if (del_start_in_piece == 0 && del_len == p->length) {
            /* Remove entire piece */
            memmove(&buf->pieces[i], &buf->pieces[i + 1],
                    (size_t)(buf->piece_count - i - 1) * sizeof(Piece));
            buf->piece_count--;
            len -= del_len;
            byte_pos += del_len;
            /* don't increment i */
        } else if (del_start_in_piece == 0) {
            /* Trim from the start */
            p->offset += del_len;
            p->length -= del_len;
            len -= del_len;
            byte_pos += del_len;
            accum += p->length;
            i++;
        } else if (del_end_in_piece == p->length) {
            /* Trim from the end */
            p->length -= del_len;
            len -= del_len;
            byte_pos += del_len;
            accum += p->length;
            i++;
        } else {
            /* Split: delete from middle of a piece */
            Piece before = { p->source, p->offset, del_start_in_piece };
            Piece after = { p->source, p->offset + del_end_in_piece,
                            p->length - del_end_in_piece };

            pieces_ensure_cap(buf, 1);
            p = &buf->pieces[i]; /* realloc may have moved */
            memmove(&buf->pieces[i + 2], &buf->pieces[i + 1],
                    (size_t)(buf->piece_count - i - 1) * sizeof(Piece));
            buf->pieces[i] = before;
            buf->pieces[i + 1] = after;
            buf->piece_count++;
            len = 0;
        }
    }

    buf->total_bytes = 0;
    for (int j = 0; j < buf->piece_count; j++)
        buf->total_bytes += buf->pieces[j].length;

    buf->dirty = 1;
    return 0;
}

/* ── Save ─────────────────────────────────────────────────────────── */

int buf_save(Buffer *buf)
{
    if (!buf->filename)
        return -1;

    /* Create temp file in the same directory */
    size_t flen = strlen(buf->filename);
    char *tmpname = malloc(flen + 8);
    snprintf(tmpname, flen + 8, "%s.XXXXXX", buf->filename);

    int fd = mkstemp(tmpname);
    if (fd == -1) {
        free(tmpname);
        return -1;
    }

    /* Write all pieces */
    for (int i = 0; i < buf->piece_count; i++) {
        Piece *p = &buf->pieces[i];
        const char *data = piece_data(buf, p);
        size_t remaining = p->length;
        size_t offset = 0;
        while (remaining > 0) {
            ssize_t n = write(fd, data + offset, remaining);
            if (n <= 0) {
                close(fd);
                unlink(tmpname);
                free(tmpname);
                return -1;
            }
            offset += (size_t)n;
            remaining -= (size_t)n;
        }
    }

    if (fsync(fd) == -1) {
        close(fd);
        unlink(tmpname);
        free(tmpname);
        return -1;
    }
    close(fd);

    if (rename(tmpname, buf->filename) == -1) {
        unlink(tmpname);
        free(tmpname);
        return -1;
    }
    free(tmpname);

    /* Re-mmap the saved file */
    if (buf->orig_fd >= 0) {
        munmap((void *)buf->orig_data, buf->orig_size);
        close(buf->orig_fd);
    } else if (buf->orig_data) {
        free((void *)buf->orig_data);
    }
    buf->orig_data = NULL;
    buf->orig_size = 0;
    buf->orig_fd = -1;

    int newfd = open(buf->filename, O_RDONLY);
    if (newfd >= 0) {
        struct stat st;
        if (fstat(newfd, &st) == 0 && st.st_size > 0) {
            void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, newfd, 0);
            if (map != MAP_FAILED) {
                madvise(map, (size_t)st.st_size, MADV_RANDOM);
                buf->orig_data = map;
                buf->orig_size = (size_t)st.st_size;
                buf->orig_fd = newfd;
            } else {
                close(newfd);
            }
        } else {
            close(newfd);
        }
    }

    /* Reset to single piece over the new file */
    buf->pieces[0] = (Piece){ SRC_ORIGINAL, 0, buf->orig_size };
    buf->piece_count = 1;
    buf->total_bytes = buf->orig_size;

    /* Reset add buffer */
    buf->add_len = 0;
    buf->dirty = 0;

    return 0;
}

/* ── Line cache ───────────────────────────────────────────────────── */

void lcache_init(LineCache *lc)
{
    lc->cap = 256;
    lc->entries = malloc((size_t)lc->cap * sizeof(LineCacheEntry));
    lc->count = 0;

    /* Seed with line 0 at byte 0 */
    lc->entries[0] = (LineCacheEntry){ 0, 0, 0, 0 };
    lc->count = 1;
}

void lcache_free(LineCache *lc)
{
    free(lc->entries);
    memset(lc, 0, sizeof(*lc));
}

void lcache_invalidate_from(LineCache *lc, size_t byte_offset)
{
    /* Binary search for first entry with byte_offset >= threshold, remove from there */
    int lo = 0, hi = lc->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (lc->entries[mid].byte_offset < byte_offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* Keep entry at byte_offset 0 always */
    if (lo < 1)
        lo = 1;
    lc->count = lo;
}

static void lcache_insert_entry(LineCache *lc, LineCacheEntry entry)
{
    /* Find insertion point (sorted by line_num) */
    int lo = 0, hi = lc->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (lc->entries[mid].line_num < entry.line_num)
            lo = mid + 1;
        else
            hi = mid;
    }

    /* If exact line already cached, update it */
    if (lo < lc->count && lc->entries[lo].line_num == entry.line_num) {
        lc->entries[lo] = entry;
        return;
    }

    /* Insert */
    if (lc->count >= lc->cap) {
        lc->cap *= 2;
        lc->entries = realloc(lc->entries, (size_t)lc->cap * sizeof(LineCacheEntry));
    }
    memmove(&lc->entries[lo + 1], &lc->entries[lo],
            (size_t)(lc->count - lo) * sizeof(LineCacheEntry));
    lc->entries[lo] = entry;
    lc->count++;
}

/* Find the byte offset of the start of target_line.
   Returns (size_t)-1 if the line doesn't exist (past end of file). */
size_t lcache_find_line_offset(LineCache *lc, Buffer *buf, size_t target_line)
{
    if (buf->total_bytes == 0)
        return target_line == 0 ? 0 : (size_t)-1;

    /* Binary search for nearest cached line <= target_line */
    int lo = 0, hi = lc->count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (lc->entries[mid].line_num <= target_line)
            lo = mid;
        else
            hi = mid - 1;
    }

    LineCacheEntry anchor = lc->entries[lo];
    if (anchor.line_num == target_line)
        return anchor.byte_offset;

    /* Scan forward from anchor counting newlines */
    size_t cur_line = anchor.line_num;
    size_t cur_byte = anchor.byte_offset;

    /* Scan through pieces */
    size_t piece_byte_start = 0;
    int pi = 0;

    /* Find the piece containing cur_byte */
    for (pi = 0; pi < buf->piece_count; pi++) {
        if (cur_byte < piece_byte_start + buf->pieces[pi].length)
            break;
        piece_byte_start += buf->pieces[pi].length;
    }

    size_t off_in_piece = cur_byte - piece_byte_start;

    while (pi < buf->piece_count && cur_line < target_line) {
        Piece *p = &buf->pieces[pi];
        const char *data = piece_data(buf, p);
        size_t scan_from = off_in_piece;

        while (scan_from < p->length && cur_line < target_line) {
            const char *found = memchr(data + scan_from, '\n', p->length - scan_from);
            if (!found)
                break;
            size_t nl_pos = (size_t)(found - data);
            cur_byte = piece_byte_start + nl_pos + 1;
            cur_line++;

            /* Cache every 512 lines */
            if (cur_line % 512 == 0) {
                /* Find piece index and offset for caching */
                size_t cache_off = cur_byte - piece_byte_start;
                if (cache_off >= p->length) {
                    /* The newline was at end of this piece, next line starts in next piece */
                    lcache_insert_entry(lc, (LineCacheEntry){
                        cur_line, pi + 1, 0, cur_byte });
                } else {
                    lcache_insert_entry(lc, (LineCacheEntry){
                        cur_line, pi, cache_off, cur_byte });
                }
            }

            scan_from = nl_pos + 1;
        }

        piece_byte_start += p->length;
        off_in_piece = 0;
        pi++;
    }

    if (cur_line == target_line) {
        /* Cache this line too */
        lcache_insert_entry(lc, (LineCacheEntry){
            cur_line, 0, 0, cur_byte });
        /* Fix up piece_idx / piece_offset */
        size_t pbs = 0;
        for (int j = 0; j < buf->piece_count; j++) {
            if (cur_byte < pbs + buf->pieces[j].length) {
                lc->entries[lc->count - 1].piece_idx = j;
                lc->entries[lc->count - 1].piece_offset = cur_byte - pbs;
                break;
            }
            pbs += buf->pieces[j].length;
        }
        return cur_byte;
    }

    return (size_t)-1; /* Past end of file */
}

/* Get the content of line_num into out. Returns length, or (size_t)-1 if past EOF. */
size_t lcache_get_line_content(LineCache *lc, Buffer *buf, size_t line_num,
                               char *out, size_t max_len)
{
    size_t line_start = lcache_find_line_offset(lc, buf, line_num);
    if (line_start == (size_t)-1)
        return (size_t)-1;

    /* Read bytes until newline or end of buffer */
    size_t len = 0;
    size_t pos = line_start;
    char tmp[256];

    while (pos < buf->total_bytes && len < max_len) {
        size_t chunk = sizeof(tmp);
        if (chunk > max_len - len)
            chunk = max_len - len;
        size_t got = buf_read_range(buf, pos, chunk, tmp);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            if (tmp[i] == '\n')
                return len;
            if (len < max_len)
                out[len++] = tmp[i];
        }
        pos += got;
    }

    return len;
}

size_t lcache_line_length(LineCache *lc, Buffer *buf, size_t line_num)
{
    size_t line_start = lcache_find_line_offset(lc, buf, line_num);
    if (line_start == (size_t)-1)
        return 0;

    size_t len = 0;
    size_t pos = line_start;
    char tmp[256];

    while (pos < buf->total_bytes) {
        size_t got = buf_read_range(buf, pos, sizeof(tmp), tmp);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            if (tmp[i] == '\n')
                return len;
            len++;
        }
        pos += got;
    }

    return len;
}

/* Get absolute byte offset for a (line, col) position */
size_t buf_byte_offset(Buffer *buf, size_t line, size_t col)
{
    size_t line_start = lcache_find_line_offset(&E.lcache, buf, line);
    if (line_start == (size_t)-1)
        return buf->total_bytes;
    return line_start + col;
}
