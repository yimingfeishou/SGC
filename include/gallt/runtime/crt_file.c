typedef struct gallt_file {
    FILE* fp;

    int error;
    int eof;
    int readable;
    struct gallt_file* next;
    void* slot;
} gallt_file;

typedef void* gallt_file_slot;

static gallt_file* gallt_file_registry = NULL;
static int gallt_file_exit_registered = 0;
static int gallt_file_last_error = 0;

static void gallt_file_release_all(void) {
    gallt_file* h = gallt_file_registry;

    while (h != NULL) {
        gallt_file* next = h->next;
        if (h->fp != NULL) {
            fclose(h->fp);
            h->fp = NULL;
        }

        free(h->slot);
        free(h);
        h = next;
    }

    gallt_file_registry = NULL;
}

static void gallt_file_register(gallt_file* handle) {
    handle->next = gallt_file_registry;
    gallt_file_registry = handle;

    if (!gallt_file_exit_registered) {
        atexit(gallt_file_release_all);
        gallt_file_exit_registered = 1;
    }
}

static gallt_file* gallt_file_deref(void* slot) {
    if (slot == NULL) { return NULL; }
    return *(gallt_file**)slot;
}

static int gallt_errno_code(void) {
    switch (errno) {
    case ENOENT: return 3;
    case EACCES:
    case EPERM:  return 2;
    case EEXIST: return 4;
    case ENOSPC: return 8;
    default:     return 9;
    }
}

static void gallt_file_set_error(gallt_file* handle, int code) {
    if (handle != NULL) { handle->error = code; }
}

static char* gallt_cstr_from_string(const gallt_string* s) {
    int64_t len = (s == NULL) ? 0 : s->length;
    if (len < 0) { len = 0; }
    char* buffer = (char*)malloc((size_t)len + 1);
    if (buffer == NULL) { return NULL; }
    if (len > 0) { memcpy(buffer, gallt_string_data_ptr(s), (size_t)len); }
    buffer[len] = '\0';
    return buffer;
}

static gallt_file* gallt_file_valid(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    if (h == NULL || h->fp == NULL) {
        gallt_file_set_error(h, 1);
        return NULL;
    }
    return h;
}

static int gallt_valid_file_mode(const char* mode) {
    static const char* valid_modes[] = {
        "r", "w", "a", "r+", "w+", "a+",
        "rb", "wb", "ab", "r+b", "w+b", "a+b",
    };
    if (mode == NULL) { return 0; }

    for (size_t i = 0; i < sizeof(valid_modes) / sizeof(valid_modes[0]); ++i) {
        if (strcmp(mode, valid_modes[i]) == 0) { return 1; }
    }

    return 0;
}

void* gallt_file_open(const gallt_string* path, const gallt_string* mode) {
    if (path == NULL || mode == NULL) {
        gallt_file_last_error = 9;
        return NULL;
    }

    char* path_cstr = gallt_cstr_from_string(path);
    char* mode_cstr = gallt_cstr_from_string(mode);
    if (path_cstr == NULL || mode_cstr == NULL) {
        free(path_cstr);
        free(mode_cstr);
        gallt_file_last_error = 9;
        return NULL;
    }

    if (!gallt_valid_file_mode(mode_cstr)) {
        free(path_cstr);
        free(mode_cstr);
        gallt_file_last_error = 9;
        return NULL;
    }

    const char* mode_text = gallt_string_data_ptr(mode);
    const int mode_readable = (strchr(mode_text, 'r') != NULL ||
        strchr(mode_text, '+') != NULL) ? 1 : 0;
    char native_mode[8];
    gallt_platform_file_mode(mode_cstr, native_mode, sizeof(native_mode));
    FILE* fp = fopen(path_cstr, native_mode);
    free(path_cstr);
    free(mode_cstr);

    if (fp == NULL) {
        gallt_file_last_error = gallt_errno_code();
        return NULL;
    }

    gallt_file* handle = (gallt_file*)calloc(1, sizeof(gallt_file));

    if (handle == NULL) {
        fclose(fp);
        gallt_file_last_error = 9;
        return NULL;
    }

    gallt_file_slot* slot = (gallt_file_slot*)calloc(1, sizeof(gallt_file_slot));

    if (slot == NULL) {
        fclose(fp);
        free(handle);
        gallt_file_last_error = 9;
        return NULL;
    }

    *slot = handle;
    handle->fp = fp;
    handle->error = 0;
    handle->eof = 0;
    handle->readable = mode_readable;
    handle->slot = slot;
    gallt_file_register(handle);
    gallt_file_last_error = 0;
    return slot;
}

int8_t gallt_file_close(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    if (h == NULL || h->fp == NULL) { return 0; }
    FILE* fp = h->fp;
    h->fp = NULL;
    int rc = fclose(fp);
    h->error = (rc == 0) ? 0 : gallt_errno_code();
    *(gallt_file**)slot = NULL;
    int ok = (rc == 0) ? 1 : 0;
    return (int8_t)ok;
}

int8_t gallt_file_flush(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return 0; }

    if (fflush(h->fp) != 0) {
        gallt_file_set_error(h, 6);
        return 0;
    }

    h->error = 0;
    return 1;
}

int32_t gallt_file_read(void* handle, void* buffer, int32_t count) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }

    if (buffer == NULL || count < 0) {
        gallt_file_set_error(h, 5);
        return -1;
    }

    size_t want = (size_t)count;
    size_t got = fread(buffer, 1, want, h->fp);
    h->eof = feof(h->fp) ? 1 : 0;

    if (got < want && ferror(h->fp)) {
        gallt_file_set_error(h, 5);
        return -1;
    }

    h->error = 0;
    return (int32_t)got;
}

int32_t gallt_file_write(void* handle, const gallt_string* s) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }

    if (s == NULL) {
        gallt_file_set_error(h, 6);
        return -1;
    }

    int64_t len = s->length;
    if (len <= 0) {
        h->error = 0;
        return 0;
    }

    size_t written = fwrite(gallt_string_data_ptr(s), 1, (size_t)len, h->fp);

    if (written != (size_t)len) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }

    h->error = 0;
    return (int32_t)written;
}

int32_t gallt_file_write_bytes(void* handle, const void* buffer, int32_t count) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }

    if (buffer == NULL || count < 0) {
        gallt_file_set_error(h, 6);
        return -1;
    }

    if (count == 0) {
        h->error = 0;
        return 0;
    }

    size_t written = fwrite(buffer, 1, (size_t)count, h->fp);

    if (written != (size_t)count) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }

    h->error = 0;
    return (int32_t)written;
}

int32_t gallt_file_getc(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }
    int c = fgetc(h->fp);

    if (c == EOF) {
        h->eof = 1;
        h->error = ferror(h->fp) ? 5 : 0;
        return -1;
    }

    h->error = 0;
    return (int32_t)(c & 0xFF);
}

int32_t gallt_file_putc(void* handle, int32_t ch) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }
    int c = fputc(ch & 0xFF, h->fp);

    if (c == EOF) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }

    h->error = 0;
    return (int32_t)(c & 0xFF);
}

void gallt_file_readline(gallt_string* out, void* handle) {
    if (out == NULL) { return; }
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) {
        *out = gallt_string_from_bytes_impl("", 0);
        return;
    }

    char* buffer = NULL;
    size_t capacity = 0;
    size_t length = 0;
    int c = EOF;

    while ((c = fgetc(h->fp)) != EOF) {
        if (c == '\n') { break; }

        if (length + 1 > capacity) {
            size_t next = (capacity == 0) ? 128 : capacity * 2;
            char* grown = (char*)realloc(buffer, next);
            if (grown == NULL) { break; }
            buffer = grown;
            capacity = next;
        }

        buffer[length++] = (char)c;
    }
    if (c == EOF) { h->eof = 1; }
    if (length > 0 && buffer != NULL && buffer[length - 1] == '\r') { length--; }

    if (buffer == NULL) {
        *out = gallt_string_from_bytes_impl("", 0);
    } else {
        *out = gallt_string_from_bytes_impl(buffer, (int64_t)length);
        free(buffer);
    }
    h->error = 0;
}

int32_t gallt_file_writeline(void* handle, const gallt_string* s) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }

    int64_t len = (s == NULL) ? 0 : s->length;
    size_t written = 0;

    if (len > 0) {
        written = fwrite(gallt_string_data_ptr(s), 1, (size_t)len, h->fp);

        if (written != (size_t)len) {
            gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
            return -1;
        }
    }

    if (fputc('\n', h->fp) == EOF) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }

    h->error = 0;
    return (int32_t)(written + 1);
}

int8_t gallt_file_seek(void* handle, int32_t offset, int32_t origin) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return 0; }

    int whence = SEEK_SET;

    switch (origin) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:
        gallt_file_set_error(h, 7);
        return 0;
    }

    if (fseek(h->fp, (long)offset, whence) != 0) {
        gallt_file_set_error(h, 7);
        return 0;
    }

    h->eof = 0;
    h->error = 0;
    return 1;
}

int32_t gallt_file_tell(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return -1; }
    long pos = ftell(h->fp);

    if (pos < 0) {
        gallt_file_set_error(h, 7);
        return -1;
    }

    h->error = 0;
    return (int32_t)pos;
}

int8_t gallt_file_eof(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) { return 0; }
    if (h->eof || feof(h->fp)) { return 1; }

    if (h->readable) {
        int c = fgetc(h->fp);

        if (c == EOF) {
            h->eof = 1;
            return 1;
        }

        ungetc(c, h->fp);
        return 0;
    }

    long position = ftell(h->fp);
    if (position < 0) { return 0; }

    if (fseek(h->fp, 0, SEEK_END) != 0) {
        fseek(h->fp, position, SEEK_SET);
        return 0;
    }

    long end = ftell(h->fp);
    fseek(h->fp, position, SEEK_SET);
    if (end < 0) { return 0; }
    return position >= end ? 1 : 0;
}

int32_t gallt_file_error(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    if (h == NULL || h->fp == NULL) {
        if (slot == NULL) { return gallt_file_last_error; }
        return 1;
    }
    return h->error;
}

int8_t gallt_file_remove(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) { return 0; }
    int rc = remove(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_rename(const gallt_string* from, const gallt_string* to) {
    char* a = gallt_cstr_from_string(from);
    char* b = gallt_cstr_from_string(to);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }

    int rc = rename(a, b);
    free(a);
    free(b);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_exists(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) { return 0; }
    FILE* fp = fopen(p, "rb");

    if (fp != NULL) {
        fclose(fp);
        free(p);
        return 1;
    }

    free(p);
    return 0;
}

int32_t gallt_file_size(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) { return -1; }
    FILE* fp = fopen(p, "rb");

    if (fp == NULL) {
        free(p);
        return -1;
    }

    free(p);

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    long size = ftell(fp);
    fclose(fp);
    if (size < 0) { return -1; }
    return (int32_t)size;
}

int8_t gallt_file_copy(const gallt_string* from, const gallt_string* to) {
    char* a = gallt_cstr_from_string(from);
    char* b = gallt_cstr_from_string(to);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }

    FILE* in = fopen(a, "rb");

    if (in == NULL) {
        free(a);
        free(b);
        return 0;
    }

    FILE* out = fopen(b, "wb");

    if (out == NULL) {
        fclose(in);
        free(a);
        free(b);
        return 0;
    }

    char chunk[8192];
    size_t n = 0;
    int ok = 1;

    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }

    if (ferror(in)) { ok = 0; }
    fclose(in);
    if (fclose(out) != 0) { ok = 0; }
    free(a);
    free(b);
    return (int8_t)(ok ? 1 : 0);
}

int8_t gallt_file_mkdir(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) { return 0; }
    int rc = GALLT_PLATFORM_MKDIR(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_removedir(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) { return 0; }
    int rc = GALLT_PLATFORM_RMDIR(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}

static int64_t gallt_string_safe_length(const gallt_string* s) {
    if (s == NULL || s->length < 0) { return 0; }
    return s->length;
}

static const char* gallt_string_safe_data(const gallt_string* s) {
    if (s == NULL) { return ""; }
    return gallt_string_data_ptr(s);
}

static char* gallt_string_mutable_data(gallt_string* s) {
    if (s == NULL) { return NULL; }
    return s->length <= 15 ? s->data.bytes : s->data.heap;
}

static int gallt_string_is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
        c == '\v' || c == '\f';
}
