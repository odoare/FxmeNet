/*
  ------------------------------------------------------------------------------
    Terminal.cpp  (fxmenet-top)

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#include "Terminal.h"

#include <cstdio>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <conio.h>
 #include <windows.h>
#else
 #include <sys/ioctl.h>
 #include <termios.h>
 #include <unistd.h>
#endif

namespace fxmenet::top
{

namespace
{
#if defined (_WIN32)
    DWORD savedMode = 0;
    UINT savedCodePage = 0;
#else
    termios saved {};

    /** One byte if one is waiting. */
    bool next (unsigned char& c)
    {
        return read (STDIN_FILENO, &c, 1) == 1;
    }
#endif

    void write (const std::string& s)
    {
        std::fwrite (s.data(), 1, s.size(), stdout);
        std::fflush (stdout);
    }
}

std::string utf8 (int c)
{
    std::string s;
    if (c < 0x80)
        s += static_cast<char> (c);
    else if (c < 0x800)
    {
        s += static_cast<char> (0xC0 | (c >> 6));
        s += static_cast<char> (0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        s += static_cast<char> (0xE0 | (c >> 12));
        s += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char> (0x80 | (c & 0x3F));
    }
    else
    {
        s += static_cast<char> (0xF0 | (c >> 18));
        s += static_cast<char> (0x80 | ((c >> 12) & 0x3F));
        s += static_cast<char> (0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char> (0x80 | (c & 0x3F));
    }
    return s;
}

Terminal::Terminal()
{
#if defined (_WIN32)
    auto out = GetStdHandle (STD_OUTPUT_HANDLE);
    GetConsoleMode (out, &savedMode);
    SetConsoleMode (out, savedMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    savedCodePage = GetConsoleOutputCP();
    SetConsoleOutputCP (CP_UTF8);
#else
    tcgetattr (STDIN_FILENO, &saved);
    termios raw = saved;
    raw.c_lflag &= static_cast<tcflag_t> (~(ICANON | ECHO));   // ISIG kept: Ctrl-C quits
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr (STDIN_FILENO, TCSANOW, &raw);
#endif
    write ("\x1b[?1049h\x1b[?25l\x1b[2J");   // alternate screen, hide cursor
}

Terminal::~Terminal()
{
    write ("\x1b[0m\x1b[?25h\x1b[?1049l");   // show cursor, main screen
#if defined (_WIN32)
    SetConsoleMode (GetStdHandle (STD_OUTPUT_HANDLE), savedMode);
    SetConsoleOutputCP (savedCodePage);
#else
    tcsetattr (STDIN_FILENO, TCSANOW, &saved);
#endif
}

void Terminal::size (int& columns, int& rows)
{
    columns = 80;
    rows = 24;
#if defined (_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo (GetStdHandle (STD_OUTPUT_HANDLE), &info))
    {
        columns = info.srWindow.Right - info.srWindow.Left + 1;
        rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
#else
    winsize w {};
    if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0)
    {
        columns = w.ws_col;
        rows = w.ws_row;
    }
#endif
}

int Terminal::key()
{
#if defined (_WIN32)
    if (! _kbhit())
        return 0;
    const int c = (int) _getwch();
    if (c == 0 || c == 0xE0)   // special keys come as a prefix and a scan code
    {
        switch (_getwch())
        {
            case 72: return Up;
            case 80: return Down;
            case 77: return Right;
            case 75: return Left;
            case 73: return PageUp;
            case 81: return PageDown;
            case 71: return Home;
            case 79: return End;
            case 83: return Delete;
            default: return 0;
        }
    }
    switch (c)
    {
        case 8:   return Backspace;
        case 9:   return Tab;
        case 13:  return Enter;
        case 27:  return Escape;
        default:  break;
    }
    if (c >= 0xD800 && c < 0xDC00)   // a surrogate pair: the rest of the code point
    {
        const int low = (int) _getwch();
        return 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
    }
    return c;
#else
    unsigned char c = 0;
    if (! next (c))
        return 0;

    switch (c)
    {
        case 8:
        case 127: return Backspace;
        case 9:   return Tab;
        case 10:
        case 13:  return Enter;
        default:  break;
    }

    if (c == 27)
    {
        // ESC [ or ESC O, then a letter or digits and '~'. A lone ESC has
        // nothing after it.
        unsigned char intro = 0, code = 0;
        if (! next (intro) || (intro != '[' && intro != 'O') || ! next (code))
            return Escape;
        switch (code)
        {
            case 'A': return Up;
            case 'B': return Down;
            case 'C': return Right;
            case 'D': return Left;
            case 'H': return Home;
            case 'F': return End;
            default:  break;
        }
        if (code < '0' || code > '9')
            return 0;
        int number = code - '0';
        unsigned char more = 0;
        while (next (more) && more >= '0' && more <= '9')
            number = number * 10 + (more - '0');
        if (more != '~')
            return 0;
        switch (number)
        {
            case 1: case 7: return Home;
            case 4: case 8: return End;
            case 3: return Delete;
            case 5: return PageUp;
            case 6: return PageDown;
            default: return 0;
        }
    }

    // UTF-8: the lead byte says how many follow.
    if (c < 0x80)
        return c;
    const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra == 0)
        return 0;
    int codePoint = c & (0x3F >> extra);
    for (int i = 0; i < extra; ++i)
    {
        unsigned char b = 0;
        if (! next (b) || (b & 0xC0) != 0x80)
            return 0;
        codePoint = (codePoint << 6) | (b & 0x3F);
    }
    return codePoint;
#endif
}

void Terminal::draw (const std::string& frame)
{
    write ("\x1b[H" + frame + "\x1b[J");
}

} // namespace fxmenet::top
