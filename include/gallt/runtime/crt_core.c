#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#define GALLT_PLATFORM_MKDIR(path) _mkdir(path)
#define GALLT_PLATFORM_RMDIR(path) _rmdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define GALLT_PLATFORM_MKDIR(path) mkdir((path), 0755)
#define GALLT_PLATFORM_RMDIR(path) rmdir(path)
#endif

static void gallt_platform_file_mode(const char* mode, char* out, size_t size) {
    size_t n = 0;
    if (mode == NULL || out == NULL || size == 0) {
        if (out != NULL && size > 0) out[0] = '\0';
        return;
    }
#if defined(_WIN32)
    for (size_t i = 0; mode[i] != '\0' && n + 1 < size; ++i) {
        out[n++] = mode[i];
    }
#else
    for (size_t i = 0; mode[i] != '\0' && n + 1 < size; ++i) {
        if (mode[i] == 'b') continue;
        out[n++] = mode[i];
    }
#endif
    out[n] = '\0';
}

typedef struct gallt_string {
    int64_t length;
    int64_t capacity;
    union {
        char bytes[16];
        char* heap;
    } data;
} gallt_string;

static const char* gallt_string_data_ptr(const gallt_string* s) {
    return s->length <= 15 ? s->data.bytes : s->data.heap;
}

static void gallt_string_free_contents(gallt_string* s) {
    if (s->length > 15 && s->data.heap != NULL) {
        free(s->data.heap);
    }
    memset(s, 0, sizeof(*s));
}

static gallt_string gallt_string_from_bytes_impl(const char* data, int64_t len) {
    gallt_string result;
    memset(&result, 0, sizeof(result));
    result.length = len;
    if (len <= 15) {
        result.capacity = 16;
        if (len > 0) memcpy(result.data.bytes, data, (size_t)len);
        if (len < 16) result.data.bytes[len] = '\0';
        return result;
    }
    char* buffer = (char*)malloc((size_t)len + 1);
    if (buffer == NULL) {
        result.length = 0;
        result.capacity = 0;
        return result;
    }
    memcpy(buffer, data, (size_t)len);
    buffer[len] = '\0';
    result.capacity = len + 1;
    result.data.heap = buffer;
    return result;
}

void gallt_string_init(gallt_string* out, const char* data, int64_t len) {
    if (data == NULL || len < 0) len = 0;
    if (out != NULL) {
        *out = gallt_string_from_bytes_impl(data == NULL ? "" : data, len);
    }
}

void gallt_string_assign(gallt_string* dest, const gallt_string* src) {
    if (dest == NULL || src == NULL) return;
    gallt_string replacement = gallt_string_from_bytes_impl(
        gallt_string_data_ptr(src), src->length);
    gallt_string_free_contents(dest);
    *dest = replacement;
}

void gallt_string_destroy(gallt_string* s) {
    if (s != NULL) gallt_string_free_contents(s);
}

const char* gallt_string_cstr(const gallt_string* s) {
    if (s == NULL) return "";
    return gallt_string_data_ptr(s);
}

void gallt_string_concat(gallt_string* out, const gallt_string* a, const gallt_string* b) {
    if (out == NULL || a == NULL || b == NULL) return;
    int64_t la = a->length > 0 ? a->length : 0;
    int64_t lb = b->length > 0 ? b->length : 0;
    const char* pa = a->length > 0 ? gallt_string_data_ptr(a) : NULL;
    const char* pb = b->length > 0 ? gallt_string_data_ptr(b) : NULL;
    int64_t total = la + lb;
    gallt_string replacement;
    memset(&replacement, 0, sizeof(replacement));
    replacement.length = total;
    if (total <= 15) {
        replacement.capacity = 16;
        if (la > 0) memcpy(replacement.data.bytes, pa, (size_t)la);
        if (lb > 0) memcpy(replacement.data.bytes + la, pb, (size_t)lb);
        replacement.data.bytes[total] = '\0';
        *out = replacement;
        return;
    }
    char* buffer = (char*)malloc((size_t)total + 1);
    if (buffer == NULL) {
        replacement.length = 0;
        replacement.capacity = 0;
        *out = replacement;
        return;
    }
    if (la > 0) memcpy(buffer, pa, (size_t)la);
    if (lb > 0) memcpy(buffer + la, pb, (size_t)lb);
    buffer[total] = '\0';
    replacement.capacity = total + 1;
    replacement.data.heap = buffer;
    *out = replacement;
}

static int gallt_format_fp(double v, char* buffer, size_t size) {
    int n = snprintf(buffer, size, "%g", v);
    if (n < 0) return 0;
    if ((size_t)n + 2 >= size) return n;
    if (strpbrk(buffer, ".eEnNiI") == NULL) {
        buffer[n] = '.';
        buffer[n + 1] = '0';
        buffer[n + 2] = '\0';
        return n + 2;
    }
    return n;
}

void gallt_output_string(const gallt_string* s) {
    if (s == NULL) return;
    fwrite(gallt_string_data_ptr(s), 1, (size_t)s->length, stdout);
    fflush(stdout);
}

void gallt_output_i32(int32_t v) { printf("%d", (int)v); fflush(stdout); }
void gallt_output_u32(uint32_t v) { printf("%u", (unsigned)v); fflush(stdout); }
void gallt_output_i64(int64_t v) { printf("%lld", (long long)v); fflush(stdout); }
void gallt_output_u64(uint64_t v) { printf("%llu", (unsigned long long)v); fflush(stdout); }
void gallt_output_f32(float v) {
    char buffer[80];
    int n = gallt_format_fp((double)v, buffer, sizeof(buffer));
    fwrite(buffer, 1, (size_t)n, stdout);
    fflush(stdout);
}
void gallt_output_f64(double v) {
    char buffer[80];
    int n = gallt_format_fp(v, buffer, sizeof(buffer));
    fwrite(buffer, 1, (size_t)n, stdout);
    fflush(stdout);
}
void gallt_output_char(char v) { putchar((unsigned char)v); fflush(stdout); }
void gallt_output_bool(unsigned char v) { fputs(v ? "true" : "false", stdout); fflush(stdout); }
void gallt_output_ptr(const void* v) { printf("%p", v); fflush(stdout); }

void gallt_input_i32(int32_t* p) { if (scanf("%d", p) != 1 && p) *p = 0; }
void gallt_input_u32(uint32_t* p) { if (scanf("%u", p) != 1 && p) *p = 0; }
void gallt_input_i64(int64_t* p) { if (scanf("%lld", (long long*)p) != 1 && p) *p = 0; }
void gallt_input_u64(uint64_t* p) { if (scanf("%llu", (unsigned long long*)p) != 1 && p) *p = 0; }
void gallt_input_uchar(unsigned char* p) { if (scanf(" %c", (char*)p) != 1 && p) *p = 0; }
void gallt_input_f32(float* p) { if (scanf("%f", p) != 1 && p) *p = 0; }
void gallt_input_f64(double* p) { if (scanf("%lf", p) != 1 && p) *p = 0; }
void gallt_input_char(char* p) { if (scanf(" %c", p) != 1 && p) *p = 0; }
void gallt_input_bool(unsigned char* p) {
    int v = 0;
    if (scanf("%d", &v) != 1) v = 0;
    if (p) *p = v ? 1 : 0;
}

void gallt_input_string(gallt_string* p) {
    if (p == NULL) return;
    char* buffer = NULL;
    size_t capacity = 0;
    size_t len = 0;
    int c = EOF;
    for (;;) {
        c = fgetc(stdin);
        if (c == EOF || c == '\n') break;
        if (len + 1 > capacity) {
            size_t next = (capacity == 0) ? 128 : capacity * 2;
            char* grown = (char*)realloc(buffer, next);
            if (grown == NULL) break;
            buffer = grown;
            capacity = next;
        }
        buffer[len++] = (char)c;
    }
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        --len;
    }
    gallt_string replacement = gallt_string_from_bytes_impl(
        buffer == NULL ? "" : buffer, (int64_t)len);
    free(buffer);
    gallt_string_free_contents(p);
    *p = replacement;
}

static gallt_string gallt_string_from_int64(int64_t v) {
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "%lld", (long long)v);
    return gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}

void gallt_string_from_i32(gallt_string* out, int32_t v) {
    if (out != NULL) *out = gallt_string_from_int64(v);
}
void gallt_string_from_u32(gallt_string* out, uint32_t v) {
    if (out == NULL) return;
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "%u", (unsigned)v);
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}
void gallt_string_from_i64(gallt_string* out, int64_t v) {
    if (out != NULL) *out = gallt_string_from_int64(v);
}
void gallt_string_from_u64(gallt_string* out, uint64_t v) {
    if (out == NULL) return;
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)v);
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}
void gallt_string_from_char(gallt_string* out, char v) {
    if (out != NULL) *out = gallt_string_from_bytes_impl(&v, 1);
}
void gallt_string_from_bool(gallt_string* out, unsigned char v) {
    if (out == NULL) return;
    *out = v ? gallt_string_from_bytes_impl("true", 4)
             : gallt_string_from_bytes_impl("false", 5);
}
void gallt_string_from_f32(gallt_string* out, float v) {
    if (out == NULL) return;
    char buffer[64];
    int n = gallt_format_fp((double)v, buffer, sizeof(buffer));
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}
void gallt_string_from_f64(gallt_string* out, double v) {
    if (out == NULL) return;
    char buffer[64];
    int n = gallt_format_fp(v, buffer, sizeof(buffer));
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}
void* gallt_alloc_bytes(int64_t size) {
    if (size <= 0) size = 1;
    return calloc(1, (size_t)size);
}
void gallt_free_ptr(void* ptr) {
    free(ptr);
}
void gallt_check_fptr(void* fn) {
    if (fn == NULL) {
        fprintf(stderr, "RTER 0002: null function pointer call\n");
        exit(1);
    }
}
