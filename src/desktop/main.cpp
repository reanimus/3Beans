/*
    Copyright 2023-2026 Hydr8gon

    This file is part of 3Beans.

    3Beans is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    3Beans is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with 3Beans. If not, see <https://www.gnu.org/licenses/>.
*/

#include "b3_app.h"
#include "../scripting/cli.h"
#include <exception>

wxIMPLEMENT_APP_NO_MAIN(b3App);

int main(int argc, char **argv) {
    try { launchOptions.parse(argc, argv); }
    catch (const std::exception &e) { fprintf(stderr, "%s\n%s", e.what(), commandHelp()); return 2; }
    if (launchOptions.help) { fputs(commandHelp(), stdout); return 0; }
    if (launchOptions.headless) return runHeadless(launchOptions);
    // Our parser owns CLI arguments; wxWidgets receives only the application name.
    int wxArgc = 1;
    return wxEntry(wxArgc, argv);
}
