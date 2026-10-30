/*
    ftpd.c - a small FTP server for syncing the SD card over Wi-Fi

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

/*
    Only what a sync client needs is implemented: passive mode, binary transfers, LIST/SIZE for checking
    what is already on the card, STOR/RETR, MKD/DELE/RMD and directory navigation. One client at a time.
*/

#include "ftpd.h"
#include "console.h"
#include "pad.h"
#include "shutdown.h"
#include "util.h"
#include "sd.h"
#include "update/update.h"
#include <wiisocket.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <gccore.h>
#include <ogc/isfs.h>
#include <malloc.h>
#include <dir.h>

#define FTPD_PORT 21
#define FTPD_PASV_PORT_FIRST 50000
#define FTPD_PASV_PORT_COUNT 1000
#define FTPD_PATH_MAX 768
#define FTPD_LINE_MAX 1024
#define FTPD_BUFFER_SIZE (64 * 1024)
/* libwiisocket copies every send/recv buffer through its own 64KB heap, so socket calls must stay well below that. */
#define FTPD_NET_CHUNK (8 * 1024)
#define FTPD_DATA_TIMEOUT_MS 15000
/* A transfer that makes no progress for this long is dropped, so a lost connection can't freeze the Wii. */
#define FTPD_IO_TIMEOUT_MS 30000
#define FTPD_POLL_MS 50

#define FTPD_NAND_DIR "/_nand"
#define FTPD_NAND_RATING_NAME "RRRating.pul"
/* Where Pulsar keeps Retro Rewind's VR and BR ratings. */
#define FTPD_NAND_RATING_PATH "/shared2/Pulsar/RetroRewind6/RRRating.pul"
#define FTPD_RATING_BACKUP "sd:/" RRC_RETRO_REWIND_CHANNEL_DIR "/RRRating.pul.bak"
#define FTPD_NAND_FILE_MAX FTPD_BUFFER_SIZE
#define FTPD_NAND_GHOSTS_NAME "Ghosts"
/* Where Pulsar keeps time trial ghosts: one folder per track ID, a folder per speed, and a leaderboard (ldb.pul). */
#define FTPD_NAND_GHOSTS_PATH "/shared2/Pulsar/RetroRewind6/Ghosts"
/* NAND names are at most 12 characters, and ghosts sit at most 3 folders deep ("<track>/150/<time>.rkg"). */
#define FTPD_NAND_NAME_MAX 12
#ifndef ISFS_MAXPATH
#define ISFS_MAXPATH 64
#endif
#define FTPD_NAND_GHOSTS_DEPTH 3
/* IOS file system error for a file that does not exist. */
#define FTPD_ISFS_ENOENT (-106)

/*
    The screen: the address and buttons on top, then who is connected and the totals for this visit, then a log of
    the latest events (connections, each file received or sent, errors).
*/
#define FTPD_HEADER_ROW 1
#define FTPD_SD_ROW 4
#define FTPD_CONNECTION_ROW 5
#define FTPD_TOTALS_ROW 6
#define FTPD_LOG_ROW 8
#define FTPD_LOG_LINES 8
#define FTPD_LOG_WIDTH 96
/* Added to the greeting while the player asked (A button) for the save and VR to be copied to the PC. */
#define FTPD_COPY_REQUEST "Copy to PC requested."
/*
    Wheel Wizard+ checks in every few seconds while its Sync to Wii page is open (connect, read the greeting, quit).
    It counts as connected until it hasn't checked in for this long.
*/
#define FTPD_PRESENCE_SECONDS 12

struct ftpd_session
{
    int control;
    int pasv_listen;
    /* The client's current directory, as an FTP path: always starts with '/', no trailing slash except the root. */
    char cwd[FTPD_PATH_MAX];
    char line[FTPD_LINE_MAX];
    int line_len;
    u32 ip;
    int next_pasv_port;
    char *buffer;
    int *files_received;
    int files_sent;
    /* Files sent during this connection; a finished copy to the PC clears the A button request. */
    int session_sent;
    int session_received;
    bool copy_requested;
    bool logged_in;
    char peer[16];
    /* Whether Wheel Wizard+ is around (it checked in lately), and when it last did. */
    bool present;
    time_t last_contact;
    bool quit;
};

static bool ftpd_wait(int fd, short events)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    return poll(&pfd, 1, FTPD_IO_TIMEOUT_MS) > 0;
}

static int ftpd_send_all(int fd, const char *data, int len)
{
    while (len > 0)
    {
        if (!ftpd_wait(fd, POLLOUT))
            return -1;
        int sent = send(fd, data, len < FTPD_NET_CHUNK ? len : FTPD_NET_CHUNK, 0);
        if (sent <= 0)
            return -1;
        data += sent;
        len -= sent;
    }
    return 0;
}

/* Receives up to `len' bytes (at most one chunk). Returns 0 at the end of the stream and -1 on error or timeout. */
static int ftpd_recv(int fd, char *buffer, int len)
{
    if (!ftpd_wait(fd, POLLIN))
        return -1;
    return recv(fd, buffer, len < FTPD_NET_CHUNK ? len : FTPD_NET_CHUNK, 0);
}

static void ftpd_reply(struct ftpd_session *s, const char *fmt, ...)
{
    char msg[FTPD_PATH_MAX + 64];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(msg, sizeof(msg) - 2, fmt, args);
    va_end(args);
    if (len < 0)
        return;
    if (len > (int)sizeof(msg) - 3)
        len = sizeof(msg) - 3;
    msg[len++] = '\r';
    msg[len++] = '\n';
    ftpd_send_all(s->control, msg, len);
}

/* Shortens text wider than `max' by cutting it after its first `keep' characters, since it would wrap onto the rows below. */
static void ftpd_fit(char *msg, int max, int keep)
{
    int len = strlen(msg);
    if (len <= max || max < keep + 4)
        return;
    int tail = max - keep - 3;
    memmove(msg + keep + 3, msg + len - tail, tail + 1);
    memcpy(msg + keep, "...", 3);
}

static void ftpd_show(int row, const char *fmt, ...)
{
    char msg[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    ftpd_fit(msg, rrc_con_get_cols() - 2, 0);
    rrc_con_clear_line(row);
    rrc_con_print_text_centered(row, msg);
}

static void ftpd_show_totals(const struct ftpd_session *s)
{
    ftpd_show(FTPD_TOTALS_ROW, "Received %d   Sent %d", *s->files_received, s->files_sent);
}

static void ftpd_size_text(char *out, int out_size, unsigned long long bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(out, out_size, "%llu.%llu GB", bytes / (1024ULL * 1024 * 1024), bytes * 10 / (1024ULL * 1024 * 1024) % 10);
    else if (bytes >= 1024 * 1024)
        snprintf(out, out_size, "%llu.%llu MB", bytes / (1024 * 1024), bytes * 10 / (1024 * 1024) % 10);
    else if (bytes >= 1024)
        snprintf(out, out_size, "%llu KB", (bytes + 512) / 1024);
    else
        snprintf(out, out_size, "%llu bytes", bytes);
}

static void ftpd_show_free_space(void)
{
    unsigned long long free_bytes = 0;
    if (rrc_result_is_error(rrc_sd_get_free_space(&free_bytes)))
    {
        ftpd_show(FTPD_SD_ROW, "SD card: free space unknown");
        return;
    }
    char size[24];
    ftpd_size_text(size, sizeof(size), free_bytes);
    ftpd_show(FTPD_SD_ROW, "SD card: %s free", size);
}

static char ftpd_log_lines[FTPD_LOG_LINES][FTPD_LOG_WIDTH];
static int ftpd_log_count;

static void ftpd_log_draw(void)
{
    int width = rrc_con_get_cols() - 2;
    for (int i = 0; i < FTPD_LOG_LINES; i++)
    {
        // Padding to the full width clears what was there before, in one print.
        rrc_con_cursor_seek_to(FTPD_LOG_ROW + i, 1);
        printf("%-*s", width, i < ftpd_log_count ? ftpd_log_lines[i] : "");
    }
}

/* Adds a line with the time in front, or with `replace' overwrites the newest one ("Receiving x" -> "Received x"). */
static void ftpd_vlog(bool replace, const char *fmt, va_list args)
{
    if (!replace || ftpd_log_count == 0)
    {
        if (ftpd_log_count == FTPD_LOG_LINES)
            memmove(ftpd_log_lines[0], ftpd_log_lines[1], sizeof(ftpd_log_lines[0]) * (FTPD_LOG_LINES - 1));
        else
            ftpd_log_count++;
    }

    char *line = ftpd_log_lines[ftpd_log_count - 1];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int stamp = tm != NULL ? (int)strftime(line, FTPD_LOG_WIDTH, "%H:%M ", tm) : 0;
    vsnprintf(line + stamp, FTPD_LOG_WIDTH - stamp, fmt, args);
    // Long file paths lose their middle, so both the time and the file name stay readable.
    ftpd_fit(line, rrc_con_get_cols() - 2, stamp);
    ftpd_log_draw();
}

static void ftpd_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ftpd_vlog(false, fmt, args);
    va_end(args);
}

static void ftpd_log_update(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    ftpd_vlog(true, fmt, args);
    va_end(args);
}

/* Resolves `arg' against the current directory into a normalized FTP path ("/a/b"). */
static void ftpd_resolve(const struct ftpd_session *s, const char *arg, char *out)
{
    char joined[FTPD_PATH_MAX * 2];
    if (arg == NULL || *arg == 0)
        snprintf(joined, sizeof(joined), "%s", s->cwd);
    else if (arg[0] == '/')
        snprintf(joined, sizeof(joined), "%s", arg);
    else
        snprintf(joined, sizeof(joined), "%s/%s", s->cwd, arg);

    int out_len = 0;
    out[0] = 0;
    char *save = NULL;
    for (char *part = strtok_r(joined, "/", &save); part != NULL; part = strtok_r(NULL, "/", &save))
    {
        if (strcmp(part, ".") == 0)
            continue;
        if (strcmp(part, "..") == 0)
        {
            char *slash = strrchr(out, '/');
            out_len = slash ? slash - out : 0;
            out[out_len] = 0;
            continue;
        }
        int part_len = strlen(part);
        if (out_len + part_len + 2 >= FTPD_PATH_MAX)
            break;
        out[out_len++] = '/';
        memcpy(out + out_len, part, part_len);
        out_len += part_len;
        out[out_len] = 0;
    }

    if (out_len == 0)
        strcpy(out, "/");
}

/* FTP path -> SD card path ("/a/b" -> "sd:/a/b"). */
static void ftpd_to_sd(const char *ftp_path, char *out)
{
    snprintf(out, FTPD_PATH_MAX + 4, "sd:%s", ftp_path);
}

static void ftpd_resolve_sd(const struct ftpd_session *s, const char *arg, char *out)
{
    char ftp_path[FTPD_PATH_MAX];
    ftpd_resolve(s, arg, ftp_path);
    ftpd_to_sd(ftp_path, out);
}

static void ftpd_close_pasv(struct ftpd_session *s)
{
    if (s->pasv_listen >= 0)
    {
        close(s->pasv_listen);
        s->pasv_listen = -1;
    }
}

static int ftpd_listen_on(u16 port, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    // Lets Wi-Fi sync be reopened right after it was closed. Not every IOS supports it, so failure is ignored.
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, backlog) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void ftpd_cmd_pasv(struct ftpd_session *s)
{
    ftpd_close_pasv(s);
    for (int attempt = 0; attempt < 20 && s->pasv_listen < 0; attempt++)
    {
        u16 port = FTPD_PASV_PORT_FIRST + s->next_pasv_port;
        s->next_pasv_port = (s->next_pasv_port + 1) % FTPD_PASV_PORT_COUNT;
        int fd = ftpd_listen_on(port, 1);
        if (fd >= 0)
        {
            s->pasv_listen = fd;
            ftpd_reply(s, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
                       (s->ip >> 24) & 0xff, (s->ip >> 16) & 0xff, (s->ip >> 8) & 0xff, s->ip & 0xff,
                       port >> 8, port & 0xff);
            return;
        }
    }
    ftpd_reply(s, "425 Could not open a data port.");
}

/* Waits for the client to connect to the passive port. Returns the data socket, or -1 after replying with an error. */
static int ftpd_open_data(struct ftpd_session *s)
{
    if (s->pasv_listen < 0)
    {
        ftpd_reply(s, "425 Use PASV first.");
        return -1;
    }

    struct pollfd pfd = {.fd = s->pasv_listen, .events = POLLIN};
    int data = -1;
    if (poll(&pfd, 1, FTPD_DATA_TIMEOUT_MS) > 0)
    {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        data = accept(s->pasv_listen, (struct sockaddr *)&addr, &addr_len);
    }
    ftpd_close_pasv(s);

    if (data < 0)
        ftpd_reply(s, "425 Data connection failed.");
    return data;
}

static void ftpd_format_entry(char *out, int out_size, const char *name, const struct stat *st)
{
    char date[16] = "Jan 01  2000";
    struct tm *tm = localtime(&st->st_mtime);
    if (tm != NULL)
        strftime(date, sizeof(date), "%b %d  %Y", tm);

    bool dir = S_ISDIR(st->st_mode);
    snprintf(out, out_size, "%s 1 wii wii %llu %s %s\r\n", dir ? "drwxr-xr-x" : "-rw-r--r--",
             dir ? 0ULL : (unsigned long long)st->st_size, date, name);
}

static void ftpd_cmd_list(struct ftpd_session *s, const char *arg, bool names_only)
{
    // Skip "ls" style flags such as "-a" or "-la".
    while (arg != NULL && arg[0] == '-')
    {
        const char *space = strchr(arg, ' ');
        arg = space ? space + 1 : NULL;
    }

    char path[FTPD_PATH_MAX + 4];
    ftpd_resolve_sd(s, arg, path);
    struct stat st;
    if (stat(path, &st) != 0)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "550 No such file or directory.");
        return;
    }

    ftpd_reply(s, "150 Here comes the listing.");
    int data = ftpd_open_data(s);
    if (data < 0)
        return;

    char entry[FTPD_PATH_MAX + 96];
    bool ok = true;
    if (!S_ISDIR(st.st_mode))
    {
        const char *name = strrchr(path, '/');
        name = name ? name + 1 : path;
        if (names_only)
            snprintf(entry, sizeof(entry), "%s\r\n", name);
        else
            ftpd_format_entry(entry, sizeof(entry), name, &st);
        ok = ftpd_send_all(data, entry, strlen(entry)) == 0;
    }
    else
    {
        DIR *dir = opendir(path);
        struct dirent *ent;
        while (ok && dir != NULL && (ent = readdir(dir)) != NULL)
        {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            if (names_only)
            {
                snprintf(entry, sizeof(entry), "%s\r\n", ent->d_name);
            }
            else
            {
                char child[FTPD_PATH_MAX * 2];
                snprintf(child, sizeof(child), "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", ent->d_name);
                struct stat child_st;
                if (stat(child, &child_st) != 0)
                    continue;
                ftpd_format_entry(entry, sizeof(entry), ent->d_name, &child_st);
            }
            ok = ftpd_send_all(data, entry, strlen(entry)) == 0;
        }
        if (dir != NULL)
            closedir(dir);

        // The card's root also shows the virtual folder that holds the VR rating file from NAND.
        if (ok && strcmp(path, "sd:/") == 0)
        {
            if (names_only)
                snprintf(entry, sizeof(entry), "%s\r\n", FTPD_NAND_DIR + 1);
            else
                snprintf(entry, sizeof(entry), "drwxr-xr-x 1 wii wii 0 Jan 01  2000 %s\r\n", FTPD_NAND_DIR + 1);
            ok = ftpd_send_all(data, entry, strlen(entry)) == 0;
        }
    }

    close(data);
    if (ok)
        ftpd_reply(s, "226 Listing done.");
    else
        ftpd_reply(s, "426 Connection lost.");
}

/*
    The file is written next to its destination first and only replaces it once it arrived in full, so a dropped
    connection never leaves a half-written save or pack file behind.
*/
static void ftpd_cmd_stor(struct ftpd_session *s, const char *arg)
{
    char path[FTPD_PATH_MAX + 4];
    char temp_path[FTPD_PATH_MAX + 16];
    ftpd_resolve_sd(s, arg, path);
    snprintf(temp_path, sizeof(temp_path), "%s.wwsync", path);
    FILE *file = fopen(temp_path, "wb");
    if (file == NULL)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "553 Could not create the file (is the SD card locked?).");
        return;
    }

    const char *name = strrchr(path, '/');
    // Just the file name, e.g. "rksys.dat".
    const char *label = name ? name + 1 : path;
    ftpd_log("Receiving %s", label);
    ftpd_reply(s, "150 Ready to receive.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        fclose(file);
        unlink(temp_path);
        ftpd_log_update("Failed: %s (no connection)", label);
        return;
    }

    // Network reads are small, so collect a full buffer before each write to keep SD card writes large.
    bool ok = true;
    bool done = false;
    unsigned long long total = 0;
    while (ok && !done)
    {
        int filled = 0;
        while (filled < FTPD_BUFFER_SIZE)
        {
            int received = ftpd_recv(data, s->buffer + filled, FTPD_BUFFER_SIZE - filled);
            if (received == 0)
            {
                done = true;
                break;
            }
            if (received < 0)
            {
                ok = false;
                break;
            }
            filled += received;
        }
        if (ok && filled > 0 && fwrite(s->buffer, 1, filled, file) != (size_t)filled)
            ok = false;
        total += filled;
    }
    close(data);

    if (fclose(file) != 0)
        ok = false;

    if (!ok)
    {
        unlink(temp_path);
        ftpd_log_update("Failed: %s (lost connection or SD full)", label);
        ftpd_reply(s, "451 Transfer failed (lost connection, or the SD card is full).");
        return;
    }

    // FAT can't rename over an existing file, so the old one is removed first.
    unlink(path);
    if (rename(temp_path, path) != 0)
    {
        // Keep the complete copy rather than lose both.
        ftpd_log_update("Failed: %s (could not rename)", label);
        ftpd_reply(s, "451 Received, but could not rename it from %s.wwsync.", name ? name + 1 : path);
        return;
    }

    (*s->files_received)++;
    s->session_received++;
    char size[24];
    ftpd_size_text(size, sizeof(size), total);
    ftpd_log_update("Received %s (%s)", label, size);
    ftpd_show_totals(s);
    ftpd_reply(s, "226 File received.");
}

static void ftpd_cmd_retr(struct ftpd_session *s, const char *arg)
{
    char path[FTPD_PATH_MAX + 4];
    ftpd_resolve_sd(s, arg, path);
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "550 No such file.");
        return;
    }

    const char *slash = strrchr(path, '/');
    const char *label = slash ? slash + 1 : path;
    ftpd_log("Sending %s", label);
    ftpd_reply(s, "150 Sending the file.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        fclose(file);
        ftpd_log_update("Failed: %s (no connection)", label);
        return;
    }

    bool ok = true;
    unsigned long long total = 0;
    size_t read;
    while ((read = fread(s->buffer, 1, FTPD_BUFFER_SIZE, file)) > 0)
    {
        if (ftpd_send_all(data, s->buffer, read) != 0)
        {
            ok = false;
            break;
        }
        total += read;
    }
    fclose(file);
    close(data);
    if (ok)
    {
        s->files_sent++;
        s->session_sent++;
        char size[24];
        ftpd_size_text(size, sizeof(size), total);
        ftpd_log_update("Sent %s (%s)", label, size);
        ftpd_show_totals(s);
    }
    else
        ftpd_log_update("Failed: %s (lost connection)", label);
    ftpd_reply(s, ok ? "226 File sent." : "426 Connection lost.");
}

/*
    Two things Retro Rewind keeps in the Wii's internal memory (NAND) are reachable through a virtual "/_nand" folder:
    the VR rating file, and the time trial ghosts folder (one folder per track, named by the track's ID). Nothing else
    in NAND is reachable. Ghosts can be added but never replaced or deleted, so a sync can't lose one.
*/

static const char *ftpd_skip_list_flags(const char *arg)
{
    while (arg != NULL && arg[0] == '-')
    {
        const char *space = strchr(arg, ' ');
        arg = space ? space + 1 : NULL;
    }
    return arg;
}

static bool ftpd_in_nand(const char *ftp_path)
{
    return strcasecmp(ftp_path, FTPD_NAND_DIR) == 0 || strncasecmp(ftp_path, FTPD_NAND_DIR "/", strlen(FTPD_NAND_DIR) + 1) == 0;
}

static bool ftpd_is_nand_rating(const char *ftp_path)
{
    return strcasecmp(ftp_path, FTPD_NAND_DIR "/" FTPD_NAND_RATING_NAME) == 0;
}

/*
    "/_nand/Ghosts/a/b" -> "/shared2/Pulsar/RetroRewind6/Ghosts/a/b". Returns false for paths outside the ghosts
    folder, too deep, or with a name NAND can't hold (at most 12 letters, digits, '.', '_' or '-').
*/
static bool ftpd_nand_ghost_path(const char *ftp_path, char *out, int out_size)
{
    const char *prefix = FTPD_NAND_DIR "/" FTPD_NAND_GHOSTS_NAME;
    int prefix_len = strlen(prefix);
    if (strncasecmp(ftp_path, prefix, prefix_len) != 0 || (ftp_path[prefix_len] != 0 && ftp_path[prefix_len] != '/'))
        return false;

    // ftpd_resolve already removed "." and ".." and empty parts.
    const char *rest = ftp_path + prefix_len;
    int depth = 0;
    int part_len = 0;
    for (const char *c = rest; *c; c++)
    {
        if (*c == '/')
        {
            depth++;
            part_len = 0;
            continue;
        }
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-')
            return false;
        if (++part_len > FTPD_NAND_NAME_MAX)
            return false;
    }
    if (depth > FTPD_NAND_GHOSTS_DEPTH)
        return false;
    return snprintf(out, out_size, "%s%s", FTPD_NAND_GHOSTS_PATH, rest) < out_size;
}

/* What a NAND path is: 1 for a file (with its size), 2 for a folder, 0 if it doesn't exist or can't be read. */
static int ftpd_nand_stat(const char *path, u32 *size)
{
    *size = 0;
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd >= 0)
    {
        fstats stats __attribute__((aligned(32)));
        if (ISFS_GetFileStats(fd, &stats) >= 0)
            *size = stats.file_length;
        ISFS_Close(fd);
        return 1;
    }
    u32 count = 0;
    return ISFS_ReadDir(path, NULL, &count) >= 0 ? 2 : 0;
}

/* Reads a NAND file into a new 32-byte aligned buffer. Returns its length, or a negative ISFS error. */
static s32 ftpd_nand_read(const char *path, u8 **out)
{
    *out = NULL;
    s32 fd = ISFS_Open(path, ISFS_OPEN_READ);
    if (fd < 0)
        return fd;

    fstats stats __attribute__((aligned(32)));
    s32 res = ISFS_GetFileStats(fd, &stats);
    if (res >= 0 && (stats.file_length == 0 || stats.file_length > FTPD_NAND_FILE_MAX))
        res = ISFS_EINVAL;
    if (res >= 0)
    {
        *out = memalign(32, align_up(stats.file_length, 32));
        res = *out == NULL ? ISFS_ENOMEM : ISFS_Read(fd, *out, stats.file_length);
        if (res >= 0 && res != (s32)stats.file_length)
            res = ISFS_EINVAL;
    }
    ISFS_Close(fd);

    if (res < 0)
    {
        free(*out);
        *out = NULL;
        return res;
    }
    return stats.file_length;
}

/* Creates the missing folders above `path', readable and writable by everyone so the game can use them. */
static void ftpd_nand_make_parents(const char *path)
{
    char folder[ISFS_MAXPATH];
    for (const char *slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/'))
    {
        int len = slash - path;
        if (len >= (int)sizeof(folder))
            return;
        memcpy(folder, path, len);
        folder[len] = 0;
        if (strcmp(folder, "/shared2") != 0)
            ISFS_CreateDir(folder, 0, 3, 3, 3);
    }
}

/*
    Writes a NAND file. With `old_len' the same as `len' it is overwritten in place, which keeps the owner and
    permissions the game set. Otherwise it is (re)created readable and writable by everyone, so the game can still
    update it. `old_len' is negative when there is no file yet.
*/
static s32 ftpd_nand_write(const char *path, const char *data, u32 len, s32 old_len)
{
    u8 *buffer = memalign(32, align_up(len, 32));
    if (buffer == NULL)
        return ISFS_ENOMEM;
    memcpy(buffer, data, len);

    if (old_len != (s32)len)
    {
        if (old_len >= 0)
            ISFS_Delete(path);
        ftpd_nand_make_parents(path);
        s32 created = ISFS_CreateFile(path, 0, 3, 3, 3);
        if (created < 0)
        {
            free(buffer);
            return created;
        }
    }

    s32 fd = ISFS_Open(path, ISFS_OPEN_WRITE);
    s32 res = fd;
    if (fd >= 0)
    {
        res = ISFS_Write(fd, buffer, len);
        ISFS_Close(fd);
    }
    free(buffer);
    return res < 0 ? res : (res == (s32)len ? 0 : ISFS_EINVAL);
}

/* Replaces the rating file in NAND. The old one is copied to the SD card first. */
static s32 ftpd_nand_write_rating(const char *data, u32 len)
{
    u8 *old = NULL;
    s32 old_len = ftpd_nand_read(FTPD_NAND_RATING_PATH, &old);
    // A file that exists but can't be read would be lost without a backup, so leave it alone.
    if (old_len < 0 && old_len != FTPD_ISFS_ENOENT)
        return old_len;
    if (old_len > 0)
    {
        FILE *backup = fopen(FTPD_RATING_BACKUP, "wb");
        bool backed_up = backup != NULL && fwrite(old, 1, old_len, backup) == (size_t)old_len;
        if (backup != NULL && fclose(backup) != 0)
            backed_up = false;
        free(old);
        if (!backed_up)
            return ISFS_EINVAL;
    }
    return ftpd_nand_write(FTPD_NAND_RATING_PATH, data, len, old_len);
}

static bool ftpd_nand_send_entry(int data, bool names_only, const char *name, bool dir, u32 size)
{
    char entry[96];
    if (names_only)
        snprintf(entry, sizeof(entry), "%s\r\n", name);
    else
        snprintf(entry, sizeof(entry), "%s 1 wii wii %u Jan 01  2000 %s\r\n", dir ? "drwxr-xr-x" : "-rw-r--r--",
                 dir ? 0 : (unsigned)size, name);
    return ftpd_send_all(data, entry, strlen(entry)) == 0;
}

static void ftpd_nand_list(struct ftpd_session *s, const char *ftp_path, bool names_only)
{
    bool root = strcasecmp(ftp_path, FTPD_NAND_DIR) == 0;
    char nand_path[ISFS_MAXPATH];
    char *names = NULL;
    u32 count = 0;
    if (!root)
    {
        u32 size;
        int kind = ftpd_nand_ghost_path(ftp_path, nand_path, sizeof(nand_path)) ? ftpd_nand_stat(nand_path, &size) : 0;
        bool ghosts_root = strcasecmp(ftp_path, FTPD_NAND_DIR "/" FTPD_NAND_GHOSTS_NAME) == 0;
        // The ghosts folder is listed (empty) even before the game made it.
        if (kind != 2 && !ghosts_root)
        {
            ftpd_close_pasv(s);
            ftpd_reply(s, "550 No such folder.");
            return;
        }
        if (kind == 2 && ISFS_ReadDir(nand_path, NULL, &count) >= 0 && count > 0)
        {
            names = memalign(32, align_up(count * (FTPD_NAND_NAME_MAX + 1), 32));
            if (names == NULL || ISFS_ReadDir(nand_path, names, &count) < 0)
                count = 0;
        }
    }

    ftpd_reply(s, "150 Here comes the listing.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        free(names);
        return;
    }

    bool ok = true;
    if (root)
    {
        u32 size;
        if (ftpd_nand_stat(FTPD_NAND_RATING_PATH, &size) == 1 && size > 0)
            ok = ftpd_nand_send_entry(data, names_only, FTPD_NAND_RATING_NAME, false, size);
        if (ok)
            ok = ftpd_nand_send_entry(data, names_only, FTPD_NAND_GHOSTS_NAME, true, 0);
    }
    else
    {
        // ISFS_ReadDir gives the names one after another, each ending in a 0.
        const char *name = names;
        for (u32 i = 0; ok && name != NULL && i < count; i++, name += strlen(name) + 1)
        {
            char child[ISFS_MAXPATH];
            u32 size;
            if (snprintf(child, sizeof(child), "%s/%s", nand_path, name) >= (int)sizeof(child))
                continue;
            int kind = ftpd_nand_stat(child, &size);
            if (kind != 0)
                ok = ftpd_nand_send_entry(data, names_only, name, kind == 2, size);
        }
    }
    free(names);
    close(data);
    ftpd_reply(s, ok ? "226 Listing done." : "426 Connection lost.");
}

static void ftpd_nand_retr(struct ftpd_session *s, const char *nand_path, const char *label)
{
    u8 *file = NULL;
    s32 len = ftpd_nand_read(nand_path, &file);
    if (len <= 0)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "550 No such file.");
        return;
    }

    ftpd_log("Sending %s", label);
    ftpd_reply(s, "150 Sending the file.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        free(file);
        ftpd_log_update("Failed: %s (no connection)", label);
        return;
    }
    bool ok = ftpd_send_all(data, (const char *)file, len) == 0;
    free(file);
    close(data);
    if (ok)
    {
        s->files_sent++;
        s->session_sent++;
        char size[24];
        ftpd_size_text(size, sizeof(size), len);
        ftpd_log_update("Sent %s (%s)", label, size);
        ftpd_show_totals(s);
    }
    else
        ftpd_log_update("Failed: %s (lost connection)", label);
    ftpd_reply(s, ok ? "226 File sent." : "426 Connection lost.");
}

/*
    Receives a small upload whole into the session buffer, before anything in NAND is touched. Returns its length,
    -1 if it didn't arrive in full, or -2 if there was no data connection (the client was already told).
*/
static int ftpd_nand_receive(struct ftpd_session *s)
{
    ftpd_reply(s, "150 Ready to receive.");
    int data = ftpd_open_data(s);
    if (data < 0)
        return -2;

    int filled = 0;
    bool ok = true;
    while (1)
    {
        if (filled >= FTPD_BUFFER_SIZE)
        {
            ok = false;
            break;
        }
        int received = ftpd_recv(data, s->buffer + filled, FTPD_BUFFER_SIZE - filled);
        if (received == 0)
            break;
        if (received < 0)
        {
            ok = false;
            break;
        }
        filled += received;
    }
    close(data);
    return ok && filled > 0 ? filled : -1;
}

static void ftpd_nand_stor_rating(struct ftpd_session *s)
{
    ftpd_log("Receiving VR rating");
    int len = ftpd_nand_receive(s);
    if (len < 0)
    {
        ftpd_log_update("Failed: VR rating (lost connection)");
        if (len == -1)
            ftpd_reply(s, "451 The VR file did not arrive in full, so nothing was changed.");
        return;
    }

    s32 res = ftpd_nand_write_rating(s->buffer, len);
    if (res < 0)
    {
        ftpd_log_update("Failed: VR rating (error %d)", (int)res);
        ftpd_reply(s, "451 Could not write the VR file to the Wii's memory (error %d).", (int)res);
        return;
    }

    (*s->files_received)++;
    s->session_received++;
    ftpd_log_update("Received VR rating");
    ftpd_show_totals(s);
    ftpd_reply(s, "226 File received.");
}

static void ftpd_nand_stor_ghost(struct ftpd_session *s, const char *nand_path, const char *label)
{
    u32 size;
    if (ftpd_nand_stat(nand_path, &size) != 0)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "550 This file is already on the Wii, and ghosts are never replaced.");
        return;
    }

    ftpd_log("Receiving %s", label);
    int len = ftpd_nand_receive(s);
    if (len < 0)
    {
        ftpd_log_update("Failed: %s (lost connection)", label);
        if (len == -1)
            ftpd_reply(s, "451 The ghost did not arrive in full, so nothing was changed.");
        return;
    }

    s32 res = ftpd_nand_write(nand_path, s->buffer, len, -1);
    if (res < 0)
    {
        ISFS_Delete(nand_path);
        ftpd_log_update("Failed: %s (error %d)", label, (int)res);
        ftpd_reply(s, "451 Could not write the ghost to the Wii's memory (error %d).", (int)res);
        return;
    }

    (*s->files_received)++;
    s->session_received++;
    char size_text[24];
    ftpd_size_text(size_text, sizeof(size_text), len);
    ftpd_log_update("Received %s (%s)", label, size_text);
    ftpd_show_totals(s);
    ftpd_reply(s, "226 File received.");
}

/* Handles commands that touch "/_nand". Returns false for everything else. */
static bool ftpd_handle_nand(struct ftpd_session *s, const char *cmd, const char *arg)
{
    bool is_list = strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0;
    char ftp_path[FTPD_PATH_MAX];
    ftpd_resolve(s, is_list ? ftpd_skip_list_flags(arg) : arg, ftp_path);
    if (!ftpd_in_nand(ftp_path))
        return false;

    bool root = strcasecmp(ftp_path, FTPD_NAND_DIR) == 0;
    bool rating = ftpd_is_nand_rating(ftp_path);
    char nand_path[ISFS_MAXPATH];
    bool ghost = ftpd_nand_ghost_path(ftp_path, nand_path, sizeof(nand_path));
    bool ghosts_root = strcasecmp(ftp_path, FTPD_NAND_DIR "/" FTPD_NAND_GHOSTS_NAME) == 0;
    // Shown on the Wii's screen: "ghost 2m24s871.rkg" or "leaderboard".
    const char *file_name = strrchr(ftp_path, '/') + 1;
    char label[32];
    if (strcasecmp(file_name, "ldb.pul") == 0)
        snprintf(label, sizeof(label), "leaderboard");
    else
        snprintf(label, sizeof(label), "ghost %s", file_name);
    u32 size = 0;
    int kind = ghost ? ftpd_nand_stat(nand_path, &size) : 0;

    if (strcmp(cmd, "CWD") == 0 && (root || ghosts_root || kind == 2))
    {
        strcpy(s->cwd, ftp_path);
        ftpd_reply(s, "250 Directory changed.");
    }
    else if (is_list && (root || ghost))
        ftpd_nand_list(s, ftp_path, strcmp(cmd, "NLST") == 0);
    else if (strcmp(cmd, "SIZE") == 0 && (rating || kind == 1))
    {
        if (rating)
            ftpd_nand_stat(FTPD_NAND_RATING_PATH, &size);
        if (size > 0)
            ftpd_reply(s, "213 %u", (unsigned)size);
        else
            ftpd_reply(s, "550 No such file.");
    }
    else if (strcmp(cmd, "RETR") == 0 && rating)
        ftpd_nand_retr(s, FTPD_NAND_RATING_PATH, "VR rating");
    else if (strcmp(cmd, "RETR") == 0 && kind == 1)
        ftpd_nand_retr(s, nand_path, label);
    else if (strcmp(cmd, "STOR") == 0 && rating)
        ftpd_nand_stor_rating(s);
    else if (strcmp(cmd, "STOR") == 0 && ghost && !ghosts_root)
        ftpd_nand_stor_ghost(s, nand_path, label);
    else if ((strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0) && ghost)
    {
        if (kind != 2)
        {
            ftpd_nand_make_parents(nand_path);
            ISFS_CreateDir(nand_path, 0, 3, 3, 3);
            kind = ftpd_nand_stat(nand_path, &size);
        }
        if (kind == 2)
            ftpd_reply(s, "257 \"%s\" created.", ftp_path);
        else
            ftpd_reply(s, "550 Could not create the folder in the Wii's memory.");
    }
    else if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "SIZE") == 0 || strcmp(cmd, "RETR") == 0 || strcmp(cmd, "STOR") == 0 ||
             is_list || strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0 || strcmp(cmd, "DELE") == 0 ||
             strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "550 Only the VR rating and new ghosts can be used here.");
    }
    else
        return false;
    return true;
}

static void ftpd_handle(struct ftpd_session *s, char *line)
{
    char *arg = strchr(line, ' ');
    if (arg != NULL)
    {
        *arg++ = 0;
        while (*arg == ' ')
            arg++;
        if (*arg == 0)
            arg = NULL;
    }
    for (char *c = line; *c; c++)
        *c = toupper((unsigned char)*c);

    if (ftpd_handle_nand(s, line, arg))
        return;

    char path[FTPD_PATH_MAX + 4];
    struct stat st;

    if (strcmp(line, "USER") == 0)
    {
        if (!s->logged_in)
        {
            s->logged_in = true;
            s->session_sent = 0;
            s->session_received = 0;
        }
        ftpd_reply(s, "331 Any password works.");
    }
    else if (strcmp(line, "PASS") == 0)
        ftpd_reply(s, "230 Logged in.");
    else if (strcmp(line, "SYST") == 0)
        ftpd_reply(s, "215 UNIX Type: L8");
    else if (strcmp(line, "FEAT") == 0)
    {
        const char *features = "211-Features:\r\n SIZE\r\n UTF8\r\n211 End\r\n";
        ftpd_send_all(s->control, features, strlen(features));
    }
    else if (strcmp(line, "OPTS") == 0 || strcmp(line, "TYPE") == 0 || strcmp(line, "MODE") == 0 || strcmp(line, "STRU") == 0 ||
             strcmp(line, "NOOP") == 0)
        ftpd_reply(s, "200 OK.");
    else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0)
        ftpd_reply(s, "257 \"%s\" is the current directory.", s->cwd);
    else if (strcmp(line, "CWD") == 0 || strcmp(line, "CDUP") == 0)
    {
        char ftp_path[FTPD_PATH_MAX];
        ftpd_resolve(s, strcmp(line, "CDUP") == 0 ? ".." : arg, ftp_path);
        ftpd_to_sd(ftp_path, path);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        {
            strcpy(s->cwd, ftp_path);
            ftpd_reply(s, "250 Directory changed.");
        }
        else
            ftpd_reply(s, "550 No such directory.");
    }
    else if (strcmp(line, "PASV") == 0)
        ftpd_cmd_pasv(s);
    else if (strcmp(line, "LIST") == 0 || strcmp(line, "NLST") == 0)
        ftpd_cmd_list(s, arg, strcmp(line, "NLST") == 0);
    else if (strcmp(line, "SIZE") == 0)
    {
        ftpd_resolve_sd(s, arg, path);
        if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode))
            ftpd_reply(s, "213 %llu", (unsigned long long)st.st_size);
        else
            ftpd_reply(s, "550 No such file.");
    }
    else if (strcmp(line, "STOR") == 0 && arg != NULL)
        ftpd_cmd_stor(s, arg);
    else if (strcmp(line, "RETR") == 0 && arg != NULL)
        ftpd_cmd_retr(s, arg);
    else if ((strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) && arg != NULL)
    {
        char ftp_path[FTPD_PATH_MAX];
        ftpd_resolve(s, arg, ftp_path);
        ftpd_to_sd(ftp_path, path);
        if (mkdir(path, 0777) == 0)
            ftpd_reply(s, "257 \"%s\" created.", ftp_path);
        else
            ftpd_reply(s, "550 Could not create the directory.");
    }
    else if (strcmp(line, "DELE") == 0 && arg != NULL)
    {
        ftpd_resolve_sd(s, arg, path);
        bool deleted = unlink(path) == 0;
        const char *deleted_name = strrchr(path, '/');
        ftpd_log(deleted ? "Removed %s" : "Failed: remove %s", deleted_name ? deleted_name + 1 : path);
        ftpd_reply(s, deleted ? "250 Deleted." : "550 Could not delete the file.");
    }
    else if ((strcmp(line, "RMD") == 0 || strcmp(line, "XRMD") == 0) && arg != NULL)
    {
        ftpd_resolve_sd(s, arg, path);
        bool removed = rmdir(path) == 0;
        if (!removed)
            ftpd_log("Failed: remove folder %s", path + 4);
        ftpd_reply(s, removed ? "250 Removed." : "550 Could not remove the directory.");
    }
    else if (strcmp(line, "ABOR") == 0)
    {
        ftpd_close_pasv(s);
        ftpd_reply(s, "226 Aborted.");
    }
    else if (strcmp(line, "QUIT") == 0)
    {
        ftpd_reply(s, "221 Bye.");
        s->quit = true;
    }
    else
        ftpd_reply(s, "502 Not supported.");
}

/* Reads what the client sent and runs every complete command line. Returns false when the client is gone. */
static bool ftpd_read_commands(struct ftpd_session *s)
{
    int received = recv(s->control, s->line + s->line_len, FTPD_LINE_MAX - 1 - s->line_len, 0);
    if (received <= 0)
        return false;
    s->line_len += received;
    s->line[s->line_len] = 0;

    char *start = s->line;
    char *end;
    while (!s->quit && (end = strstr(start, "\r\n")) != NULL)
    {
        *end = 0;
        ftpd_handle(s, start);
        start = end + 2;
    }

    s->line_len = strlen(start);
    memmove(s->line, start, s->line_len + 1);
    if (s->line_len >= FTPD_LINE_MAX - 1)
        s->line_len = 0; // A line this long is not a real command; drop it.
    return !s->quit;
}

struct rrc_result rrc_ftpd_run(void *xfb, int *files_received)
{
    *files_received = 0;
    rrc_con_clear(true);
    rrc_con_print_text_centered(_RRC_ACTION_ROW, "Connecting to Wi-Fi...");

    if (wiisocket_init() < 0)
        return rrc_result_create_error_errno(ENETDOWN, "Could not connect to Wi-Fi. Check the Wii's internet settings.");

    struct in_addr ip = {.s_addr = (u32)gethostid()};
    if (ip.s_addr == 0 || ip.s_addr == (u32)-1)
        return rrc_result_create_error_errno(ENETDOWN, "Connected, but the Wii has no IP address.");

    int server = ftpd_listen_on(FTPD_PORT, 1);
    if (server < 0)
        return rrc_result_create_error_errno(errno, "Could not start the sync server");

    struct ftpd_session s = {.control = -1, .pasv_listen = -1, .ip = ntohl(ip.s_addr), .files_received = files_received};
    s.buffer = malloc(FTPD_BUFFER_SIZE);
    if (s.buffer == NULL)
    {
        close(server);
        return rrc_result_create_error_errno(ENOMEM, "Could not start the sync server");
    }

    // Needed only for the VR rating file; if it fails, that file just reports an error.
    ISFS_Initialize();

    char address[64];
    snprintf(address, sizeof(address), "Wi-Fi sync: %s", inet_ntoa(ip));
    rrc_con_clear(true);
    rrc_con_print_text_centered(FTPD_HEADER_ROW, address);
    rrc_con_print_text_centered(FTPD_HEADER_ROW + 1, "Enter this in Wheel Wizard+ > Sync to Wii");
    rrc_con_print_text_centered(FTPD_HEADER_ROW + 2, "A: send save and VR to PC   B: exit");
    ftpd_show_free_space();
    ftpd_show(FTPD_CONNECTION_ROW, "Not connected");
    ftpd_show_totals(&s);
    ftpd_log_count = 0;
    // The greeting tells Wheel Wizard+ the pack version, so it can show it without logging in.
    char version[32] = "";
    struct rrc_version pack_version;
    if (!rrc_result_is_error(rrc_update_get_current_version(&pack_version)))
        snprintf(version, sizeof(version), " RR %d.%d.%d.", pack_version.major, pack_version.minor, pack_version.patch);
    ftpd_log("Waiting for Wheel Wizard+");

    while (1)
    {
        rrc_shutdown_check();
        struct pad_state pad = rrc_pad_buttons();
        if (rrc_pad_b_pressed(pad) || rrc_pad_home_pressed(pad))
            break;
        if (s.present && s.control < 0 && time(NULL) - s.last_contact > FTPD_PRESENCE_SECONDS)
        {
            s.present = false;
            ftpd_show(FTPD_CONNECTION_ROW, "Not connected");
            ftpd_log("Disconnected");
        }

        // Wheel Wizard+ checks the Wii every few seconds, sees the request in the greeting, and copies them.
        if (rrc_pad_a_pressed(pad))
        {
            // Pressing it again takes the request back, so it can be asked again.
            s.copy_requested = !s.copy_requested;
            ftpd_log(s.copy_requested ? "Sending save and VR to PC..." : "Cancelled");
        }

        struct pollfd pfd = {.fd = s.control >= 0 ? s.control : server, .events = POLLIN};
        if (poll(&pfd, 1, FTPD_POLL_MS) <= 0)
            continue;

        if (s.control < 0)
        {
            struct sockaddr_in addr;
            socklen_t addr_len = sizeof(addr);
            s.control = accept(server, (struct sockaddr *)&addr, &addr_len);
            if (s.control >= 0)
            {
                strcpy(s.cwd, "/");
                s.line_len = 0;
                s.quit = false;
                s.logged_in = false;
                snprintf(s.peer, sizeof(s.peer), "%s", inet_ntoa(addr.sin_addr));
                s.last_contact = time(NULL);
                if (!s.present)
                {
                    s.present = true;
                    ftpd_show(FTPD_CONNECTION_ROW, "Connected to Wheel Wizard+ (%s)", s.peer);
                    ftpd_log("Connected to Wheel Wizard+");
                }
                ftpd_reply(&s, "220 Retro Rewind Wi-Fi sync ready.%s%s", version, s.copy_requested ? " " FTPD_COPY_REQUEST : "");
            }
        }
        else if (!ftpd_read_commands(&s))
        {
            ftpd_close_pasv(&s);
            close(s.control);
            s.control = -1;
            s.last_contact = time(NULL);
            if (s.logged_in)
            {
                if (s.session_received + s.session_sent > 0)
                    ftpd_log("Sync done: %d received, %d sent", s.session_received, s.session_sent);
                ftpd_show_free_space();
                if (!rrc_result_is_error(rrc_update_get_current_version(&pack_version)))
                    snprintf(version, sizeof(version), " RR %d.%d.%d.", pack_version.major, pack_version.minor, pack_version.patch);
                if (s.copy_requested && s.session_sent > 0)
                {
                    s.copy_requested = false;
                    ftpd_log("Save and VR sent to PC");
                }
            }
        }
    }

    ftpd_close_pasv(&s);
    if (s.control >= 0)
        close(s.control);
    close(server);
    free(s.buffer);
    ISFS_Deinitialize();
    return rrc_result_success;
}
