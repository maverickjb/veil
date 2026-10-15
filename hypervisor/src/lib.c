#include <stdarg.h>

#include "veil.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = a;
    const uint8_t *pb = b;
    while (n--) {
        if (*pa != *pb) {
            return (int)*pa - (int)*pb;
        }
        pa++;
        pb++;
    }
    return 0;
}

static void print_u64(uint64_t v, unsigned base, int width, int zero)
{
    char buf[32];
    const char *digits = "0123456789abcdef";
    int i = 0;

    if (v == 0) {
        buf[i++] = '0';
    }
    while (v) {
        buf[i++] = digits[v % base];
        v /= base;
    }
    while (i < width) {
        buf[i++] = zero ? '0' : ' ';
    }
    while (i--) {
        uart_putc(buf[i]);
    }
}

void hyp_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            uart_putc(*fmt);
            continue;
        }
        fmt++;
        int zero = 0;
        int width = 0;
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            uart_puts(s ? s : "(null)");
            break;
        }
        case 'c':
            uart_putc((char)va_arg(ap, int));
            break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) {
                uart_putc('-');
                v = -v;
            }
            print_u64((uint64_t)(unsigned)v, 10, width, zero);
            break;
        }
        case 'u':
            print_u64(va_arg(ap, unsigned), 10, width, zero);
            break;
        case 'x':
            print_u64(va_arg(ap, unsigned), 16, width, zero);
            break;
        case 'p':
            uart_puts("0x");
            print_u64(va_arg(ap, uint64_t), 16, 16, 1);
            break;
        case 'l':
            if (fmt[1] == 'x') {
                fmt++;
                print_u64(va_arg(ap, unsigned long), 16, width, zero);
            } else if (fmt[1] == 'l' && fmt[2] == 'x') {
                fmt += 2;
                print_u64(va_arg(ap, unsigned long long), 16, width, zero);
            }
            break;
        case '%':
            uart_putc('%');
            break;
        default:
            uart_putc('%');
            uart_putc(*fmt);
            break;
        }
    }
    va_end(ap);
}
