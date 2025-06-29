
/* Tokenizer implementation */

#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include "pycore_call.h" // _PyObject_CallNoArgs()

#include <assert.h>
#include <ctype.h>

#include "errcode.h"
#include "tokenizer.h"

/* Alternate tab spacing */
#define ALTTABSIZE 1

#define is_potential_identifier_start(c)                                       \
  ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 128))

#define is_potential_identifier_char(c)                                        \
  ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||                         \
   (c >= '0' && c <= '9') || c == '_' || (c >= 128))

/* Don't ever change this -- it would break the portability of Python code */
#define TABSIZE 8

#define MAKE_TOKEN(token_type)                                                 \
  token_setup(tok, token, token_type, p_start, p_end)
#define MAKE_TYPE_COMMENT_TOKEN(token_type, col_offset, end_col_offset)        \
  (type_comment_token_setup(tok, token, token_type, col_offset,                \
                            end_col_offset, p_start, p_end))
#define ADVANCE_LINENO()                                                       \
  tok->lineno++;                                                               \
  tok->col_offset = 0;

#define INSIDE_FSTRING(tok) (tok->tok_mode_stack_index > 0)
#define INSIDE_FSTRING_EXPR(tok) (tok->curly_bracket_expr_start_depth >= 0)
#ifdef Py_DEBUG
static inline tokenizer_mode *TOK_GET_MODE(struct tok_state *tok) {
  assert(tok->tok_mode_stack_index >= 0);
  assert(tok->tok_mode_stack_index < MAXFSTRINGLEVEL);
  return &(tok->tok_mode_stack[tok->tok_mode_stack_index]);
}
static inline tokenizer_mode *TOK_NEXT_MODE(struct tok_state *tok) {
  assert(tok->tok_mode_stack_index >= 0);
  assert(tok->tok_mode_stack_index + 1 < MAXFSTRINGLEVEL);
  return &(tok->tok_mode_stack[++tok->tok_mode_stack_index]);
}
#else
#define TOK_GET_MODE(tok) (&(tok->tok_mode_stack[tok->tok_mode_stack_index]))
#define TOK_NEXT_MODE(tok) (&(tok->tok_mode_stack[++tok->tok_mode_stack_index]))
#endif

/* Forward */
static struct tok_state *tok_new(void);
static int tok_nextc(struct tok_state *tok);
static void tok_backup(struct tok_state *tok, int c);
static int syntaxerror(struct tok_state *tok, const char *format, ...);

/* Spaces in this constant are treated as "zero or more spaces or tabs" when
   tokenizing. */
static const char *type_comment_prefix = "# type: ";

/* Create and initialize a new tok_state structure */

static struct tok_state *tok_new(void) {
  struct tok_state *tok =
      (struct tok_state *)PyMem_Calloc(1, sizeof(struct tok_state));
  if (tok == NULL)
    return NULL;
  tok->buf = tok->cur = tok->inp = NULL;
  tok->fp_interactive = 0;
  tok->interactive_src_start = NULL;
  tok->interactive_src_end = NULL;
  tok->start = NULL;
  tok->end = NULL;
  tok->done = E_OK;
  tok->fp = NULL;
  tok->input = NULL;
  tok->tabsize = TABSIZE;
  tok->indent = 0;
  tok->indstack[0] = 0;
  tok->atbol = 1;
  tok->pendin = 0;
  tok->prompt = tok->nextprompt = NULL;
  tok->lineno = 0;
  tok->starting_col_offset = -1;
  tok->col_offset = -1;
  tok->level = 0;
  tok->altindstack[0] = 0;
  tok->decoding_state = STATE_INIT;
  tok->decoding_erred = 0;
  tok->enc = NULL;
  tok->encoding = NULL;
  tok->cont_line = 0;
  tok->filename = NULL;
  tok->decoding_readline = NULL;
  tok->decoding_buffer = NULL;
  tok->readline = NULL;
  tok->type_comments = 0;
  tok->async_hacks = 0;
  tok->async_def = 0;
  tok->async_def_indent = 0;
  tok->async_def_nl = 0;
  tok->interactive_underflow = IUNDERFLOW_NORMAL;
  tok->str = NULL;
  tok->report_warnings = 1;
  tok->tok_extra_tokens = 0;
  tok->comment_newline = 0;
  tok->implicit_newline = 0;
  tok->tok_mode_stack[0] = (tokenizer_mode){.kind = TOK_REGULAR_MODE,
                                            .f_string_quote = '\0',
                                            .f_string_quote_size = 0,
                                            .f_string_debug = 0};
  tok->tok_mode_stack_index = 0;
#ifdef Py_DEBUG
  tok->debug = _Py_GetConfig()->parser_debug;
#endif
  return tok;
}

static char *new_string(const char *s, Py_ssize_t len, struct tok_state *tok) {
  char *result = (char *)PyMem_Malloc(len + 1);
  if (!result) {
    tok->done = E_NOMEM;
    return NULL;
  }
  memcpy(result, s, len);
  result[len] = '\0';
  return result;
}

static char *error_ret(struct tok_state *tok) /* XXX */
{
  tok->decoding_erred = 1;
  if ((tok->fp != NULL || tok->readline != NULL) &&
      tok->buf != NULL) { /* see _PyTokenizer_Free */
    PyMem_Free(tok->buf);
  }
  tok->buf = tok->cur = tok->inp = NULL;
  tok->start = NULL;
  tok->end = NULL;
  tok->done = E_DECODE;
  return NULL; /* as if it were EOF */
}

static const char *get_normal_name(const char *s) /* for utf-8 and latin-1 */
{
  char buf[13];
  int i;
  for (i = 0; i < 12; i++) {
    int c = s[i];
    if (c == '\0')
      break;
    else if (c == '_')
      buf[i] = '-';
    else
      buf[i] = tolower(c);
  }
  buf[i] = '\0';
  if (strcmp(buf, "utf-8") == 0 || strncmp(buf, "utf-8-", 6) == 0)
    return "utf-8";
  else if (strcmp(buf, "latin-1") == 0 || strcmp(buf, "iso-8859-1") == 0 ||
           strcmp(buf, "iso-latin-1") == 0 ||
           strncmp(buf, "latin-1-", 8) == 0 ||
           strncmp(buf, "iso-8859-1-", 11) == 0 ||
           strncmp(buf, "iso-latin-1-", 12) == 0)
    return "iso-8859-1";
  else
    return s;
}

/* Return the coding spec in S, or NULL if none is found.  */

static int get_coding_spec(const char *s, char **spec, Py_ssize_t size,
                           struct tok_state *tok) {
  Py_ssize_t i;
  *spec = NULL;
  /* Coding spec must be in a comment, and that comment must be
   * the only statement on the source code line. */
  for (i = 0; i < size - 6; i++) {
    if (s[i] == '#')
      break;
    if (s[i] != ' ' && s[i] != '\t' && s[i] != '\014')
      return 1;
  }
  for (; i < size - 6; i++) { /* XXX inefficient search */
    const char *t = s + i;
    if (memcmp(t, "coding", 6) == 0) {
      const char *begin = NULL;
      t += 6;
      if (t[0] != ':' && t[0] != '=')
        continue;
      do {
        t++;
      } while (t[0] == ' ' || t[0] == '\t');

      begin = t;
      while (Py_ISALNUM(t[0]) || t[0] == '-' || t[0] == '_' || t[0] == '.')
        t++;

      if (begin < t) {
        char *r = new_string(begin, t - begin, tok);
        const char *q;
        if (!r)
          return 0;
        q = get_normal_name(r);
        if (r != q) {
          PyMem_Free(r);
          r = new_string(q, strlen(q), tok);
          if (!r)
            return 0;
        }
        *spec = r;
        break;
      }
    }
  }
  return 1;
}

/* Check whether the line contains a coding spec. If it does,
   invoke the set_readline function for the new encoding.
   This function receives the tok_state and the new encoding.
   Return 1 on success, 0 on failure.  */

static int
check_coding_spec(const char *line, Py_ssize_t size, struct tok_state *tok,
                  int set_readline(struct tok_state *, const char *)) {
  char *cs;
  if (tok->cont_line) {
    /* It's a continuation line, so it can't be a coding spec. */
    tok->decoding_state = STATE_NORMAL;
    return 1;
  }
  if (!get_coding_spec(line, &cs, size, tok)) {
    return 0;
  }
  if (!cs) {
    Py_ssize_t i;
    for (i = 0; i < size; i++) {
      if (line[i] == '#' || line[i] == '\n' || line[i] == '\r')
        break;
      if (line[i] != ' ' && line[i] != '\t' && line[i] != '\014') {
        /* Stop checking coding spec after a line containing
         * anything except a comment. */
        tok->decoding_state = STATE_NORMAL;
        break;
      }
    }
    return 1;
  }
  tok->decoding_state = STATE_NORMAL;
  if (tok->encoding == NULL) {
    assert(tok->decoding_readline == NULL);
    if (strcmp(cs, "utf-8") != 0 && !set_readline(tok, cs)) {
      error_ret(tok);
      PyErr_Format(PyExc_SyntaxError, "encoding problem: %s", cs);
      PyMem_Free(cs);
      return 0;
    }
    tok->encoding = cs;
  } else { /* then, compare cs with BOM */
    if (strcmp(tok->encoding, cs) != 0) {
      error_ret(tok);
      PyErr_Format(PyExc_SyntaxError, "encoding problem: %s with BOM", cs);
      PyMem_Free(cs);
      return 0;
    }
    PyMem_Free(cs);
  }
  return 1;
}

/* See whether the file starts with a BOM. If it does,
   invoke the set_readline function with the new encoding.
   Return 1 on success, 0 on failure.  */

static int check_bom(int get_char(struct tok_state *),
                     void unget_char(int, struct tok_state *),
                     int set_readline(struct tok_state *, const char *),
                     struct tok_state *tok) {
  int ch1, ch2, ch3;
  ch1 = get_char(tok);
  tok->decoding_state = STATE_SEEK_CODING;
  if (ch1 == EOF) {
    return 1;
  } else if (ch1 == 0xEF) {
    ch2 = get_char(tok);
    if (ch2 != 0xBB) {
      unget_char(ch2, tok);
      unget_char(ch1, tok);
      return 1;
    }
    ch3 = get_char(tok);
    if (ch3 != 0xBF) {
      unget_char(ch3, tok);
      unget_char(ch2, tok);
      unget_char(ch1, tok);
      return 1;
    }
  } else {
    unget_char(ch1, tok);
    return 1;
  }
  if (tok->encoding != NULL)
    PyMem_Free(tok->encoding);
  tok->encoding = new_string("utf-8", 5, tok);
  if (!tok->encoding)
    return 0;
  /* No need to set_readline: input is already utf-8 */
  return 1;
}

static int tok_concatenate_interactive_new_line(struct tok_state *tok,
                                                const char *line) {
  assert(tok->fp_interactive);

  if (!line) {
    return 0;
  }

  Py_ssize_t current_size =
      tok->interactive_src_end - tok->interactive_src_start;
  Py_ssize_t line_size = strlen(line);
  char last_char = line[line_size > 0 ? line_size - 1 : line_size];
  if (last_char != '\n') {
    line_size += 1;
  }
  char *new_str = tok->interactive_src_start;

  new_str = PyMem_Realloc(new_str, current_size + line_size + 1);
  if (!new_str) {
    if (tok->interactive_src_start) {
      PyMem_Free(tok->interactive_src_start);
    }
    tok->interactive_src_start = NULL;
    tok->interactive_src_end = NULL;
    tok->done = E_NOMEM;
    return -1;
  }
  strcpy(new_str + current_size, line);
  tok->implicit_newline = 0;
  if (last_char != '\n') {
    /* Last line does not end in \n, fake one */
    new_str[current_size + line_size - 1] = '\n';
    new_str[current_size + line_size] = '\0';
    tok->implicit_newline = 1;
  }
  tok->interactive_src_start = new_str;
  tok->interactive_src_end = new_str + current_size + line_size;
  return 0;
}

/* Traverse and remember all f-string buffers, in order to be able to restore
   them after reallocating tok->buf */
static void remember_fstring_buffers(struct tok_state *tok) {
  int index;
  tokenizer_mode *mode;

  for (index = tok->tok_mode_stack_index; index >= 0; --index) {
    mode = &(tok->tok_mode_stack[index]);
    mode->f_string_start_offset = mode->f_string_start - tok->buf;
    mode->f_string_multi_line_start_offset =
        mode->f_string_multi_line_start - tok->buf;
  }
}

/* Traverse and restore all f-string buffers after reallocating tok->buf */
static void restore_fstring_buffers(struct tok_state *tok) {
  int index;
  tokenizer_mode *mode;

  for (index = tok->tok_mode_stack_index; index >= 0; --index) {
    mode = &(tok->tok_mode_stack[index]);
    mode->f_string_start = tok->buf + mode->f_string_start_offset;
    mode->f_string_multi_line_start =
        tok->buf + mode->f_string_multi_line_start_offset;
  }
}

static int set_fstring_expr(struct tok_state *tok, struct token *token,
                            char c) {
  assert(token != NULL);
  assert(c == '}' || c == ':' || c == '!');
  tokenizer_mode *tok_mode = TOK_GET_MODE(tok);

  if (!tok_mode->f_string_debug || token->metadata) {
    return 0;
  }

  PyObject *res = NULL;

  // Check if there is a # character in the expression
  int hash_detected = 0;
  for (Py_ssize_t i = 0; i < tok_mode->last_expr_size - tok_mode->last_expr_end;
       i++) {
    if (tok_mode->last_expr_buffer[i] == '#') {
      hash_detected = 1;
      break;
    }
  }

  if (hash_detected) {
    Py_ssize_t input_length =
        tok_mode->last_expr_size - tok_mode->last_expr_end;
    char *result = (char *)PyObject_Malloc((input_length + 1) * sizeof(char));
    if (!result) {
      return -1;
    }

    Py_ssize_t i = 0;
    Py_ssize_t j = 0;

    for (i = 0, j = 0; i < input_length; i++) {
      if (tok_mode->last_expr_buffer[i] == '#') {
        // Skip characters until newline or end of string
        while (tok_mode->last_expr_buffer[i] != '\0' && i < input_length) {
          if (tok_mode->last_expr_buffer[i] == '\n') {
            result[j++] = tok_mode->last_expr_buffer[i];
            break;
          }
          i++;
        }
      } else {
        result[j++] = tok_mode->last_expr_buffer[i];
      }
    }

    result[j] = '\0'; // Null-terminate the result string
    res = PyUnicode_DecodeUTF8(result, j, NULL);
    PyObject_Free(result);
  } else {
    res = PyUnicode_DecodeUTF8(
        tok_mode->last_expr_buffer,
        tok_mode->last_expr_size - tok_mode->last_expr_end, NULL);
  }

  if (!res) {
    return -1;
  }
  token->metadata = res;
  return 0;
}

static int update_fstring_expr(struct tok_state *tok, char cur) {
  assert(tok->cur != NULL);

  Py_ssize_t size = strlen(tok->cur);
  tokenizer_mode *tok_mode = TOK_GET_MODE(tok);

  switch (cur) {
  case 0:
    if (!tok_mode->last_expr_buffer || tok_mode->last_expr_end >= 0) {
      return 1;
    }
    char *new_buffer = PyMem_Realloc(tok_mode->last_expr_buffer,
                                     tok_mode->last_expr_size + size);
    if (new_buffer == NULL) {
      PyMem_Free(tok_mode->last_expr_buffer);
      goto error;
    }
    tok_mode->last_expr_buffer = new_buffer;
    strncpy(tok_mode->last_expr_buffer + tok_mode->last_expr_size, tok->cur,
            size);
    tok_mode->last_expr_size += size;
    break;
  case '{':
    if (tok_mode->last_expr_buffer != NULL) {
      PyMem_Free(tok_mode->last_expr_buffer);
    }
    tok_mode->last_expr_buffer = PyMem_Malloc(size);
    if (tok_mode->last_expr_buffer == NULL) {
      goto error;
    }
    tok_mode->last_expr_size = size;
    tok_mode->last_expr_end = -1;
    strncpy(tok_mode->last_expr_buffer, tok->cur, size);
    break;
  case '}':
  case '!':
    tok_mode->last_expr_end = strlen(tok->start);
  case ':':
    if (tok_mode->last_expr_end == -1) {
        tok_mode->last_expr_end = strlen(tok->start);
    }
    break;
  default:
    Py_UNREACHABLE();
  }
  return 1;
error:
  tok->done = E_NOMEM;
  return 0;
}

static void free_fstring_expressions(struct tok_state *tok) {
  int index;
  tokenizer_mode *mode;

  for (index = tok->tok_mode_stack_index; index >= 0; --index) {
    mode = &(tok->tok_mode_stack[index]);
    if (mode->last_expr_buffer != NULL) {
      PyMem_Free(mode->last_expr_buffer);
      mode->last_expr_buffer = NULL;
      mode->last_expr_size = 0;
      mode->last_expr_end = -1;
      mode->in_format_spec = 0;
    }
  }
}

/* Read a line of text from TOK into S, using the stream in TOK.
   Return NULL on failure, else S.

   On entry, tok->decoding_buffer will be one of:
     1) NULL: need to call tok->decoding_readline to get a new line
     2) PyUnicodeObject *: decoding_feof has called tok->decoding_readline and
       stored the result in tok->decoding_buffer
     3) PyByteArrayObject *: previous call to tok_readline_recode did not have
   enough room (in the s buffer) to copy entire contents of the line read by
   tok->decoding_readline.  tok->decoding_buffer has the overflow. In this case,
   tok_readline_recode is called in a loop (with an expanded buffer) until the
   buffer ends with a '\n' (or until the end of the file is reached): see
   tok_nextc and its calls to tok_reserve_buf.
*/

static int tok_reserve_buf(struct tok_state *tok, Py_ssize_t size) {
  Py_ssize_t cur = tok->cur - tok->buf;
  Py_ssize_t oldsize = tok->inp - tok->buf;
  Py_ssize_t newsize = oldsize + Py_MAX(size, oldsize >> 1);
  if (newsize > tok->end - tok->buf) {
    char *newbuf = tok->buf;
    Py_ssize_t start = tok->start == NULL ? -1 : tok->start - tok->buf;
    Py_ssize_t line_start =
        tok->start == NULL ? -1 : tok->line_start - tok->buf;
    Py_ssize_t multi_line_start = tok->multi_line_start - tok->buf;
    remember_fstring_buffers(tok);
    newbuf = (char *)PyMem_Realloc(newbuf, newsize);
    if (newbuf == NULL) {
      tok->done = E_NOMEM;
      return 0;
    }
    tok->buf = newbuf;
    tok->cur = tok->buf + cur;
    tok->inp = tok->buf + oldsize;
    tok->end = tok->buf + newsize;
    tok->start = start < 0 ? NULL : tok->buf + start;
    tok->line_start = line_start < 0 ? NULL : tok->buf + line_start;
    tok->multi_line_start =
        multi_line_start < 0 ? NULL : tok->buf + multi_line_start;
    restore_fstring_buffers(tok);
  }
  return 1;
}

static inline int contains_null_bytes(const char *str, size_t size) {
  return memchr(str, 0, size) != NULL;
}

static int tok_readline_recode(struct tok_state *tok) {
  PyObject *line;
  const char *buf;
  Py_ssize_t buflen;
  line = tok->decoding_buffer;
  if (line == NULL) {
    line = PyObject_CallNoArgs(tok->decoding_readline);
    if (line == NULL) {
      error_ret(tok);
      goto error;
    }
  } else {
    tok->decoding_buffer = NULL;
  }
  buf = PyUnicode_AsUTF8AndSize(line, &buflen);
  if (buf == NULL) {
    error_ret(tok);
    goto error;
  }
  // Make room for the null terminator *and* potentially
  // an extra newline character that we may need to artificially
  // add.
  size_t buffer_size = buflen + 2;
  if (!tok_reserve_buf(tok, buffer_size)) {
    goto error;
  }
  memcpy(tok->inp, buf, buflen);
  tok->inp += buflen;
  *tok->inp = '\0';
  if (tok->fp_interactive &&
      tok_concatenate_interactive_new_line(tok, buf) == -1) {
    goto error;
  }
  Py_DECREF(line);
  return 1;
error:
  Py_XDECREF(line);
  return 0;
}

/* Set the readline function for TOK to a StreamReader's
   readline function. The StreamReader is named ENC.

   This function is called from check_bom and check_coding_spec.

   ENC is usually identical to the future value of tok->encoding,
   except for the (currently unsupported) case of UTF-16.

   Return 1 on success, 0 on failure. */

static int fp_setreadl(struct tok_state *tok, const char *enc) {
  PyObject *readline, *open, *stream;
  int fd;
  long pos;

  fd = fileno(tok->fp);
  /* Due to buffering the file offset for fd can be different from the file
   * position of tok->fp.  If tok->fp was opened in text mode on Windows,
   * its file position counts CRLF as one char and can't be directly mapped
   * to the file offset for fd.  Instead we step back one byte and read to
   * the end of line.*/
  pos = ftell(tok->fp);
  if (pos == -1 ||
      lseek(fd, (off_t)(pos > 0 ? pos - 1 : pos), SEEK_SET) == (off_t)-1) {
    PyErr_SetFromErrnoWithFilename(PyExc_OSError, NULL);
    return 0;
  }

  open = _PyImport_GetModuleAttrString("io", "open");
  if (open == NULL) {
    return 0;
  }
  stream = PyObject_CallFunction(open, "isisOOO", fd, "r", -1, enc, Py_None,
                                 Py_None, Py_False);
  Py_DECREF(open);
  if (stream == NULL) {
    return 0;
  }

  readline = PyObject_GetAttr(stream, &_Py_ID(readline));
  Py_DECREF(stream);
  if (readline == NULL) {
    return 0;
  }
  Py_XSETREF(tok->decoding_readline, readline);

  if (pos > 0) {
    PyObject *bufobj = _PyObject_CallNoArgs(readline);
    if (bufobj == NULL) {
      return 0;
    }
    Py_DECREF(bufobj);
  }

  return 1;
}

/* Fetch the next byte from TOK. */

static int fp_getc(struct tok_state *tok) { return getc(tok->fp); }

/* Unfetch the last byte back into TOK.  */

static void fp_ungetc(int c, struct tok_state *tok) { ungetc(c, tok->fp); }

/* Check whether the characters at s start a valid
   UTF-8 sequence. Return the number of characters forming
   the sequence if yes, 0 if not.  The special cases match
   those in stringlib/codecs.h:utf8_decode.
*/
static int valid_utf8(const unsigned char *s) {
  int expected = 0;
  int length;
  if (*s < 0x80) {
    /* single-byte code */
    return 1;
  } else if (*s < 0xE0) {
    /* \xC2\x80-\xDF\xBF -- 0080-07FF */
    if (*s < 0xC2) {
      /* invalid sequence
         \x80-\xBF -- continuation byte
         \xC0-\xC1 -- fake 0000-007F */
      return 0;
    }
    expected = 1;
  } else if (*s < 0xF0) {
    /* \xE0\xA0\x80-\xEF\xBF\xBF -- 0800-FFFF */
    if (*s == 0xE0 && *(s + 1) < 0xA0) {
      /* invalid sequence
         \xE0\x80\x80-\xE0\x9F\xBF -- fake 0000-0800 */
      return 0;
    } else if (*s == 0xED && *(s + 1) >= 0xA0) {
      /* Decoding UTF-8 sequences in range \xED\xA0\x80-\xED\xBF\xBF
         will result in surrogates in range D800-DFFF. Surrogates are
         not valid UTF-8 so they are rejected.
         See https://www.unicode.org/versions/Unicode5.2.0/ch03.pdf
         (table 3-7) and http://www.rfc-editor.org/rfc/rfc3629.txt */
      return 0;
    }
    expected = 2;
  } else if (*s < 0xF5) {
    /* \xF0\x90\x80\x80-\xF4\x8F\xBF\xBF -- 10000-10FFFF */
    if (*(s + 1) < 0x90 ? *s == 0xF0 : *s == 0xF4) {
      /* invalid sequence -- one of:
         \xF0\x80\x80\x80-\xF0\x8F\xBF\xBF -- fake 0000-FFFF
         \xF4\x90\x80\x80- -- 110000- overflow */
      return 0;
    }
    expected = 3;
  } else {
    /* invalid start byte */
    return 0;
  }
  length = expected + 1;
  for (; expected; expected--)
    if (s[expected] < 0x80 || s[expected] >= 0xC0)
      return 0;
  return length;
}

static int ensure_utf8(char *line, struct tok_state *tok) {
  int badchar = 0;
  unsigned char *c;
  int length;
  for (c = (unsigned char *)line; *c; c += length) {
    if (!(length = valid_utf8(c))) {
      badchar = *c;
      break;
    }
  }
  if (badchar) {
    PyErr_Format(PyExc_SyntaxError,
                 "Non-UTF-8 code starting with '\\x%.2x' "
                 "in file %U on line %i, "
                 "but no encoding declared; "
                 "see https://peps.python.org/pep-0263/ for details",
                 badchar, tok->filename, tok->lineno);
    return 0;
  }
  return 1;
}

/* Fetch a byte from TOK, using the string buffer. */

static int buf_getc(struct tok_state *tok) { return Py_CHARMASK(*tok->str++); }

/* Unfetch a byte from TOK, using the string buffer. */

static void buf_ungetc(int c, struct tok_state *tok) {
  tok->str--;
  assert(Py_CHARMASK(*tok->str) ==
         c); /* tok->cur may point to read-only segment */
}

/* Set the readline function for TOK to ENC. For the string-based
   tokenizer, this means to just record the encoding. */

static int buf_setreadl(struct tok_state *tok, const char *enc) {
  tok->enc = enc;
  return 1;
}

/* Return a UTF-8 encoding Python string object from the
   C byte string STR, which is encoded with ENC. */

static PyObject *translate_into_utf8(const char *str, const char *enc) {
  PyObject *utf8;
  PyObject *buf = PyUnicode_Decode(str, strlen(str), enc, NULL);
  if (buf == NULL)
    return NULL;
  utf8 = PyUnicode_AsUTF8String(buf);
  Py_DECREF(buf);
  return utf8;
}

static char *translate_newlines(const char *s, int exec_input,
                                int preserve_crlf, struct tok_state *tok) {
  int skip_next_lf = 0;
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
    __msan_unpoison_string(s);
#  endif
#endif
  size_t needed_length = strlen(s) + 2, final_length;
  char *buf, *current;
  char c = '\0';
  buf = PyMem_Malloc(needed_length);
  if (buf == NULL) {
    tok->done = E_NOMEM;
    return NULL;
  }
  for (current = buf; *s; s++, current++) {
    c = *s;
    if (skip_next_lf) {
      skip_next_lf = 0;
      if (c == '\n') {
        c = *++s;
        if (!c)
          break;
      }
    }
    if (!preserve_crlf && c == '\r') {
      skip_next_lf = 1;
      c = '\n';
    }
    *current = c;
  }
  /* If this is exec input, add a newline to the end of the string if
     there isn't one already. */
  if (exec_input && c != '\n' && c != '\0') {
    *current = '\n';
    current++;
  }
  *current = '\0';
  final_length = current - buf + 1;
  if (final_length < needed_length && final_length) {
    /* should never fail */
    char *result = PyMem_Realloc(buf, final_length);
    if (result == NULL) {
      PyMem_Free(buf);
    }
    buf = result;
  }
  return buf;
}

/* Decode a byte string STR for use as the buffer of TOK.
   Look for encoding declarations inside STR, and record them
   inside TOK.  */

static char *decode_str(const char *input, int single, struct tok_state *tok,
                        int preserve_crlf) {
  PyObject *utf8 = NULL;
  char *str;
  const char *s;
  const char *newl[2] = {NULL, NULL};
  int lineno = 0;
  tok->input = str = translate_newlines(input, single, preserve_crlf, tok);
  if (str == NULL)
    return NULL;
  tok->enc = NULL;
  tok->str = str;
  if (!check_bom(buf_getc, buf_ungetc, buf_setreadl, tok))
    return error_ret(tok);
  str = tok->str; /* string after BOM if any */
  assert(str);
  if (tok->enc != NULL) {
    utf8 = translate_into_utf8(str, tok->enc);
    if (utf8 == NULL)
      return error_ret(tok);
    str = PyBytes_AsString(utf8);
  }
  for (s = str;; s++) {
    if (*s == '\0')
      break;
    else if (*s == '\n') {
      assert(lineno < 2);
      newl[lineno] = s;
      lineno++;
      if (lineno == 2)
        break;
    }
  }
  tok->enc = NULL;
  /* need to check line 1 and 2 separately since check_coding_spec
     assumes a single line as input */
  if (newl[0]) {
    if (!check_coding_spec(str, newl[0] - str, tok, buf_setreadl)) {
      return NULL;
    }
    if (tok->enc == NULL && tok->decoding_state != STATE_NORMAL && newl[1]) {
      if (!check_coding_spec(newl[0] + 1, newl[1] - newl[0], tok, buf_setreadl))
        return NULL;
    }
  }
  if (tok->enc != NULL) {
    assert(utf8 == NULL);
    utf8 = translate_into_utf8(str, tok->enc);
    if (utf8 == NULL)
      return error_ret(tok);
    str = PyBytes_AS_STRING(utf8);
  }
  assert(tok->decoding_buffer == NULL);
  tok->decoding_buffer = utf8; /* CAUTION */
  return str;
}

/* Set up tokenizer for string */

struct tok_state *_PyTokenizer_FromString(const char *str, int exec_input,
                                          int preserve_crlf) {
  struct tok_state *tok = tok_new();
  char *decoded;

  if (tok == NULL)
    return NULL;
  decoded = decode_str(str, exec_input, tok, preserve_crlf);
  if (decoded == NULL) {
    _PyTokenizer_Free(tok);
    return NULL;
  }

  tok->buf = tok->cur = tok->inp = decoded;
  tok->end = decoded;
  return tok;
}

struct tok_state *_PyTokenizer_FromReadline(PyObject *readline, const char *enc,
                                            int exec_input, int preserve_crlf) {
  struct tok_state *tok = tok_new();
  if (tok == NULL)
    return NULL;
  if ((tok->buf = (char *)PyMem_Malloc(BUFSIZ)) == NULL) {
    _PyTokenizer_Free(tok);
    return NULL;
  }
  tok->cur = tok->inp = tok->buf;
  tok->end = tok->buf + BUFSIZ;
  tok->fp = NULL;
  if (enc != NULL) {
    tok->encoding = new_string(enc, strlen(enc), tok);
    if (!tok->encoding) {
      _PyTokenizer_Free(tok);
      return NULL;
    }
  }
  tok->decoding_state = STATE_NORMAL;
  Py_INCREF(readline);
  tok->readline = readline;
  return tok;
}

/* Set up tokenizer for UTF-8 string */

struct tok_state *_PyTokenizer_FromUTF8(const char *str, int exec_input,
                                        int preserve_crlf) {
  struct tok_state *tok = tok_new();
  char *translated;
  if (tok == NULL)
    return NULL;
  tok->input = translated =
      translate_newlines(str, exec_input, preserve_crlf, tok);
  if (translated == NULL) {
    _PyTokenizer_Free(tok);
    return NULL;
  }
  tok->decoding_state = STATE_NORMAL;
  tok->enc = NULL;
  tok->str = translated;
  tok->encoding = new_string("utf-8", 5, tok);
  if (!tok->encoding) {
    _PyTokenizer_Free(tok);
    return NULL;
  }

  tok->buf = tok->cur = tok->inp = translated;
  tok->end = translated;
  return tok;
}

/* Set up tokenizer for file */

struct tok_state *_PyTokenizer_FromFile(FILE *fp, const char *enc,
                                        const char *ps1, const char *ps2) {
  struct tok_state *tok = tok_new();
  if (tok == NULL)
    return NULL;
  if ((tok->buf = (char *)PyMem_Malloc(BUFSIZ)) == NULL) {
    _PyTokenizer_Free(tok);
    return NULL;
  }
  tok->cur = tok->inp = tok->buf;
  tok->end = tok->buf + BUFSIZ;
  tok->fp = fp;
  tok->prompt = ps1;
  tok->nextprompt = ps2;
  if (enc != NULL) {
    /* Must copy encoding declaration since it
       gets copied into the parse tree. */
    tok->encoding = new_string(enc, strlen(enc), tok);
    if (!tok->encoding) {
      _PyTokenizer_Free(tok);
      return NULL;
    }
    tok->decoding_state = STATE_NORMAL;
  }
  return tok;
}

/* Free a tok_state structure */

void _PyTokenizer_Free(struct tok_state *tok) {
  if (tok->encoding != NULL) {
    PyMem_Free(tok->encoding);
  }
  Py_XDECREF(tok->decoding_readline);
  Py_XDECREF(tok->decoding_buffer);
  Py_XDECREF(tok->readline);
  Py_XDECREF(tok->filename);
  if ((tok->readline != NULL || tok->fp != NULL) && tok->buf != NULL) {
    PyMem_Free(tok->buf);
  }
  if (tok->input) {
    PyMem_Free(tok->input);
  }
  if (tok->interactive_src_start != NULL) {
    PyMem_Free(tok->interactive_src_start);
  }
  free_fstring_expressions(tok);
  PyMem_Free(tok);
}

void _PyToken_Free(struct token *token) { Py_XDECREF(token->metadata); }

void _PyToken_Init(struct token *token) { token->metadata = NULL; }

static int tok_readline_raw(struct tok_state *tok) {
  do {
    if (!tok_reserve_buf(tok, BUFSIZ)) {
      return 0;
    }
    int n_chars = (int)(tok->end - tok->inp);
    size_t line_size = 0;
    char *line = _Py_UniversalNewlineFgetsWithSize(tok->inp, n_chars, tok->fp,
                                                   NULL, &line_size);
    if (line == NULL) {
      return 1;
    }
    if (tok->fp_interactive &&
        tok_concatenate_interactive_new_line(tok, line) == -1) {
      return 0;
    }
    tok->inp += line_size;
    if (tok->inp == tok->buf) {
      return 0;
    }
  } while (tok->inp[-1] != '\n');
  return 1;
}

static int tok_readline_string(struct tok_state *tok) {
  PyObject *line = NULL;
  PyObject *raw_line = PyObject_CallNoArgs(tok->readline);
  if (raw_line == NULL) {
    if (PyErr_ExceptionMatches(PyExc_StopIteration)) {
      PyErr_Clear();
      return 1;
    }
    error_ret(tok);
    goto error;
  }
  if (tok->encoding != NULL) {
    if (!PyBytes_Check(raw_line)) {
      PyErr_Format(PyExc_TypeError, "readline() returned a non-bytes object");
      error_ret(tok);
      goto error;
    }
    line =
        PyUnicode_Decode(PyBytes_AS_STRING(raw_line),
                         PyBytes_GET_SIZE(raw_line), tok->encoding, "replace");
    Py_CLEAR(raw_line);
    if (line == NULL) {
      error_ret(tok);
      goto error;
    }
  } else {
    if (!PyUnicode_Check(raw_line)) {
      PyErr_Format(PyExc_TypeError, "readline() returned a non-string object");
      error_ret(tok);
      goto error;
    }
    line = raw_line;
    raw_line = NULL;
  }
  Py_ssize_t buflen;
  const char *buf = PyUnicode_AsUTF8AndSize(line, &buflen);
  if (buf == NULL) {
    error_ret(tok);
    goto error;
  }

  // Make room for the null terminator *and* potentially
  // an extra newline character that we may need to artificially
  // add.
  size_t buffer_size = buflen + 2;
  if (!tok_reserve_buf(tok, buffer_size)) {
    goto error;
  }
  memcpy(tok->inp, buf, buflen);
  tok->inp += buflen;
  *tok->inp = '\0';

  tok->line_start = tok->cur;
  Py_DECREF(line);
  return 1;
error:
  Py_XDECREF(raw_line);
  Py_XDECREF(line);
  return 0;
}

static int tok_underflow_string(struct tok_state *tok) {
  char *end = strchr(tok->inp, '\n');
  if (end != NULL) {
    end++;
  } else {
    end = strchr(tok->inp, '\0');
    if (end == tok->inp) {
      tok->done = E_EOF;
      return 0;
    }
  }
  if (tok->start == NULL) {
    tok->buf = tok->cur;
  }
  tok->line_start = tok->cur;
  ADVANCE_LINENO();
  tok->inp = end;
  return 1;
}

static int tok_underflow_interactive(struct tok_state *tok) {
  if (tok->interactive_underflow == IUNDERFLOW_STOP) {
    tok->done = E_INTERACT_STOP;
    return 1;
  }
  char *newtok = PyOS_Readline(tok->fp ? tok->fp : stdin, stdout, tok->prompt);
  if (newtok != NULL) {
    char *translated = translate_newlines(newtok, 0, 0, tok);
    PyMem_Free(newtok);
    if (translated == NULL) {
      return 0;
    }
    newtok = translated;
  }
  if (tok->encoding && newtok && *newtok) {
    /* Recode to UTF-8 */
    Py_ssize_t buflen;
    const char *buf;
    PyObject *u = translate_into_utf8(newtok, tok->encoding);
    PyMem_Free(newtok);
    if (u == NULL) {
      tok->done = E_DECODE;
      return 0;
    }
    buflen = PyBytes_GET_SIZE(u);
    buf = PyBytes_AS_STRING(u);
    newtok = PyMem_Malloc(buflen + 1);
    if (newtok == NULL) {
      Py_DECREF(u);
      tok->done = E_NOMEM;
      return 0;
    }
    strcpy(newtok, buf);
    Py_DECREF(u);
  }
  if (tok->fp_interactive &&
      tok_concatenate_interactive_new_line(tok, newtok) == -1) {
    PyMem_Free(newtok);
    return 0;
  }
  if (tok->nextprompt != NULL) {
    tok->prompt = tok->nextprompt;
  }
  if (newtok == NULL) {
    tok->done = E_INTR;
  } else if (*newtok == '\0') {
    PyMem_Free(newtok);
    tok->done = E_EOF;
  } else if (tok->start != NULL) {
    Py_ssize_t cur_multi_line_start = tok->multi_line_start - tok->buf;
    remember_fstring_buffers(tok);
    size_t size = strlen(newtok);
    ADVANCE_LINENO();
    if (!tok_reserve_buf(tok, size + 1)) {
      PyMem_Free(tok->buf);
      tok->buf = NULL;
      PyMem_Free(newtok);
      return 0;
    }
    memcpy(tok->cur, newtok, size + 1);
    PyMem_Free(newtok);
    tok->inp += size;
    tok->multi_line_start = tok->buf + cur_multi_line_start;
    restore_fstring_buffers(tok);
  } else {
    remember_fstring_buffers(tok);
    ADVANCE_LINENO();
    PyMem_Free(tok->buf);
    tok->buf = newtok;
    tok->cur = tok->buf;
    tok->line_start = tok->buf;
    tok->inp = strchr(tok->buf, '\0');
    tok->end = tok->inp + 1;
    restore_fstring_buffers(tok);
  }
  if (tok->done != E_OK) {
    if (tok->prompt != NULL) {
      PySys_WriteStderr("\n");
    }
    return 0;
  }

  if (tok->tok_mode_stack_index && !update_fstring_expr(tok, 0)) {
    return 0;
  }
  return 1;
}

static int tok_underflow_file(struct tok_state *tok) {
  if (tok->start == NULL && !INSIDE_FSTRING(tok)) {
    tok->cur = tok->inp = tok->buf;
  }
  if (tok->decoding_state == STATE_INIT) {
    /* We have not yet determined the encoding.
       If an encoding is found, use the file-pointer
       reader functions from now on. */
    if (!check_bom(fp_getc, fp_ungetc, fp_setreadl, tok)) {
      error_ret(tok);
      return 0;
    }
    assert(tok->decoding_state != STATE_INIT);
  }
  /* Read until '\n' or EOF */
  if (tok->decoding_readline != NULL) {
    /* We already have a codec associated with this input. */
    if (!tok_readline_recode(tok)) {
      return 0;
    }
  } else {
    /* We want a 'raw' read. */
    if (!tok_readline_raw(tok)) {
      return 0;
    }
  }
  if (tok->inp == tok->cur) {
    tok->done = E_EOF;
    return 0;
  }
  tok->implicit_newline = 0;
  if (tok->inp[-1] != '\n') {
    assert(tok->inp + 1 < tok->end);
    /* Last line does not end in \n, fake one */
    *tok->inp++ = '\n';
    *tok->inp = '\0';
    tok->implicit_newline = 1;
  }

  if (tok->tok_mode_stack_index && !update_fstring_expr(tok, 0)) {
    return 0;
  }

  ADVANCE_LINENO();
  if (tok->decoding_state != STATE_NORMAL) {
    if (tok->lineno > 2) {
      tok->decoding_state = STATE_NORMAL;
    } else if (!check_coding_spec(tok->cur, strlen(tok->cur), tok,
                                  fp_setreadl)) {
      return 0;
    }
  }
  /* The default encoding is UTF-8, so make sure we don't have any
     non-UTF-8 sequences in it. */
  if (!tok->encoding && !ensure_utf8(tok->cur, tok)) {
    error_ret(tok);
    return 0;
  }
  assert(tok->done == E_OK);
  return tok->done == E_OK;
}

static int tok_underflow_readline(struct tok_state *tok) {
  assert(tok->decoding_state == STATE_NORMAL);
  assert(tok->fp == NULL && tok->input == NULL &&
         tok->decoding_readline == NULL);
  if (tok->start == NULL && !INSIDE_FSTRING(tok)) {
    tok->cur = tok->inp = tok->buf;
  }
  if (!tok_readline_string(tok)) {
    return 0;
  }
  if (tok->inp == tok->cur) {
    tok->done = E_EOF;
    return 0;
  }
  tok->implicit_newline = 0;
  if (tok->inp[-1] != '\n') {
    assert(tok->inp + 1 < tok->end);
    /* Last line does not end in \n, fake one */
    *tok->inp++ = '\n';
    *tok->inp = '\0';
    tok->implicit_newline = 1;
  }

  if (tok->tok_mode_stack_index && !update_fstring_expr(tok, 0)) {
    return 0;
  }

  ADVANCE_LINENO();
  /* The default encoding is UTF-8, so make sure we don't have any
     non-UTF-8 sequences in it. */
  if (!tok->encoding && !ensure_utf8(tok->cur, tok)) {
    error_ret(tok);
    return 0;
  }
  assert(tok->done == E_OK);
  return tok->done == E_OK;
}

#if defined(Py_DEBUG)
static void print_escape(FILE *f, const char *s, Py_ssize_t size) {
  if (s == NULL) {
    fputs("NULL", f);
    return;
  }
  putc('"', f);
  while (size-- > 0) {
    unsigned char c = *s++;
    switch (c) {
    case '\n':
      fputs("\\n", f);
      break;
    case '\r':
      fputs("\\r", f);
      break;
    case '\t':
      fputs("\\t", f);
      break;
    case '\f':
      fputs("\\f", f);
      break;
    case '\'':
      fputs("\\'", f);
      break;
    case '"':
      fputs("\\\"", f);
      break;
    default:
      if (0x20 <= c && c <= 0x7f)
        putc(c, f);
      else
        fprintf(f, "\\x%02x", c);
    }
  }
  putc('"', f);
}
#endif

/* Get next char, updating state; error code goes into tok->done */

static int tok_nextc(struct tok_state *tok) {
  int rc;
  for (;;) {
    if (tok->cur != tok->inp) {
      if ((unsigned int)tok->col_offset >= (unsigned int)INT_MAX) {
        tok->done = E_COLUMNOVERFLOW;
        return EOF;
      }
      tok->col_offset++;
      return Py_CHARMASK(*tok->cur++); /* Fast path */
    }
    if (tok->done != E_OK) {
      return EOF;
    }
    if (tok->readline) {
      rc = tok_underflow_readline(tok);
    } else if (tok->fp == NULL) {
      rc = tok_underflow_string(tok);
    } else if (tok->prompt != NULL) {
      rc = tok_underflow_interactive(tok);
    } else {
      rc = tok_underflow_file(tok);
    }
#if defined(Py_DEBUG)
    if (tok->debug) {
      fprintf(stderr, "line[%d] = ", tok->lineno);
      print_escape(stderr, tok->cur, tok->inp - tok->cur);
      fprintf(stderr, "  tok->done = %d\n", tok->done);
    }
#endif
    if (!rc) {
      tok->cur = tok->inp;
      return EOF;
    }
    tok->line_start = tok->cur;

    if (contains_null_bytes(tok->line_start, tok->inp - tok->line_start)) {
      syntaxerror(tok, "source code cannot contain null bytes");
      tok->cur = tok->inp;
      return EOF;
    }
  }
  Py_UNREACHABLE();
}

/* Back-up one character */

static void tok_backup(struct tok_state *tok, int c) {
  if (c != EOF) {
    if (--tok->cur < tok->buf) {
      Py_FatalError("tokenizer beginning of buffer");
    }
    if ((int)(unsigned char)*tok->cur != Py_CHARMASK(c)) {
      Py_FatalError("tok_backup: wrong character");
    }
    tok->col_offset--;
  }
}

static int _syntaxerror_range(struct tok_state *tok, const char *format,
                              int col_offset, int end_col_offset,
                              va_list vargs) {
  // In release builds, we don't want to overwrite a previous error, but in
  // debug builds we want to fail if we are not doing it so we can fix it.
  assert(tok->done != E_ERROR);
  if (tok->done == E_ERROR) {
    return ERRORTOKEN;
  }
  PyObject *errmsg, *errtext, *args;
  errmsg = PyUnicode_FromFormatV(format, vargs);
  if (!errmsg) {
    goto error;
  }

  errtext = PyUnicode_DecodeUTF8(tok->line_start, tok->cur - tok->line_start,
                                 "replace");
  if (!errtext) {
    goto error;
  }

  if (col_offset == -1) {
    col_offset = (int)PyUnicode_GET_LENGTH(errtext);
  }
  if (end_col_offset == -1) {
    end_col_offset = col_offset;
  }

  Py_ssize_t line_len = strcspn(tok->line_start, "\n");
  if (line_len != tok->cur - tok->line_start) {
    Py_DECREF(errtext);
    errtext = PyUnicode_DecodeUTF8(tok->line_start, line_len, "replace");
  }
  if (!errtext) {
    goto error;
  }

  args = Py_BuildValue("(O(OiiNii))", errmsg, tok->filename, tok->lineno,
                       col_offset, errtext, tok->lineno, end_col_offset);
  if (args) {
    PyErr_SetObject(PyExc_SyntaxError, args);
    Py_DECREF(args);
  }

error:
  Py_XDECREF(errmsg);
  tok->done = E_ERROR;
  return ERRORTOKEN;
}

static int syntaxerror(struct tok_state *tok, const char *format, ...) {
  // This errors are cleaned on startup. Todo: Fix it.
  va_list vargs;
  va_start(vargs, format);
  int ret = _syntaxerror_range(tok, format, -1, -1, vargs);
  va_end(vargs);
  return ret;
}

static int syntaxerror_known_range(struct tok_state *tok, int col_offset,
                                   int end_col_offset, const char *format,
                                   ...) {
  va_list vargs;
  va_start(vargs, format);
  int ret = _syntaxerror_range(tok, format, col_offset, end_col_offset, vargs);
  va_end(vargs);
  return ret;
}

static int indenterror(struct tok_state *tok) {
  tok->done = E_TABSPACE;
  tok->cur = tok->inp;
  return ERRORTOKEN;
}

static int parser_warn(struct tok_state *tok, PyObject *category,
                       const char *format, ...) {
  if (!tok->report_warnings) {
    return 0;
  }

  PyObject *errmsg;
  va_list vargs;
  va_start(vargs, format);
  errmsg = PyUnicode_FromFormatV(format, vargs);
  va_end(vargs);
  if (!errmsg) {
    goto error;
  }

  if (PyErr_WarnExplicitObject(category, errmsg, tok->filename, tok->lineno,
                               NULL, NULL) < 0) {
    if (PyErr_ExceptionMatches(category)) {
      /* Replace the DeprecationWarning exception with a SyntaxError
         to get a more accurate error report */
      PyErr_Clear();
      syntaxerror(tok, "%U", errmsg);
    }
    goto error;
  }
  Py_DECREF(errmsg);
  return 0;

error:
  Py_XDECREF(errmsg);
  tok->done = E_ERROR;
  return -1;
}

static int warn_invalid_escape_sequence(struct tok_state *tok,
                                        int first_invalid_escape_char) {
  if (!tok->report_warnings) {
    return 0;
  }

  PyObject *msg = PyUnicode_FromFormat("invalid escape sequence '\\%c'",
                                       (char)first_invalid_escape_char);

  if (msg == NULL) {
    return -1;
  }

  if (PyErr_WarnExplicitObject(PyExc_SyntaxWarning, msg, tok->filename,
                               tok->lineno, NULL, NULL) < 0) {
    Py_DECREF(msg);

    if (PyErr_ExceptionMatches(PyExc_SyntaxWarning)) {
      /* Replace the SyntaxWarning exception with a SyntaxError
         to get a more accurate error report */
      PyErr_Clear();
      return syntaxerror(tok, "invalid escape sequence '\\%c'",
                         (char)first_invalid_escape_char);
    }

    return -1;
  }

  Py_DECREF(msg);
  return 0;
}

static int lookahead(struct tok_state *tok, const char *test) {
  const char *s = test;
  int res = 0;
  while (1) {
    int c = tok_nextc(tok);
    if (*s == 0) {
      res = !is_potential_identifier_char(c);
    } else if (c == *s) {
      s++;
      continue;
    }

    tok_backup(tok, c);
    while (s != test) {
      tok_backup(tok, *--s);
    }
    return res;
  }
}

static int verify_end_of_number(struct tok_state *tok, int c,
                                const char *kind) {
  if (tok->tok_extra_tokens) {
    // When we are parsing extra tokens, we don't want to emit warnings
    // about invalid literals, because we want to be a bit more liberal.
    return 1;
  }
  /* Emit a deprecation warning only if the numeric literal is immediately
   * followed by one of keywords which can occur after a numeric literal
   * in valid code: "and", "else", "for", "if", "in", "is" and "or".
   * It allows to gradually deprecate existing valid code without adding
   * warning before error in most cases of invalid numeric literal (which
   * would be confusing and break existing tests).
   * Raise a syntax error with slightly better message than plain
   * "invalid syntax" if the numeric literal is immediately followed by
   * other keyword or identifier.
   */
  int r = 0;
  if (c == 'a') {
    r = lookahead(tok, "nd");
  } else if (c == 'e') {
    r = lookahead(tok, "lse");
  } else if (c == 'f') {
    r = lookahead(tok, "or");
  } else if (c == 'i') {
    int c2 = tok_nextc(tok);
    if (c2 == 'f' || c2 == 'n' || c2 == 's') {
      r = 1;
    }
    tok_backup(tok, c2);
  } else if (c == 'o') {
    r = lookahead(tok, "r");
  } else if (c == 'n') {
    r = lookahead(tok, "ot");
  }
  if (r) {
    tok_backup(tok, c);
    if (parser_warn(tok, PyExc_SyntaxWarning, "invalid %s literal", kind)) {
      return 0;
    }
    tok_nextc(tok);
  } else /* In future releases, only error will remain. */
    if (c < 128 && is_potential_identifier_char(c)) {
      tok_backup(tok, c);
      syntaxerror(tok, "invalid %s literal", kind);
      return 0;
    }
  return 1;
}

/* Verify that the identifier follows PEP 3131.
   All identifier strings are guaranteed to be "ready" unicode objects.
 */
static int verify_identifier(struct tok_state *tok) {
  if (tok->tok_extra_tokens) {
    return 1;
  }
  PyObject *s;
  if (tok->decoding_erred)
    return 0;
  s = PyUnicode_DecodeUTF8(tok->start, tok->cur - tok->start, NULL);
  if (s == NULL) {
    if (PyErr_ExceptionMatches(PyExc_UnicodeDecodeError)) {
      tok->done = E_DECODE;
    } else {
      tok->done = E_ERROR;
    }
    return 0;
  }
  Py_ssize_t invalid = _PyUnicode_ScanIdentifier(s);
  if (invalid < 0) {
    Py_DECREF(s);
    tok->done = E_ERROR;
    return 0;
  }
  assert(PyUnicode_GET_LENGTH(s) > 0);
  if (invalid < PyUnicode_GET_LENGTH(s)) {
    Py_UCS4 ch = PyUnicode_READ_CHAR(s, invalid);
    if (invalid + 1 < PyUnicode_GET_LENGTH(s)) {
      /* Determine the offset in UTF-8 encoded input */
      Py_SETREF(s, PyUnicode_Substring(s, 0, invalid + 1));
      if (s != NULL) {
        Py_SETREF(s, PyUnicode_AsUTF8String(s));
      }
      if (s == NULL) {
        tok->done = E_ERROR;
        return 0;
      }
      tok->cur = (char *)tok->start + PyBytes_GET_SIZE(s);
    }
    Py_DECREF(s);
    if (Py_UNICODE_ISPRINTABLE(ch)) {
      syntaxerror(tok, "invalid character '%c' (U+%04X)", ch, ch);
    } else {
      syntaxerror(tok, "invalid non-printable character U+%04X", ch);
    }
    return 0;
  }
  Py_DECREF(s);
  return 1;
}

static int tok_decimal_tail(struct tok_state *tok) {
  int c;

  while (1) {
    do {
      c = tok_nextc(tok);
    } while (isdigit(c));
    if (c != '_') {
      break;
    }
    c = tok_nextc(tok);
    if (!isdigit(c)) {
      tok_backup(tok, c);
      syntaxerror(tok, "invalid decimal literal");
      return 0;
    }
  }
  return c;
}

static inline int tok_continuation_line(struct tok_state *tok) {
  int c = tok_nextc(tok);
  if (c == '\r') {
    c = tok_nextc(tok);
  }
  if (c != '\n') {
    tok->done = E_LINECONT;
    return -1;
  }
  c = tok_nextc(tok);
  if (c == EOF) {
    tok->done = E_EOF;
    tok->cur = tok->inp;
    return -1;
  } else {
    tok_backup(tok, c);
  }
  return c;
}

static int type_comment_token_setup(struct tok_state *tok, struct token *token,
                                    int type, int col_offset,
                                    int end_col_offset, const char *start,
                                    const char *end) {
  token->level = tok->level;
  token->lineno = token->end_lineno = tok->lineno;
  token->col_offset = col_offset;
  token->end_col_offset = end_col_offset;
  token->start = start;
  token->end = end;
  return type;
}

static int token_setup(struct tok_state *tok, struct token *token, int type,
                       const char *start, const char *end) {
  assert((start == NULL && end == NULL) || (start != NULL && end != NULL));
  token->level = tok->level;
  if (ISSTRINGLIT(type)) {
    token->lineno = tok->first_lineno;
  } else {
    token->lineno = tok->lineno;
  }
  token->end_lineno = tok->lineno;
  token->col_offset = token->end_col_offset = -1;
  token->start = start;
  token->end = end;

  if (start != NULL && end != NULL) {
    token->col_offset = tok->starting_col_offset;
    token->end_col_offset = tok->col_offset;
  }
  return type;
}

static int tok_get_normal_mode(struct tok_state *tok,
                               tokenizer_mode *current_tok,
                               struct token *token) {
  int c;
  int blankline, nonascii;

  const char *p_start = NULL;
  const char *p_end = NULL;
nextline:
  tok->start = NULL;
  tok->starting_col_offset = -1;
  blankline = 0;

  /* Get indentation level */
  if (tok->atbol) {
    int col = 0;
    int altcol = 0;
    tok->atbol = 0;
    int cont_line_col = 0;
    for (;;) {
      c = tok_nextc(tok);
      if (c == ' ') {
        col++, altcol++;
      } else if (c == '\t') {
        col = (col / tok->tabsize + 1) * tok->tabsize;
        altcol = (altcol / ALTTABSIZE + 1) * ALTTABSIZE;
      } else if (c == '\014') { /* Control-L (formfeed) */
        col = altcol = 0;       /* For Emacs users */
      } else if (c == '\\') {
        // Indentation cannot be split over multiple physical lines
        // using backslashes. This means that if we found a backslash
        // preceded by whitespace, **the first one we find** determines
        // the level of indentation of whatever comes next.
        cont_line_col = cont_line_col ? cont_line_col : col;
        if ((c = tok_continuation_line(tok)) == -1) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      } else {
        break;
      }
    }
    tok_backup(tok, c);
    if (c == '#' || c == '\n' || c == '\r') {
      /* Lines with only whitespace and/or comments
         shouldn't affect the indentation and are
         not passed to the parser as NEWLINE tokens,
         except *totally* empty lines in interactive
         mode, which signal the end of a command group. */
      if (col == 0 && c == '\n' && tok->prompt != NULL) {
        blankline = 0; /* Let it through */
      } else if (tok->prompt != NULL && tok->lineno == 1) {
        /* In interactive mode, if the first line contains
           only spaces and/or a comment, let it through. */
        blankline = 0;
        col = altcol = 0;
      } else {
        blankline = 1; /* Ignore completely */
      }
      /* We can't jump back right here since we still
         may need to skip to the end of a comment */
    }
    if (!blankline && tok->level == 0) {
      col = cont_line_col ? cont_line_col : col;
      altcol = cont_line_col ? cont_line_col : altcol;
      if (col == tok->indstack[tok->indent]) {
        /* No change */
        if (altcol != tok->altindstack[tok->indent]) {
          return MAKE_TOKEN(indenterror(tok));
        }
      } else if (col > tok->indstack[tok->indent]) {
        /* Indent -- always one */
        if (tok->indent + 1 >= MAXINDENT) {
          tok->done = E_TOODEEP;
          tok->cur = tok->inp;
          return MAKE_TOKEN(ERRORTOKEN);
        }
        if (altcol <= tok->altindstack[tok->indent]) {
          return MAKE_TOKEN(indenterror(tok));
        }
        tok->pendin++;
        tok->indstack[++tok->indent] = col;
        tok->altindstack[tok->indent] = altcol;
      } else /* col < tok->indstack[tok->indent] */ {
        /* Dedent -- any number, must be consistent */
        while (tok->indent > 0 && col < tok->indstack[tok->indent]) {
          tok->pendin--;
          tok->indent--;
        }
        if (col != tok->indstack[tok->indent]) {
          tok->done = E_DEDENT;
          tok->cur = tok->inp;
          return MAKE_TOKEN(ERRORTOKEN);
        }
        if (altcol != tok->altindstack[tok->indent]) {
          return MAKE_TOKEN(indenterror(tok));
        }
      }
    }
  }

  tok->start = tok->cur;
  tok->starting_col_offset = tok->col_offset;

  /* Return pending indents/dedents */
  if (tok->pendin != 0) {
    if (tok->pendin < 0) {
      if (tok->tok_extra_tokens) {
        p_start = tok->cur;
        p_end = tok->cur;
      }
      tok->pendin++;
      return MAKE_TOKEN(DEDENT);
    } else {
      if (tok->tok_extra_tokens) {
        p_start = tok->buf;
        p_end = tok->cur;
      }
      tok->pendin--;
      return MAKE_TOKEN(INDENT);
    }
  }

  /* Peek ahead at the next character */
  c = tok_nextc(tok);
  tok_backup(tok, c);
  /* Check if we are closing an async function */
  if (tok->async_def &&
      !blankline
      /* Due to some implementation artifacts of type comments,
       * a TYPE_COMMENT at the start of a function won't set an
       * indentation level and it will produce a NEWLINE after it.
       * To avoid spuriously ending an async function due to this,
       * wait until we have some non-newline char in front of us. */
      && c != '\n' &&
      tok->level == 0
      /* There was a NEWLINE after ASYNC DEF,
         so we're past the signature. */
      && tok->async_def_nl
      /* Current indentation level is less than where
         the async function was defined */
      && tok->async_def_indent >= tok->indent) {
    tok->async_def = 0;
    tok->async_def_indent = 0;
    tok->async_def_nl = 0;
  }

again:
  tok->start = NULL;
  /* Skip spaces */
  do {
    c = tok_nextc(tok);
  } while (c == ' ' || c == '\t' || c == '\014');

  /* Set start of current token */
  tok->start = tok->cur == NULL ? NULL : tok->cur - 1;
  tok->starting_col_offset = tok->col_offset - 1;

  /* Skip comment, unless it's a type comment */
  if (c == '#') {

    const char *p = NULL;
    const char *prefix, *type_start;
    int current_starting_col_offset;

    while (c != EOF && c != '\n' && c != '\r') {
      c = tok_nextc(tok);
    }

    if (tok->tok_extra_tokens) {
      p = tok->start;
    }

    if (tok->type_comments) {
      p = tok->start;
      current_starting_col_offset = tok->starting_col_offset;
      prefix = type_comment_prefix;
      while (*prefix && p < tok->cur) {
        if (*prefix == ' ') {
          while (*p == ' ' || *p == '\t') {
            p++;
            current_starting_col_offset++;
          }
        } else if (*prefix == *p) {
          p++;
          current_starting_col_offset++;
        } else {
          break;
        }

        prefix++;
      }

      /* This is a type comment if we matched all of type_comment_prefix. */
      if (!*prefix) {
        int is_type_ignore = 1;
        // +6 in order to skip the word 'ignore'
        const char *ignore_end = p + 6;
        const int ignore_end_col_offset = current_starting_col_offset + 6;
        tok_backup(tok, c); /* don't eat the newline or EOF */

        type_start = p;

        /* A TYPE_IGNORE is "type: ignore" followed by the end of the token
         * or anything ASCII and non-alphanumeric. */
        is_type_ignore =
            (tok->cur >= ignore_end && memcmp(p, "ignore", 6) == 0 &&
             !(tok->cur > ignore_end && ((unsigned char)ignore_end[0] >= 128 ||
                                         Py_ISALNUM(ignore_end[0]))));

        if (is_type_ignore) {
          p_start = ignore_end;
          p_end = tok->cur;

          /* If this type ignore is the only thing on the line, consume the
           * newline also. */
          if (blankline) {
            tok_nextc(tok);
            tok->atbol = 1;
          }
          return MAKE_TYPE_COMMENT_TOKEN(TYPE_IGNORE, ignore_end_col_offset,
                                         tok->col_offset);
        } else {
          p_start = type_start;
          p_end = tok->cur;
          return MAKE_TYPE_COMMENT_TOKEN(
              TYPE_COMMENT, current_starting_col_offset, tok->col_offset);
        }
      }
    }
    if (tok->tok_extra_tokens) {
      tok_backup(tok, c); /* don't eat the newline or EOF */
      p_start = p;
      p_end = tok->cur;
      tok->comment_newline = blankline;
      return MAKE_TOKEN(COMMENT);
    }
  }

  if (tok->done == E_INTERACT_STOP) {
    return MAKE_TOKEN(ENDMARKER);
  }

  /* Check for EOF and errors now */
  if (c == EOF) {
    if (tok->level) {
      return MAKE_TOKEN(ERRORTOKEN);
    }
    return MAKE_TOKEN(tok->done == E_EOF ? ENDMARKER : ERRORTOKEN);
  }

  /* Identifier (most frequent token!) */
  nonascii = 0;
  if (is_potential_identifier_start(c)) {
    /* Process the various legal combinations of b"", r"", u"", and f"". */
    int saw_b = 0, saw_r = 0, saw_u = 0, saw_f = 0;
    while (1) {
      if (!(saw_b || saw_u || saw_f) && (c == 'b' || c == 'B'))
        saw_b = 1;
      /* Since this is a backwards compatibility support literal we don't
         want to support it in arbitrary order like byte literals. */
      else if (!(saw_b || saw_u || saw_r || saw_f) && (c == 'u' || c == 'U')) {
        saw_u = 1;
      }
      /* ur"" and ru"" are not supported */
      else if (!(saw_r || saw_u) && (c == 'r' || c == 'R')) {
        saw_r = 1;
      } else if (!(saw_f || saw_b || saw_u) && (c == 'f' || c == 'F')) {
        saw_f = 1;
      } else {
        break;
      }
      c = tok_nextc(tok);
      if (c == '"' || c == '\'') {
        if (saw_f) {
          goto f_string_quote;
        }
        goto letter_quote;
      }
    }
    while (is_potential_identifier_char(c)) {
      if (c >= 128) {
        nonascii = 1;
      }
      c = tok_nextc(tok);
    }
    tok_backup(tok, c);
    if (nonascii && !verify_identifier(tok)) {
      return MAKE_TOKEN(ERRORTOKEN);
    }

    p_start = tok->start;
    p_end = tok->cur;

    /* async/await parsing block. */
    if (tok->cur - tok->start == 5 && tok->start[0] == 'a') {
      /* May be an 'async' or 'await' token.  For Python 3.7 or
         later we recognize them unconditionally.  For Python
         3.5 or 3.6 we recognize 'async' in front of 'def', and
         either one inside of 'async def'.  (Technically we
         shouldn't recognize these at all for 3.4 or earlier,
         but there's no *valid* Python 3.4 code that would be
         rejected, and async functions will be rejected in a
         later phase.) */
      if (!tok->async_hacks || tok->async_def) {
        /* Always recognize the keywords. */
        if (memcmp(tok->start, "async", 5) == 0) {
          return MAKE_TOKEN(ASYNC);
        }
        if (memcmp(tok->start, "await", 5) == 0) {
          return MAKE_TOKEN(AWAIT);
        }
      } else if (memcmp(tok->start, "async", 5) == 0) {
        /* The current token is 'async'.
           Look ahead one token to see if that is 'def'. */

        struct tok_state ahead_tok;
        struct token ahead_token;
        _PyToken_Init(&ahead_token);
        int ahead_tok_kind;

        memcpy(&ahead_tok, tok, sizeof(ahead_tok));
        ahead_tok_kind =
            tok_get_normal_mode(&ahead_tok, current_tok, &ahead_token);

        if (ahead_tok_kind == NAME && ahead_tok.cur - ahead_tok.start == 3 &&
            memcmp(ahead_tok.start, "def", 3) == 0) {
          /* The next token is going to be 'def', so instead of
             returning a plain NAME token, return ASYNC. */
          tok->async_def_indent = tok->indent;
          tok->async_def = 1;
          _PyToken_Free(&ahead_token);
          return MAKE_TOKEN(ASYNC);
        }
        _PyToken_Free(&ahead_token);
      }
    }

    return MAKE_TOKEN(NAME);
  }

  if (c == '\r') {
    c = tok_nextc(tok);
  }

  /* Newline */
  if (c == '\n') {
    tok->atbol = 1;
    if (blankline || tok->level > 0) {
      if (tok->tok_extra_tokens) {
        if (tok->comment_newline) {
          tok->comment_newline = 0;
        }
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(NL);
      }
      goto nextline;
    }
    if (tok->comment_newline && tok->tok_extra_tokens) {
      tok->comment_newline = 0;
      p_start = tok->start;
      p_end = tok->cur;
      return MAKE_TOKEN(NL);
    }
    p_start = tok->start;
    p_end = tok->cur - 1; /* Leave '\n' out of the string */
    tok->cont_line = 0;
    if (tok->async_def) {
      /* We're somewhere inside an 'async def' function, and
         we've encountered a NEWLINE after its signature. */
      tok->async_def_nl = 1;
    }
    return MAKE_TOKEN(NEWLINE);
  }

  /* Period or number starting with period? */
  if (c == '.') {
    c = tok_nextc(tok);
    if (isdigit(c)) {
      goto fraction;
    } else if (c == '.') {
      c = tok_nextc(tok);
      if (c == '.') {
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(ELLIPSIS);
      } else {
        tok_backup(tok, c);
      }
      tok_backup(tok, '.');
    } else {
      tok_backup(tok, c);
    }
    p_start = tok->start;
    p_end = tok->cur;
    return MAKE_TOKEN(DOT);
  }

  /* Number */
  if (isdigit(c)) {
    if (c == '0') {
      /* Hex, octal or binary -- maybe. */
      c = tok_nextc(tok);
      if (c == 'x' || c == 'X') {
        /* Hex */
        c = tok_nextc(tok);
        do {
          if (c == '_') {
            c = tok_nextc(tok);
          }
          if (!isxdigit(c)) {
            tok_backup(tok, c);
            return MAKE_TOKEN(syntaxerror(tok, "invalid hexadecimal literal"));
          }
          do {
            c = tok_nextc(tok);
          } while (isxdigit(c));
        } while (c == '_');
        if (!verify_end_of_number(tok, c, "hexadecimal")) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      } else if (c == 'o' || c == 'O') {
        /* Octal */
        c = tok_nextc(tok);
        do {
          if (c == '_') {
            c = tok_nextc(tok);
          }
          if (c < '0' || c >= '8') {
            if (isdigit(c)) {
              return MAKE_TOKEN(
                  syntaxerror(tok, "invalid digit '%c' in octal literal", c));
            } else {
              tok_backup(tok, c);
              return MAKE_TOKEN(syntaxerror(tok, "invalid octal literal"));
            }
          }
          do {
            c = tok_nextc(tok);
          } while ('0' <= c && c < '8');
        } while (c == '_');
        if (isdigit(c)) {
          return MAKE_TOKEN(
              syntaxerror(tok, "invalid digit '%c' in octal literal", c));
        }
        if (!verify_end_of_number(tok, c, "octal")) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      } else if (c == 'b' || c == 'B') {
        /* Binary */
        c = tok_nextc(tok);
        do {
          if (c == '_') {
            c = tok_nextc(tok);
          }
          if (c != '0' && c != '1') {
            if (isdigit(c)) {
              return MAKE_TOKEN(
                  syntaxerror(tok, "invalid digit '%c' in binary literal", c));
            } else {
              tok_backup(tok, c);
              return MAKE_TOKEN(syntaxerror(tok, "invalid binary literal"));
            }
          }
          do {
            c = tok_nextc(tok);
          } while (c == '0' || c == '1');
        } while (c == '_');
        if (isdigit(c)) {
          return MAKE_TOKEN(
              syntaxerror(tok, "invalid digit '%c' in binary literal", c));
        }
        if (!verify_end_of_number(tok, c, "binary")) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      } else {
        int nonzero = 0;
        /* maybe old-style octal; c is first char of it */
        /* in any case, allow '0' as a literal */
        while (1) {
          if (c == '_') {
            c = tok_nextc(tok);
            if (!isdigit(c)) {
              tok_backup(tok, c);
              return MAKE_TOKEN(syntaxerror(tok, "invalid decimal literal"));
            }
          }
          if (c != '0') {
            break;
          }
          c = tok_nextc(tok);
        }
        char *zeros_end = tok->cur;
        if (isdigit(c)) {
          nonzero = 1;
          c = tok_decimal_tail(tok);
          if (c == 0) {
            return MAKE_TOKEN(ERRORTOKEN);
          }
        }
        if (c == '.') {
          c = tok_nextc(tok);
          goto fraction;
        } else if (c == 'e' || c == 'E') {
          goto exponent;
        } else if (c == 'j' || c == 'J') {
          goto imaginary;
        } else if (nonzero && !tok->tok_extra_tokens) {
          /* Old-style octal: now disallowed. */
          tok_backup(tok, c);
          return MAKE_TOKEN(syntaxerror_known_range(
              tok, (int)(tok->start + 1 - tok->line_start),
              (int)(zeros_end - tok->line_start),
              "leading zeros in decimal integer "
              "literals are not permitted; "
              "use an 0o prefix for octal integers"));
        }
        if (!verify_end_of_number(tok, c, "decimal")) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      }
    } else {
      /* Decimal */
      c = tok_decimal_tail(tok);
      if (c == 0) {
        return MAKE_TOKEN(ERRORTOKEN);
      }
      {
        /* Accept floating point numbers. */
        if (c == '.') {
          c = tok_nextc(tok);
        fraction:
          /* Fraction */
          if (isdigit(c)) {
            c = tok_decimal_tail(tok);
            if (c == 0) {
              return MAKE_TOKEN(ERRORTOKEN);
            }
          }
        }
        if (c == 'e' || c == 'E') {
          int e;
        exponent:
          e = c;
          /* Exponent part */
          c = tok_nextc(tok);
          if (c == '+' || c == '-') {
            c = tok_nextc(tok);
            if (!isdigit(c)) {
              tok_backup(tok, c);
              return MAKE_TOKEN(syntaxerror(tok, "invalid decimal literal"));
            }
          } else if (!isdigit(c)) {
            tok_backup(tok, c);
            if (!verify_end_of_number(tok, e, "decimal")) {
              return MAKE_TOKEN(ERRORTOKEN);
            }
            tok_backup(tok, e);
            p_start = tok->start;
            p_end = tok->cur;
            return MAKE_TOKEN(NUMBER);
          }
          c = tok_decimal_tail(tok);
          if (c == 0) {
            return MAKE_TOKEN(ERRORTOKEN);
          }
        }
        if (c == 'j' || c == 'J') {
          /* Imaginary part */
        imaginary:
          c = tok_nextc(tok);
          if (!verify_end_of_number(tok, c, "imaginary")) {
            return MAKE_TOKEN(ERRORTOKEN);
          }
        } else if (!verify_end_of_number(tok, c, "decimal")) {
          return MAKE_TOKEN(ERRORTOKEN);
        }
      }
    }
    tok_backup(tok, c);
    p_start = tok->start;
    p_end = tok->cur;
    return MAKE_TOKEN(NUMBER);
  }

f_string_quote:
  if (((tolower(*tok->start) == 'f' || tolower(*tok->start) == 'r') &&
       (c == '\'' || c == '"'))) {
    int quote = c;
    int quote_size = 1; /* 1 or 3 */

    /* Nodes of type STRING, especially multi line strings
       must be handled differently in order to get both
       the starting line number and the column offset right.
       (cf. issue 16806) */
    tok->first_lineno = tok->lineno;
    tok->multi_line_start = tok->line_start;

    /* Find the quote size and start of string */
    int after_quote = tok_nextc(tok);
    if (after_quote == quote) {
      int after_after_quote = tok_nextc(tok);
      if (after_after_quote == quote) {
        quote_size = 3;
      } else {
        // TODO: Check this
        tok_backup(tok, after_after_quote);
        tok_backup(tok, after_quote);
      }
    }
    if (after_quote != quote) {
      tok_backup(tok, after_quote);
    }

    p_start = tok->start;
    p_end = tok->cur;
    if (tok->tok_mode_stack_index + 1 >= MAXFSTRINGLEVEL) {
      return MAKE_TOKEN(syntaxerror(tok, "too many nested f-strings"));
    }
    tokenizer_mode *the_current_tok = TOK_NEXT_MODE(tok);
    the_current_tok->kind = TOK_FSTRING_MODE;
    the_current_tok->f_string_quote = quote;
    the_current_tok->f_string_quote_size = quote_size;
    the_current_tok->f_string_start = tok->start;
    the_current_tok->f_string_multi_line_start = tok->line_start;
    the_current_tok->f_string_line_start = tok->lineno;
    the_current_tok->f_string_start_offset = -1;
    the_current_tok->f_string_multi_line_start_offset = -1;
    the_current_tok->last_expr_buffer = NULL;
    the_current_tok->last_expr_size = 0;
    the_current_tok->last_expr_end = -1;
    the_current_tok->f_string_debug = 0;

    switch (*tok->start) {
    case 'F':
    case 'f':
      the_current_tok->f_string_raw = tolower(*(tok->start + 1)) == 'r';
      break;
    case 'R':
    case 'r':
      the_current_tok->f_string_raw = 1;
      break;
    default:
      Py_UNREACHABLE();
    }

    the_current_tok->curly_bracket_depth = 0;
    the_current_tok->curly_bracket_expr_start_depth = -1;
    return MAKE_TOKEN(FSTRING_START);
  }

letter_quote:
  /* String */
  if (c == '\'' || c == '"') {
    int quote = c;
    int quote_size = 1; /* 1 or 3 */
    int end_quote_size = 0;

    /* Nodes of type STRING, especially multi line strings
       must be handled differently in order to get both
       the starting line number and the column offset right.
       (cf. issue 16806) */
    tok->first_lineno = tok->lineno;
    tok->multi_line_start = tok->line_start;

    /* Find the quote size and start of string */
    c = tok_nextc(tok);
    if (c == quote) {
      c = tok_nextc(tok);
      if (c == quote) {
        quote_size = 3;
      } else {
        end_quote_size = 1; /* empty string found */
      }
    }
    if (c != quote) {
      tok_backup(tok, c);
    }

    /* Get rest of string */
    while (end_quote_size != quote_size) {
      c = tok_nextc(tok);
      if (tok->done == E_ERROR) {
        return MAKE_TOKEN(ERRORTOKEN);
      }
      if (tok->done == E_DECODE) {
        break;
      }
      if (c == EOF || (quote_size == 1 && c == '\n')) {
        assert(tok->multi_line_start != NULL);
        // shift the tok_state's location into
        // the start of string, and report the error
        // from the initial quote character
        tok->cur = (char *)tok->start;
        tok->cur++;
        tok->line_start = tok->multi_line_start;
        int start = tok->lineno;
        tok->lineno = tok->first_lineno;

        if (INSIDE_FSTRING(tok)) {
          /* When we are in an f-string, before raising the
           * unterminated string literal error, check whether
           * does the initial quote matches with f-strings quotes
           * and if it is, then this must be a missing '}' token
           * so raise the proper error */
          tokenizer_mode *the_current_tok = TOK_GET_MODE(tok);
          if (the_current_tok->f_string_quote == quote &&
              the_current_tok->f_string_quote_size == quote_size) {
            return MAKE_TOKEN(
                syntaxerror(tok, "f-string: expecting '}'", start));
          }
        }

        if (quote_size == 3) {
          syntaxerror(tok,
                      "unterminated triple-quoted string literal"
                      " (detected at line %d)",
                      start);
          if (c != '\n') {
            tok->done = E_EOFS;
          }
          return MAKE_TOKEN(ERRORTOKEN);
        } else {
          syntaxerror(tok,
                      "unterminated string literal (detected at"
                      " line %d)",
                      start);
          if (c != '\n') {
            tok->done = E_EOLS;
          }
          return MAKE_TOKEN(ERRORTOKEN);
        }
      }
      if (c == quote) {
        end_quote_size += 1;
      } else {
        end_quote_size = 0;
        if (c == '\\') {
          c = tok_nextc(tok); /* skip escaped char */
          if (c == '\r') {
            c = tok_nextc(tok);
          }
        }
      }
    }

    p_start = tok->start;
    p_end = tok->cur;
    return MAKE_TOKEN(STRING);
  }

  /* Line continuation */
  if (c == '\\') {
    if ((c = tok_continuation_line(tok)) == -1) {
      return MAKE_TOKEN(ERRORTOKEN);
    }
    tok->cont_line = 1;
    goto again; /* Read next line */
  }

  /* Punctuation character */
  int is_punctuation = (c == ':' || c == '}' || c == '!' || c == '{');
  if (is_punctuation && INSIDE_FSTRING(tok) &&
      INSIDE_FSTRING_EXPR(current_tok)) {
    /* This code block gets executed before the curly_bracket_depth is
     * incremented by the `{` case, so for ensuring that we are on the 0th
     * level, we need to adjust it manually */
    int cursor = current_tok->curly_bracket_depth - (c != '{');
    int in_format_spec = current_tok->in_format_spec;
    int cursor_in_format_with_debug =
        cursor == 1 && (current_tok->f_string_debug || in_format_spec);
    int cursor_valid = cursor == 0 || cursor_in_format_with_debug;
    if (cursor_valid && !update_fstring_expr(tok, c)) {
      return MAKE_TOKEN(ENDMARKER);
    }
    if (cursor_valid && c != '{' && set_fstring_expr(tok, token, c)) {
      return MAKE_TOKEN(ERRORTOKEN);
    }

    if (c == ':' && cursor == current_tok->curly_bracket_expr_start_depth) {
      current_tok->kind = TOK_FSTRING_MODE;
      current_tok->in_format_spec = 1;
      p_start = tok->start;
      p_end = tok->cur;
      return MAKE_TOKEN(_PyToken_OneChar(c));
    }
  }

  /* Check for two-character token */
  {
    int c2 = tok_nextc(tok);
    int current_token = _PyToken_TwoChars(c, c2);
    if (current_token != OP) {
      int c3 = tok_nextc(tok);
      int current_token3 = _PyToken_ThreeChars(c, c2, c3);
      if (current_token3 != OP) {
        current_token = current_token3;
      } else {
        tok_backup(tok, c3);
      }
      p_start = tok->start;
      p_end = tok->cur;
      return MAKE_TOKEN(current_token);
    }
    tok_backup(tok, c2);
  }

  /* Keep track of parentheses nesting level */
  switch (c) {
  case '(':
  case '[':
  case '{':
    if (tok->level >= MAXLEVEL) {
      return MAKE_TOKEN(syntaxerror(tok, "too many nested parentheses"));
    }
    tok->parenstack[tok->level] = c;
    tok->parenlinenostack[tok->level] = tok->lineno;
    tok->parencolstack[tok->level] = (int)(tok->start - tok->line_start);
    tok->level++;
    if (INSIDE_FSTRING(tok)) {
      current_tok->curly_bracket_depth++;
    }
    break;
  case ')':
  case ']':
  case '}':
    if (INSIDE_FSTRING(tok) && !current_tok->curly_bracket_depth && c == '}') {
      return MAKE_TOKEN(
          syntaxerror(tok, "f-string: single '}' is not allowed"));
    }
    if (!tok->tok_extra_tokens && !tok->level) {
      return MAKE_TOKEN(syntaxerror(tok, "unmatched '%c'", c));
    }
    if (tok->level > 0) {
      tok->level--;
      int opening = tok->parenstack[tok->level];
      if (!tok->tok_extra_tokens &&
          !((opening == '(' && c == ')') || (opening == '[' && c == ']') ||
            (opening == '{' && c == '}'))) {
        /* If the opening bracket belongs to an f-string's expression
        part (e.g. f"{)}") and the closing bracket is an arbitrary
        nested expression, then instead of matching a different
        syntactical construct with it; we'll throw an unmatched
        parentheses error. */
        if (INSIDE_FSTRING(tok) && opening == '{') {
          assert(current_tok->curly_bracket_depth >= 0);
          int previous_bracket = current_tok->curly_bracket_depth - 1;
          if (previous_bracket == current_tok->curly_bracket_expr_start_depth) {
            return MAKE_TOKEN(syntaxerror(tok, "f-string: unmatched '%c'", c));
          }
        }
        if (tok->parenlinenostack[tok->level] != tok->lineno) {
          return MAKE_TOKEN(
              syntaxerror(tok,
                          "closing parenthesis '%c' does not match "
                          "opening parenthesis '%c' on line %d",
                          c, opening, tok->parenlinenostack[tok->level]));
        } else {
          return MAKE_TOKEN(
              syntaxerror(tok,
                          "closing parenthesis '%c' does not match "
                          "opening parenthesis '%c'",
                          c, opening));
        }
      }
    }

    if (INSIDE_FSTRING(tok)) {
      current_tok->curly_bracket_depth--;
      if (current_tok->curly_bracket_depth < 0) {
        return MAKE_TOKEN(syntaxerror(tok, "f-string: unmatched '%c'", c));
      }
      if (c == '}' && current_tok->curly_bracket_depth ==
                          current_tok->curly_bracket_expr_start_depth) {
        current_tok->curly_bracket_expr_start_depth--;
        current_tok->kind = TOK_FSTRING_MODE;
        current_tok->in_format_spec = 0;
        current_tok->f_string_debug = 0;
      }
    }
    break;
  default:
    break;
  }

  if (!Py_UNICODE_ISPRINTABLE(c)) {
    return MAKE_TOKEN(
        syntaxerror(tok, "invalid non-printable character U+%04X", c));
  }

  if (c == '=' && INSIDE_FSTRING_EXPR(current_tok)) {
    current_tok->f_string_debug = 1;
  }

  /* Punctuation character */
  p_start = tok->start;
  p_end = tok->cur;
  return MAKE_TOKEN(_PyToken_OneChar(c));
}

static int tok_get_fstring_mode(struct tok_state *tok,
                                tokenizer_mode *current_tok,
                                struct token *token) {
  const char *p_start = NULL;
  const char *p_end = NULL;
  int end_quote_size = 0;
  int unicode_escape = 0;

  tok->start = tok->cur;
  tok->first_lineno = tok->lineno;
  tok->starting_col_offset = tok->col_offset;

  // If we start with a bracket, we defer to the normal mode as there is nothing
  // for us to tokenize before it.
  int start_char = tok_nextc(tok);
  if (start_char == '{') {
    int peek1 = tok_nextc(tok);
    tok_backup(tok, peek1);
    tok_backup(tok, start_char);
    if (peek1 != '{') {
      current_tok->curly_bracket_expr_start_depth++;
      if (current_tok->curly_bracket_expr_start_depth >= MAX_EXPR_NESTING) {
        return MAKE_TOKEN(
            syntaxerror(tok, "f-string: expressions nested too deeply"));
      }
      TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
      return tok_get_normal_mode(tok, current_tok, token);
    }
  } else {
    tok_backup(tok, start_char);
  }

  // Check if we are at the end of the string
  for (int i = 0; i < current_tok->f_string_quote_size; i++) {
    int quote = tok_nextc(tok);
    if (quote != current_tok->f_string_quote) {
      tok_backup(tok, quote);
      goto f_string_middle;
    }
  }

  if (current_tok->last_expr_buffer != NULL) {
    PyMem_Free(current_tok->last_expr_buffer);
    current_tok->last_expr_buffer = NULL;
    current_tok->last_expr_size = 0;
    current_tok->last_expr_end = -1;
  }

  p_start = tok->start;
  p_end = tok->cur;
  tok->tok_mode_stack_index--;
  return MAKE_TOKEN(FSTRING_END);

f_string_middle:

  // TODO: This is a bit of a hack, but it works for now. We need to find a
  // better way to handle this.
  tok->multi_line_start = tok->line_start;
  while (end_quote_size != current_tok->f_string_quote_size) {
    int c = tok_nextc(tok);
    if (tok->done == E_ERROR || tok->done == E_DECODE) {
      return MAKE_TOKEN(ERRORTOKEN);
    }
    int in_format_spec =
        (current_tok->in_format_spec && INSIDE_FSTRING_EXPR(current_tok));

    if (c == EOF || (current_tok->f_string_quote_size == 1 && c == '\n')) {
      if (tok->decoding_erred) {
        return MAKE_TOKEN(ERRORTOKEN);
      }

      // If we are in a format spec and we found a newline,
      // it means that the format spec ends here and we should
      // return to the regular mode.
      if (in_format_spec && c == '\n') {
        tok_backup(tok, c);
        TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
        current_tok->in_format_spec = 0;
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(FSTRING_MIDDLE);
      }

      assert(tok->multi_line_start != NULL);
      // shift the tok_state's location into
      // the start of string, and report the error
      // from the initial quote character
      tok->cur = (char *)current_tok->f_string_start;
      tok->cur++;
      tok->line_start = current_tok->f_string_multi_line_start;
      int start = tok->lineno;

      tokenizer_mode *the_current_tok = TOK_GET_MODE(tok);
      tok->lineno = the_current_tok->f_string_line_start;

      if (current_tok->f_string_quote_size == 3) {
        syntaxerror(tok,
                    "unterminated triple-quoted f-string literal"
                    " (detected at line %d)",
                    start);
        if (c != '\n') {
          tok->done = E_EOFS;
        }
        return MAKE_TOKEN(ERRORTOKEN);
      } else {
        return MAKE_TOKEN(
            syntaxerror(tok,
                        "unterminated f-string literal (detected at"
                        " line %d)",
                        start));
      }
    }

    if (c == current_tok->f_string_quote) {
      end_quote_size += 1;
      continue;
    } else {
      end_quote_size = 0;
    }

    if (c == '{') {
      if (!update_fstring_expr(tok, c)) {
        return MAKE_TOKEN(ENDMARKER);
      }
      int peek = tok_nextc(tok);
      if (peek != '{' || in_format_spec) {
        tok_backup(tok, peek);
        tok_backup(tok, c);
        current_tok->curly_bracket_expr_start_depth++;
        if (current_tok->curly_bracket_expr_start_depth >= MAX_EXPR_NESTING) {
          return MAKE_TOKEN(
              syntaxerror(tok, "f-string: expressions nested too deeply"));
        }
        TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
        current_tok->in_format_spec = 0;
        p_start = tok->start;
        p_end = tok->cur;
      } else {
        p_start = tok->start;
        p_end = tok->cur - 1;
      }
      return MAKE_TOKEN(FSTRING_MIDDLE);
    } else if (c == '}') {
      if (unicode_escape) {
        p_start = tok->start;
        p_end = tok->cur;
        return MAKE_TOKEN(FSTRING_MIDDLE);
      }
      int peek = tok_nextc(tok);

      // The tokenizer can only be in the format spec if we have already
      // completed the expression scanning (indicated by the end of the
      // expression being set) and we are not at the top level of the bracket
      // stack (-1 is the top level). Since format specifiers can't legally use
      // double brackets, we can bypass it here.
      int cursor = current_tok->curly_bracket_depth;
      if (peek == '}' && !in_format_spec && cursor == 0) {
        p_start = tok->start;
        p_end = tok->cur - 1;
      } else {
        tok_backup(tok, peek);
        tok_backup(tok, c);
        TOK_GET_MODE(tok)->kind = TOK_REGULAR_MODE;
        p_start = tok->start;
        p_end = tok->cur;
      }
      return MAKE_TOKEN(FSTRING_MIDDLE);
    } else if (c == '\\') {
      int peek = tok_nextc(tok);
      if (peek == '\r') {
        peek = tok_nextc(tok);
      }
      // Special case when the backslash is right before a curly
      // brace. We have to restore and return the control back
      // to the loop for the next iteration.
      if (peek == '{' || peek == '}') {
        if (!current_tok->f_string_raw) {
          if (warn_invalid_escape_sequence(tok, peek)) {
            return MAKE_TOKEN(ERRORTOKEN);
          }
        }
        tok_backup(tok, peek);
        continue;
      }

      if (!current_tok->f_string_raw) {
        if (peek == 'N') {
          /* Handle named unicode escapes (\N{BULLET}) */
          peek = tok_nextc(tok);
          if (peek == '{') {
            unicode_escape = 1;
          } else {
            tok_backup(tok, peek);
          }
        }
      } /* else {
          skip the escaped character
      }*/
    }
  }

  // Backup the f-string quotes to emit a final FSTRING_MIDDLE and
  // add the quotes to the FSTRING_END in the next tokenizer iteration.
  for (int i = 0; i < current_tok->f_string_quote_size; i++) {
    tok_backup(tok, current_tok->f_string_quote);
  }
  p_start = tok->start;
  p_end = tok->cur;
  return MAKE_TOKEN(FSTRING_MIDDLE);
}

static int tok_get(struct tok_state *tok, struct token *token) {
  tokenizer_mode *current_tok = TOK_GET_MODE(tok);
  if (current_tok->kind == TOK_REGULAR_MODE) {
    return tok_get_normal_mode(tok, current_tok, token);
  } else {
    return tok_get_fstring_mode(tok, current_tok, token);
  }
}

int _PyTokenizer_Get(struct tok_state *tok, struct token *token) {
  int result = tok_get(tok, token);
  if (tok->decoding_erred) {
    result = ERRORTOKEN;
    tok->done = E_DECODE;
  }
  return result;
}

#if defined(__wasi__) ||                                                       \
    (defined(__EMSCRIPTEN__) && (__EMSCRIPTEN_major__ >= 3))
// fdopen() with borrowed fd. WASI does not provide dup() and Emscripten's
// dup() emulation with open() is slow.
typedef union {
  void *cookie;
  int fd;
} borrowed;

static ssize_t borrow_read(void *cookie, char *buf, size_t size) {
  borrowed b = {.cookie = cookie};
  return read(b.fd, (void *)buf, size);
}

static FILE *fdopen_borrow(int fd) {
  // supports only reading. seek fails. close and write are no-ops.
  cookie_io_functions_t io_cb = {borrow_read, NULL, NULL, NULL};
  borrowed b = {.fd = fd};
  return fopencookie(b.cookie, "r", io_cb);
}
#else
static FILE *fdopen_borrow(int fd) {
  fd = _Py_dup(fd);
  if (fd < 0) {
    return NULL;
  }
  return fdopen(fd, "r");
}
#endif

/* Get the encoding of a Python file. Check for the coding cookie and check if
   the file starts with a BOM.

   _PyTokenizer_FindEncodingFilename() returns NULL when it can't find the
   encoding in the first or second line of the file (in which case the encoding
   should be assumed to be UTF-8).

   The char* returned is malloc'ed via PyMem_Malloc() and thus must be freed
   by the caller. */

char *_PyTokenizer_FindEncodingFilename(int fd, PyObject *filename) {
  struct tok_state *tok;
  FILE *fp;
  char *encoding = NULL;

  fp = fdopen_borrow(fd);
  if (fp == NULL) {
    return NULL;
  }
  tok = _PyTokenizer_FromFile(fp, NULL, NULL, NULL);
  if (tok == NULL) {
    fclose(fp);
    return NULL;
  }
  if (filename != NULL) {
    tok->filename = Py_NewRef(filename);
  } else {
    tok->filename = PyUnicode_FromString("<string>");
    if (tok->filename == NULL) {
      fclose(fp);
      _PyTokenizer_Free(tok);
      return encoding;
    }
  }
  struct token token;
  // We don't want to report warnings here because it could cause infinite
  // recursion if fetching the encoding shows a warning.
  tok->report_warnings = 0;
  while (tok->lineno < 2 && tok->done == E_OK) {
    _PyToken_Init(&token);
    _PyTokenizer_Get(tok, &token);
    _PyToken_Free(&token);
  }
  fclose(fp);
  if (tok->encoding) {
    encoding = (char *)PyMem_Malloc(strlen(tok->encoding) + 1);
    if (encoding) {
      strcpy(encoding, tok->encoding);
    }
  }
  _PyTokenizer_Free(tok);
  return encoding;
}

#ifdef Py_DEBUG
void tok_dump(int type, char *start, char *end) {
  fprintf(stderr, "%s", _PyParser_TokenNames[type]);
  if (type == NAME || type == NUMBER || type == STRING || type == OP)
    fprintf(stderr, "(%.*s)", (int)(end - start), start);
}
#endif // Py_DEBUG
