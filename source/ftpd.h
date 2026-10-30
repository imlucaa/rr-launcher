/*
    ftpd.h - a small FTP server for syncing the SD card over Wi-Fi

    Copyright (C) 2025  Retro Rewind Team

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef RRC_FTPD_H
#define RRC_FTPD_H

#include "result.h"

/*
    Runs an FTP server that serves the SD card (sd:/) on port 21 until the user presses B or HOME.
    Wheel Wizard+ (Settings > Sync to Wii) connects to it to send the pack, patches and saves.
    `files_received' is set to the number of files written to the SD card.
*/
struct rrc_result rrc_ftpd_run(void *xfb, int *files_received);

#endif
