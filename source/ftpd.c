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

#define FTPD_STATUS_ROW (_RRC_ACTION_ROW + 4)

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

static void ftpd_status(const char *fmt, ...)
{
    char msg[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    rrc_con_clear_line(FTPD_STATUS_ROW);
    rrc_con_print_text_centered(FTPD_STATUS_ROW, msg);
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
    ftpd_status("Receiving %s", name ? name + 1 : path);
    ftpd_reply(s, "150 Ready to receive.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        fclose(file);
        unlink(temp_path);
        return;
    }

    // Network reads are small, so collect a full buffer before each write to keep SD card writes large.
    bool ok = true;
    bool done = false;
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
    }
    close(data);

    if (fclose(file) != 0)
        ok = false;

    if (!ok)
    {
        unlink(temp_path);
        ftpd_reply(s, "451 Transfer failed (lost connection, or the SD card is full).");
        return;
    }

    // FAT can't rename over an existing file, so the old one is removed first.
    unlink(path);
    if (rename(temp_path, path) != 0)
    {
        // Keep the complete copy rather than lose both.
        ftpd_reply(s, "451 Received, but could not rename it from %s.wwsync.", name ? name + 1 : path);
        return;
    }

    (*s->files_received)++;
    ftpd_status("%d files received", *s->files_received);
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

    ftpd_reply(s, "150 Sending the file.");
    int data = ftpd_open_data(s);
    if (data < 0)
    {
        fclose(file);
        return;
    }

    bool ok = true;
    size_t read;
    while ((read = fread(s->buffer, 1, FTPD_BUFFER_SIZE, file)) > 0)
    {
        if (ftpd_send_all(data, s->buffer, read) != 0)
        {
            ok = false;
            break;
        }
    }
    fclose(file);
    close(data);
    ftpd_reply(s, ok ? "226 File sent." : "426 Connection lost.");
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

    char path[FTPD_PATH_MAX + 4];
    struct stat st;

    if (strcmp(line, "USER") == 0)
        ftpd_reply(s, "331 Any password works.");
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
        ftpd_reply(s, unlink(path) == 0 ? "250 Deleted." : "550 Could not delete the file.");
    }
    else if ((strcmp(line, "RMD") == 0 || strcmp(line, "XRMD") == 0) && arg != NULL)
    {
        ftpd_resolve_sd(s, arg, path);
        ftpd_reply(s, rmdir(path) == 0 ? "250 Removed." : "550 Could not remove the directory.");
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

    char address[64];
    snprintf(address, sizeof(address), "Wi-Fi sync is on: %s", inet_ntoa(ip));
    rrc_con_clear(true);
    rrc_con_print_text_centered(_RRC_ACTION_ROW, address);
    rrc_con_print_text_centered(_RRC_ACTION_ROW + 1, "In Wheel Wizard+, open Settings > Sync to Wii and enter this address.");
    rrc_con_print_text_centered(_RRC_ACTION_ROW + 2, "Press B when you are done.");
    ftpd_status("Waiting for Wheel Wizard+...");

    while (1)
    {
        rrc_shutdown_check();
        struct pad_state pad = rrc_pad_buttons();
        if (rrc_pad_b_pressed(pad) || rrc_pad_home_pressed(pad))
            break;

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
                ftpd_status("Wheel Wizard+ connected");
                ftpd_reply(&s, "220 Retro Rewind Wi-Fi sync ready.");
            }
        }
        else if (!ftpd_read_commands(&s))
        {
            ftpd_close_pasv(&s);
            close(s.control);
            s.control = -1;
            ftpd_status("%d files received. Waiting for Wheel Wizard+...", *files_received);
        }
    }

    ftpd_close_pasv(&s);
    if (s.control >= 0)
        close(s.control);
    close(server);
    free(s.buffer);
    return rrc_result_success;
}
