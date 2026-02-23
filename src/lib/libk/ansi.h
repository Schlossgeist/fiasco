#pragma once

#if 1

// GENERAL
#define ANSI_ESCAPE             "\033"
#define ANSI_CSI                "["
#define ANSI_DELIMITER          ";"

// STYLING MODE
#define ANSI_MODE_STYLE         "m"

#define ANSI_STYLE_RESET        "0"
#define ANSI_BOLD               "1"
#define ANSI_DIM                "2"
#define ANSI_ITALIC             "3"
#define ANSI_UNDERLINE          "4"
#define ANSI_BLINKING           "5"
#define ANSI_REVERSE            "7"
#define ANSI_HIDDEN             "8"
#define ANSI_STRIKETHROUGH      "9"

#define ANSI_FOREGROUND         "3"
#define ANSI_BACKGROUND         "4"
#define ANSI_FOREGROUND_BRIGHT  "9"
#define ANSI_BACKGROUND_BRIGHT  "10"

#define ANSI_COLOR_BLACK 	    "0"
#define ANSI_COLOR_RED 	        "1"
#define ANSI_COLOR_GREEN 	    "2"
#define ANSI_COLOR_YELLOW       "3"
#define ANSI_COLOR_BLUE 	    "4"
#define ANSI_COLOR_MAGENTA      "5"
#define ANSI_COLOR_CYAN 	    "6"
#define ANSI_COLOR_WHITE 	    "7"
#define ANSI_COLOR_DEFAULT      "9"

#define ANSI_BLACK 	            ANSI_FOREGROUND ANSI_COLOR_BLACK
#define ANSI_RED 	            ANSI_FOREGROUND ANSI_COLOR_RED
#define ANSI_GREEN 	            ANSI_FOREGROUND ANSI_COLOR_GREEN
#define ANSI_YELLOW 	        ANSI_FOREGROUND ANSI_COLOR_YELLOW
#define ANSI_BLUE 	            ANSI_FOREGROUND ANSI_COLOR_BLUE
#define ANSI_MAGENTA 	        ANSI_FOREGROUND ANSI_COLOR_MAGENTA
#define ANSI_CYAN 	            ANSI_FOREGROUND ANSI_COLOR_CYAN
#define ANSI_WHITE 	            ANSI_FOREGROUND ANSI_COLOR_WHITE
#define ANSI_DEFAULT            ANSI_FOREGROUND ANSI_COLOR_DEFAULT

#define ANSI_BRIGHT_BLACK 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_BLACK
#define ANSI_BRIGHT_RED 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_RED
#define ANSI_BRIGHT_GREEN 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_GREEN
#define ANSI_BRIGHT_YELLOW 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_YELLOW
#define ANSI_BRIGHT_BLUE 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_BLUE
#define ANSI_BRIGHT_MAGENTA 	ANSI_FOREGROUND_BRIGHT ANSI_COLOR_MAGENTA
#define ANSI_BRIGHT_CYAN 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_CYAN
#define ANSI_BRIGHT_WHITE 	    ANSI_FOREGROUND_BRIGHT ANSI_COLOR_WHITE
#define ANSI_BRIGHT_DEFAULT     ANSI_FOREGROUND_BRIGHT ANSI_COLOR_DEFAULT

#define __ANSI_STYLE(str, style) \
    ANSI_ESCAPE ANSI_CSI \
    style ANSI_MODE_STYLE \
    str ANSI_ESCAPE ANSI_CSI ANSI_MODE_STYLE

// EXPANDx trick allows for up to 9 arguments
// see also: https://stackoverflow.com/a/67878835
#define __ANSI_PARENS       ()
#define __ANSI_EXPAND0(arg) \
    __ANSI_EXPAND1(__ANSI_EXPAND1(__ANSI_EXPAND1(arg)))
#define __ANSI_EXPAND1(arg) \
    __ANSI_EXPAND2(__ANSI_EXPAND2(__ANSI_EXPAND2(arg)))
#define __ANSI_EXPAND2(arg) arg
#define __ANSI_FOR_EACH(m, ...) \
    __VA_OPT__(__ANSI_EXPAND0(__ANSI_FOR_EACH_HELPER(m, __VA_ARGS__)))
#define __ANSI_FOR_EACH_HELPER(m, a1, ...)  m(a1) \
    __VA_OPT__(__ANSI_FOR_EACH_AGAIN __ANSI_PARENS (m, __VA_ARGS__))
#define __ANSI_FOR_EACH_AGAIN() __ANSI_FOR_EACH_HELPER

#define __ANSI_PREPEND(x)  ANSI_DELIMITER ANSI_ ## x
#define __ANSI_EXPAND(...) __ANSI_FOR_EACH(__ANSI_PREPEND, __VA_ARGS__)
#define ANSI(str, ...)     __ANSI_STYLE(str, __ANSI_EXPAND(__VA_ARGS__))


inline int __ANSI_count_esc_seq_length(const char *str)
{
    int length = 0;
    if (*str == '\e')
    {
        for (char *str_ptr = const_cast<char *>(str);
            *str_ptr != '\0' && *str_ptr != 'm';
            str_ptr++)
            length++;
        length += sizeof(ANSI_ESCAPE ANSI_CSI ANSI_MODE_STYLE);
    }
    return length;
}

template<typename T>
int __ANSI_count_esc_seq_length(T)
{ return 0; }

inline int ansi_printf(const char *format)
{ return __builtin_printf("%s", format); }

template<typename T, typename... Targs>
int ansi_printf(const char* format, T value, Targs... Fargs)
{
    while (*format != '\0')
    {
        if (*format == '%')
        {
            // static because this function is tail-recursive,
            // and we don't need a new buffer for every invocation
            // ---
            // the size of this buffer caps the maximum length of a
            // single conversion specifier (stuff after a '%') to 16
            static char fmt_buffer[16] = {};
            char *fmt_buffer_ptr = fmt_buffer;
            format++;

            *fmt_buffer_ptr = '%';
            fmt_buffer_ptr++;

            int sign = 1;
            for (; *format == '-' ||
                   *format == '+' ||
                   *format == ' ' ||
                   *format == '#' ||
                   *format == '0'; format++)
            {
                if (*format == '-')
                    sign = -1;

                *fmt_buffer_ptr = *format;
                fmt_buffer_ptr++;
            }
            // PADDING
            int padding_width = 0;
            for (; '1' <= *format && *format <= '9'; format++)
                padding_width = padding_width * 10 + (*format - '0');
            // always create a fmt_buffer with padding
            if (*format == '*')
                format++;
            *fmt_buffer_ptr = '*';
            fmt_buffer_ptr++;
            // LONGNESS
            for (; *format == 'l' ||
                   *format == 'j' ||
                   *format == 'z' ||
                   *format == 't'; format++)
            {
                *fmt_buffer_ptr = *format;
                fmt_buffer_ptr++;
            }
            // CONVERSION FORMATS
            for (; *format == 'c' ||
                   *format == 's' ||
                   *format == 'd' || *format == 'i' ||
                   *format == 'o' ||
                   *format == 'x' || *format == 'X' ||
                   *format == 'u' ||
                   *format == 'f' || *format == 'F' ||
                   *format == 'e' || *format == 'E' ||
                   *format == 'a' || *format == 'A' ||
                   *format == 'g' || *format == 'G' ||
                   *format == 'n' ||
                   *format == 'p'; format++)
            {
                *fmt_buffer_ptr = *format;
                fmt_buffer_ptr++;
            }
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
            __builtin_printf(fmt_buffer,
                sign * (padding_width + __ANSI_count_esc_seq_length(value)),
                value);
#pragma GCC diagnostic pop
            return ansi_printf(format, Fargs...);
        }
        __builtin_printf("%c", *format);
        format++;
    }
    return 0;
}

#else

#define ANSI(str, ...)                  str

inline int ansi_printf(const char *format)
{ return __builtin_printf("%s", format); }

template<typename T, typename... Targs>
int ansi_printf(const char* format, Targs... args)
{ return __builtin_printf(format, args); }

#endif
