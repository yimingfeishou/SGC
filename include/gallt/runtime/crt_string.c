int32_t gallt_string_length(const gallt_string* s) {
    return (int32_t)gallt_string_safe_length(s);
}

int32_t gallt_string_compare(const gallt_string* a, const gallt_string* b) {
    int64_t la = gallt_string_safe_length(a);
    int64_t lb = gallt_string_safe_length(b);
    int64_t limit = la < lb ? la : lb;
    int rc = 0;

    if (limit > 0) {
        rc = memcmp(gallt_string_safe_data(a), gallt_string_safe_data(b),
            (size_t)limit);
    }

    if (rc != 0) { return rc < 0 ? -1 : 1; }
    if (la == lb) { return 0; }
    return la < lb ? -1 : 1;
}

void gallt_string_copy(gallt_string* out, const gallt_string* src) {
    if (out == NULL || src == NULL) { return; }
    *out = gallt_string_from_bytes_impl(gallt_string_safe_data(src),
        gallt_string_safe_length(src));
}

void gallt_string_move(gallt_string* out, gallt_string* src) {
    if (out == NULL || src == NULL || out == src) { return; }
    *out = *src;
    memset(src, 0, sizeof(*src));
}

void gallt_string_substr(gallt_string* out, const gallt_string* s,
    int32_t start, int32_t count) {
    if (out == NULL) { return; }
    int64_t len = gallt_string_safe_length(s);
    if (start < 0) { start = 0; }
    if ((int64_t)start > len) { start = (int32_t)len; }
    if (count < 0) { count = 0; }
    int64_t available = len - (int64_t)start;
    if ((int64_t)count > available) { count = (int32_t)available; }
    *out = gallt_string_from_bytes_impl(gallt_string_safe_data(s) + start, count);
}

int32_t gallt_string_find(const gallt_string* s, const gallt_string* needle) {
    int64_t len = gallt_string_safe_length(s);
    int64_t nlen = gallt_string_safe_length(needle);
    if (nlen <= 0) { return 0; }
    if (nlen > len) { return -1; }
    const char* base = gallt_string_safe_data(s);
    const char* pat = gallt_string_safe_data(needle);

    if (nlen == 1) {
        const char* hit = (const char*)memchr(base, pat[0], (size_t)len);
        return hit == NULL ? -1 : (int32_t)(hit - base);
    }

    int64_t last = len - nlen;
    int64_t i = 0;

    while (i <= last) {
        const char* hit = (const char*)memchr(base + i, pat[0], (size_t)(last - i) + 1);
        if (hit == NULL) { return -1; }
        int64_t index = (int64_t)(hit - base);
        if (memcmp(base + index, pat, (size_t)nlen) == 0) { return (int32_t)index; }
        i = index + 1;
    }

    return -1;
}

int8_t gallt_string_contains(const gallt_string* s, const gallt_string* needle) {
    return (int8_t)(gallt_string_find(s, needle) >= 0 ? 1 : 0);
}

void gallt_string_replace(gallt_string* out, const gallt_string* s,
    const gallt_string* from, const gallt_string* to) {
    if (out == NULL || s == NULL) { return; }
    int64_t len = gallt_string_safe_length(s);
    int64_t flen = gallt_string_safe_length(from);
    int64_t tlen = gallt_string_safe_length(to);

    if (flen <= 0) {
        *out = gallt_string_from_bytes_impl(gallt_string_safe_data(s), len);
        return;
    }

    const char* base = gallt_string_safe_data(s);
    const char* pat = gallt_string_safe_data(from);
    const char* rep = gallt_string_safe_data(to);
    int64_t total = 0;
    int64_t i = 0;

    while (i + flen <= len) {
        if (memcmp(base + i, pat, (size_t)flen) == 0) {
            total += tlen;
            i += flen;
        } else {
            total += 1;
            i += 1;
        }
    }

    total += len - i;
    char* buffer = (char*)malloc(total > 0 ? (size_t)total : 1);

    if (buffer == NULL) {
        *out = gallt_string_from_bytes_impl("", 0);
        return;
    }

    int64_t w = 0;
    i = 0;

    while (i + flen <= len) {
        if (memcmp(base + i, pat, (size_t)flen) == 0) {
            if (tlen > 0) { memcpy(buffer + w, rep, (size_t)tlen); }
            w += tlen;
            i += flen;
        } else {
            buffer[w++] = base[i++];
        }
    }

    while (i < len) {
        buffer[w++] = base[i++];
    }

    *out = gallt_string_from_bytes_impl(buffer, total);
    free(buffer);
}

void gallt_string_upper(gallt_string* out, const gallt_string* s) {
    if (out == NULL) { return; }
    *out = gallt_string_from_bytes_impl(gallt_string_safe_data(s),
        gallt_string_safe_length(s));
    char* data = gallt_string_mutable_data(out);

    for (int64_t i = 0; i < out->length; ++i) {
        char c = data[i];
        if (c >= 'a' && c <= 'z') { data[i] = (char)(c - ('a' - 'A')); }
    }
}

void gallt_string_lower(gallt_string* out, const gallt_string* s) {
    if (out == NULL) { return; }
    *out = gallt_string_from_bytes_impl(gallt_string_safe_data(s),
        gallt_string_safe_length(s));
    char* data = gallt_string_mutable_data(out);

    for (int64_t i = 0; i < out->length; ++i) {
        char c = data[i];
        if (c >= 'A' && c <= 'Z') { data[i] = (char)(c + ('a' - 'A')); }
    }
}

void gallt_string_trim(gallt_string* out, const gallt_string* s) {
    if (out == NULL) { return; }
    int64_t len = gallt_string_safe_length(s);
    const char* base = gallt_string_safe_data(s);
    int64_t start = 0;
    int64_t end = len;
    while (start < end && gallt_string_is_space(base[start])) { ++start; }
    while (end > start && gallt_string_is_space(base[end - 1])) { --end; }
    *out = gallt_string_from_bytes_impl(base + start, end - start);
}

int8_t gallt_string_char_at(const gallt_string* s, int32_t index) {
    if (s == NULL || index < 0 || (int64_t)index >= gallt_string_safe_length(s)) {
        return 0;
    }

    return (int8_t)gallt_string_data_ptr(s)[index];
}

int8_t gallt_string_set_char(gallt_string* s, int32_t index, int8_t ch) {
    if (s == NULL || index < 0 || (int64_t)index >= gallt_string_safe_length(s)) {
        return 0;
    }

    char* data = gallt_string_mutable_data(s);
    if (data == NULL) { return 0; }
    data[index] = (char)ch;
    return 1;
}

int32_t gallt_string_split_count(const gallt_string* s, const gallt_string* sep) {
    int64_t len = gallt_string_safe_length(s);
    int64_t slen = gallt_string_safe_length(sep);
    if (slen <= 0) { return 1; }
    const char* base = gallt_string_safe_data(s);
    const char* pat = gallt_string_safe_data(sep);
    int32_t count = 1;

    if (slen == 1) {
        for (int64_t k = 0; k < len; ++k) {
            if (base[k] == pat[0]) { ++count; }
        }

        return count;
    }

    const int64_t last = len - slen;
    int64_t i = 0;

    while (i <= last) {
        const char* hit = (const char*)memchr(base + i, pat[0],
            (size_t)(last - i) + 1);
        if (hit == NULL) { break; }
        const int64_t index = (int64_t)(hit - base);
        if (memcmp(base + index, pat, (size_t)slen) == 0) {
            ++count;
            i = index + slen;
        } else {
            i = index + 1;
        }
    }

    return count;
}

void gallt_string_split_at(gallt_string* out, const gallt_string* s,
    const gallt_string* sep, int32_t index) {
    if (out == NULL) { return; }
    int64_t len = gallt_string_safe_length(s);
    int64_t slen = gallt_string_safe_length(sep);
    const char* base = gallt_string_safe_data(s);

    if (index < 0) {
        *out = gallt_string_from_bytes_impl("", 0);
        return;
    }

    if (slen <= 0) {
        *out = index == 0
            ? gallt_string_from_bytes_impl(base, len)
            : gallt_string_from_bytes_impl("", 0);
        return;
    }

    const char* pat = gallt_string_safe_data(sep);
    int32_t current = 0;
    int64_t start = 0;
    int64_t i = 0;
    const int64_t last = len - slen;

    while (i <= last) {
        const char* hit = (const char*)memchr(base + i, pat[0],
            (size_t)(last - i) + 1);
        if (hit == NULL) { break; }
        const int64_t index = (int64_t)(hit - base);
        if (memcmp(base + index, pat, (size_t)slen) == 0) {
            if (current == index) {
                *out = gallt_string_from_bytes_impl(base + start,
                    index - start);
                return;
            }
            ++current;
            i = index + slen;
            start = i;
        } else {
            i = index + 1;
        }
    }

    if (current == index) {
        *out = gallt_string_from_bytes_impl(base + start, len - start);
        return;
    }

    *out = gallt_string_from_bytes_impl("", 0);
}

void gallt_string_read(gallt_string* out) {
    gallt_input_string(out);
}

void gallt_string_write(const gallt_string* s) {
    gallt_output_string(s);
}

int32_t gallt_string_write_file(void* handle, const gallt_string* s) {
    return gallt_file_write(handle, s);
}
