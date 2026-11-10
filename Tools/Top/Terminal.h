/*
  ------------------------------------------------------------------------------
    Terminal.h  (fxmenet-top)

    A full-screen terminal session: alternate screen, hidden cursor, keys
    read without echo or line buffering. Restores everything on
    destruction. Linux, macOS and Windows (a console with virtual terminal
    sequences: Windows 10 and later). Platform code in Terminal.cpp only.
    After oto-top's (otomatotamoto).

    Author: Olivier Doaré, github.com/odoare
    SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-FXME-Commercial
  ------------------------------------------------------------------------------
*/

#pragma once

#include <string>

namespace fxmenet::top
{

class Terminal
{
public:
    Terminal();
    ~Terminal();

    Terminal (const Terminal&) = delete;
    Terminal& operator= (const Terminal&) = delete;

    /** Columns and rows (80 x 24 if unknown). */
    static void size (int& columns, int& rows);

    /** Keys that are not characters: above every Unicode code point. */
    enum Key
    {
        Up = 0x110000, Down, Right, Left, PageUp, PageDown, Home, End, Delete,
        Backspace, Enter, Escape, Tab
    };

    /** A key if one is waiting, else 0. Never blocks. A character comes as
        its Unicode code point, the others as Key values. */
    int key();

    /** Writes a whole frame at once (home, lines, clear below). */
    void draw (const std::string& frame);
};

/** A code point in UTF-8. */
std::string utf8 (int codePoint);

} // namespace fxmenet::top
