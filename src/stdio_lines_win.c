/* gow3 P6 (study-perf-deepdive-20261123.md; card runtime/stdio-one-write-per-line): one write per formatted line on
 * stdout / stderr (Windows).
 *
 * main() makes stdout unbuffered (crash lines must not wait in a buffer; stdout and stderr share the launcher's
 * pipe and stay in order). On an unbuffered stream both printf implementations in this exe (UCRT's, used by the C++
 * code and C files without _GNU_SOURCE, and mingw's __mingw_*, used by the runtime C files) write the formatted text
 * one character at a time: ~60-70 us a line into the pipe vs ~1.5 us for one write (measured 2026-11-24). The link
 * wraps both families (-Wl,--wrap=..., gpu/CMakeLists.txt, medievil-runtime only): a line for stdout / stderr is
 * formatted into a buffer by the same family's formatter and written with one fwrite, so the stream stays
 * unbuffered, the text is the same, and a line is no longer split between threads. Other streams go to the real
 * function. puts / fputs / fwrite already write once per call. Linux: glibc writes once per call (not built). */
#define __USE_MINGW_ANSI_STDIO 0
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int __real_printf(const char *format, ...);
int __real_fprintf(FILE *stream, const char *format, ...);
int __real_vprintf(const char *format, va_list args);
int __real_vfprintf(FILE *stream, const char *format, va_list args);
int __real___mingw_vfprintf(FILE *stream, const char *format, va_list args);
int __mingw_vsnprintf(char *buffer, size_t size, const char *format, va_list args);

/* Formats with `format_fn` (vsnprintf semantics) and writes the result with one fwrite. */
typedef int (*FormatFn)(char *, size_t, const char *, va_list);
static int write_line(FILE *stream, FormatFn format_fn, const char *format, va_list args) {
    char local[2048];
    va_list again;
    va_copy(again, args);
    const int n = format_fn(local, sizeof(local), format, args);
    if (n < 0) {
        va_end(again);
        return n;
    }
    if ((size_t)n < sizeof(local)) {
        va_end(again);
        return fwrite(local, 1, (size_t)n, stream) == (size_t)n ? n : -1;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap) {
        va_end(again);
        return -1;
    }
    const int m = format_fn(heap, (size_t)n + 1, format, again);
    va_end(again);
    const int written = m >= 0 && fwrite(heap, 1, (size_t)m, stream) == (size_t)m ? m : -1;
    free(heap);
    return written;
}

static int is_console_stream(FILE *stream) {
    return stream == stdout || stream == stderr;
}

static int ucrt_format(char *buffer, size_t size, const char *format, va_list args) {
    return vsnprintf(buffer, size, format, args);
}

/* UCRT family (C++ code, C files without _GNU_SOURCE). */
int __wrap_vfprintf(FILE *stream, const char *format, va_list args) {
    if (!is_console_stream(stream)) return __real_vfprintf(stream, format, args);
    return write_line(stream, ucrt_format, format, args);
}
int __wrap_vprintf(const char *format, va_list args) {
    return write_line(stdout, ucrt_format, format, args);
}
int __wrap_fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = __wrap_vfprintf(stream, format, args);
    va_end(args);
    return n;
}
int __wrap_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = write_line(stdout, ucrt_format, format, args);
    va_end(args);
    return n;
}

/* mingw family (runtime C files with _GNU_SOURCE: printf / fprintf / vfprintf are __mingw_* there). */
int __wrap___mingw_vfprintf(FILE *stream, const char *format, va_list args) {
    if (!is_console_stream(stream)) return __real___mingw_vfprintf(stream, format, args);
    return write_line(stream, __mingw_vsnprintf, format, args);
}
int __wrap___mingw_vprintf(const char *format, va_list args) {
    return write_line(stdout, __mingw_vsnprintf, format, args);
}
int __wrap___mingw_fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = __wrap___mingw_vfprintf(stream, format, args);
    va_end(args);
    return n;
}
int __wrap___mingw_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int n = write_line(stdout, __mingw_vsnprintf, format, args);
    va_end(args);
    return n;
}
