/*
 * hnc8
 * Copyright (C) 2019 hundinui
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "chip8_dbg_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include "log.h"
#include "chip8.h"
#include "file.h"

#define MAX_PACKET_SZ 512
#define MAX_TOKENS    16
#define MAX_STRARG_SZ 64

#define MSG_CURSOR      ">"
#define MSG_HELLO       "hnc8 debug server " HNC8_VERSION "\nType \"help\" for help or \"commands\" for a listing of commands.\n"
#define MSG_HELP        "TODO :)\n"
#define MSG_SHUTDOWN    "The server will shut down after client disconnect.\n"
#define MSG_OK          "OK\n"

#define MSG_ERR_FN              "Error executing function\n"
#define MSG_ERR_NO_FILE         "No file has been loaded.\nUse command \"load filename\" to load a program.\n"
#define MSG_ERR_ARGS_INVALID    "Invalid arguments for function\n"
#define MSG_ERR_ARGS_MISSING    "Too few arguments to call function\n"

#define tx_msg(msg) write(sockfd, msg, sizeof(msg))

typedef struct {
    uint8_t len;
    char *str;
} lex_t;

typedef struct {
    const char *cmd;
    const uint8_t cmd_len;
    const char *cmd_short;
    const uint8_t cmd_short_len;
    int (*fn)(int, lex_t *, int);
    const char *help_text;
} command_t;

ch8_t g_vm;
bool g_running = true;

uint16_t *g_file = NULL;
size_t g_file_sz = 0;

static void tx_printf(int sockfd, const char *fmt, ...)
{
    char buf[MAX_PACKET_SZ];
    size_t len = 0;
    va_list arg;

    va_start(arg, fmt);
    len = vsprintf(buf, fmt, arg);
    va_end(arg);

    write(sockfd, buf, len);
}

/* --- COMMANDS --- */

static int cmd_help(int sockfd, lex_t *argv, int argc)
{
    tx_msg(MSG_HELP);
    return 0;
}

static int cmd_shutdown(int sockfd, lex_t *argv, int argc)
{
    tx_msg(MSG_OK);
    tx_msg(MSG_SHUTDOWN);
    g_running = false;
    return 0;
}

static int cmd_load(int sockfd, lex_t *argv, int argc)
{
    if(argc < 2) {
        tx_msg(MSG_ERR_ARGS_MISSING);
        return -1;
    }

    if(g_file != NULL) {
        unload_file(g_file, g_file_sz);
        g_file = NULL;
        g_file_sz = 0;
    }

    if(load_file(argv[1].str, &g_file, &g_file_sz) != 0) {
        tx_printf(sockfd, "Could not load file \"%.*s\"\n", argv[1].len, argv[1].str);
        return -1;
    }

    tx_printf(sockfd, "Loaded \"%s\".\n", argv[1].str);

    tx_msg(MSG_OK);
    return 0;
}

static int cmd_break(int sockfd, lex_t *argv, int argc)
{
    if(g_file == NULL) {
        tx_msg(MSG_ERR_NO_FILE);
        return -1;
    }
    tx_msg(MSG_OK);
}

static int cmd_continue(int sockfd, lex_t *argv, int argc)
{
    if(g_file == NULL) {
        tx_msg(MSG_ERR_NO_FILE);
        return -1;
    }
    tx_msg(MSG_OK);
}

static int cmd_backtrace(int sockfd, lex_t *argv, int argc)
{
    if(g_file == NULL) {
        tx_msg(MSG_ERR_NO_FILE);
        return -1;
    }
    tx_msg(MSG_OK);
}

static int cmd_stepi(int sockfd, lex_t *argv, int argc)
{
    if(g_file == NULL) {
        tx_msg(MSG_ERR_NO_FILE);
        return -1;
    }
    tx_msg(MSG_OK);
}

static int cmd_examine(int sockfd, lex_t *argv, int argc)
{
    if(g_file == NULL) {
        tx_msg(MSG_ERR_NO_FILE);
        return -1;
    }
    if(argc < 2) {
        tx_msg(MSG_ERR_ARGS_MISSING);
        return -1;
    }

    char *endptr = NULL;
    int addr = strtol(argv[1].str, &endptr, 0);
    if(errno != 0 || endptr == argv[1].str) {
        return -1;
    }

    printf("examine %i\n", addr);

    tx_msg(MSG_OK);
    return 0;
}

static int cmd_registers(int sockfd, lex_t *argv, int argc)
{
    const char *v_reg_lut[] = {
        "v0", "v1", "v2", "v3", "v4",
        "v5", "v6", "v7", "v8", "v9",
        "v10", "v11", "v12", "v13", "v14",
        "v15"
    };
    const char *fmt_str = "%s\t0x%04x\t%i\n";
    const char *fmt_str_v = "%s\t0x%02x\t%i\n";

    if(argc == 1) { /* display registers */
        for(uint8_t i = 0; i < 16; ++i) {
            tx_printf(sockfd, fmt_str_v, v_reg_lut[i], g_vm.v[i], g_vm.v[i]);
        }
        tx_printf(sockfd, fmt_str, "i", g_vm.i, g_vm.i);
        tx_printf(sockfd, fmt_str, "pc", g_vm.pc, g_vm.pc);
        tx_printf(sockfd, fmt_str_v, "sp", g_vm.sp, g_vm.sp);
        tx_printf(sockfd, fmt_str_v, "dt", g_vm.tim_delay, g_vm.tim_delay);
        tx_printf(sockfd, fmt_str_v, "st", g_vm.tim_sound, g_vm.tim_sound);
        return 0;
    }

    if(argc >= 2) { /* display specific register */
        const char *name = NULL;
        const char *fmt = fmt_str;
        uint16_t val = 0;
        char *endptr = NULL;
        int vreg = 0;
        if(argc == 3) {
            val = strtol(argv[2].str, &endptr, 0);
            if(errno != 0 || endptr == argv[1].str) {
                tx_msg("Invalid value\n");
                return -1;
            }
        }

        switch(argv[1].str[0]) {
            case 'v':
            case 'V':
                endptr = NULL;
                vreg = strtol(argv[1].str + 1, &endptr, 0);
                if(errno != 0 || endptr == argv[1].str || vreg > 15 || vreg < 0) {
                    tx_msg("Invalid v register index\n");
                    return -1;
                }
                name = v_reg_lut[vreg];
                fmt = fmt_str_v;
                if(argc == 3) {
                    val = val > 0xFF ? 0xFF : val;
                    g_vm.v[vreg] = val;
                } else {
                    val = g_vm.v[vreg];
                }
                break;
            case 'i':
            case 'I':
                name = "i";
                if(argc == 3) {
                    g_vm.i = val;
                } else {
                    val = g_vm.i;
                }
                break;
            case 'p':
            case 'P':
                name = "pc";
                if(argc == 3) {
                    g_vm.pc = val;
                } else {
                    val = g_vm.pc;
                }
                break;
            case 's':
            case 'S':
                if(argv[1].str[1] == 'p') {
                    name = "sp";
                    if(argc == 3) {
                        val = val > 0xFF ? 0xFF : val;
                        g_vm.sp = val;
                    } else {
                        val = g_vm.sp;
                    }
                } else if(argv[1].str[1] == 't') {
                    name = "st";
                    if(argc == 3) {
                        val = val > 0xFF ? 0xFF : val;
                        g_vm.tim_sound = val;
                    } else {
                        val = g_vm.tim_sound;
                    }
                } else {
                    tx_msg("Invalid register name\n");
                    return -1;
                }
                break;
            case 'd':
            case 'D':
                name = "dt";
                if(argc == 3) {
                    val = val > 0xFF ? 0xFF : val;
                    g_vm.tim_delay = val;
                } else {
                    val = g_vm.tim_delay;
                }
                break;
            default:
                tx_msg(MSG_ERR_ARGS_INVALID);
                return -1;
        }

        tx_msg(MSG_OK);
        if(argc == 3) {
            tx_printf(sockfd, "Set %s to 0x%04x (%u)\n", name, val, val);
        } else {
            tx_printf(sockfd, fmt, name, val, val);
        }

        return 0;
    }

    return -1;
}

/*
 * only one prototyped because we need to read the list we are pointing
 * to this from :)
 */
static int cmd_commands(int sockfd, lex_t *argv, int argc);

#define DEF_CMD(cmd, shortcmd, fn, help_text) { cmd, sizeof(cmd) - 1, shortcmd, sizeof(shortcmd) - 1, fn, help_text }

static const command_t commands[] = {
    /*       NAME        SHORT     FUNC          HELP*/
    DEF_CMD("help",       "h",    cmd_help,     "- Display help message"),
    DEF_CMD("shutdown",   NULL,   cmd_shutdown, "- Shut down the server"),
    DEF_CMD("load",       "l",    cmd_load,     "filename - Load ROM into VM"),
    DEF_CMD("break",      "b",    cmd_break,    "[offset] - Add a breakpoint"),
    DEF_CMD("continue",   "c",    cmd_continue, "- Continue execution until breakpoint"),
    DEF_CMD("backtrace",  "bt",   cmd_backtrace, "- Display the stack trace"),
    DEF_CMD("stepi",      "si",   cmd_stepi,    "[count] - Step forward"),
    DEF_CMD("examine",    "x",    cmd_examine,  "address [count] - Examine memory"),
    DEF_CMD("commands",   NULL,   cmd_commands, "- Display this info about commands"),
    DEF_CMD("registers",  "r",    cmd_registers, "[register] [value] - Display and edit VM registers")
};
#define commands_count (sizeof(commands) / sizeof(commands[0]))

static int cmd_commands(int sockfd, lex_t *argv, int argc)
{
    tx_printf(sockfd, "Available commands:\n");
    for(uint8_t i = 0; i < commands_count; ++i) {
        tx_printf(sockfd, "  %s %s\n", commands[i].cmd, commands[i].help_text);
    }
}

static inline void decode_msg(int sockfd, char *msg, size_t len)
{
    uint8_t lex_i = 0;
    lex_t lex[MAX_TOKENS] = { 0 };

    /* clean the newline */
    if(msg[len - 1] == '\n') {
        msg[len - 1] = '\0';
        len -= 1;
    }

    /* split the message we got into lexemes */
    char *start = msg;
    char *end;

    while((end = memchr(start, ' ', len)) != NULL && end < msg + len) {
        uint8_t len = end - start;
        if(len == 0) {
            start = end + 1;
            continue;
        }
        lex[lex_i].len = len;
        lex[lex_i].str = start;
        lex_i += 1;
        start = end + 1;
    }
    lex[lex_i].len = (msg + len) - start;
    lex[lex_i].str = start;
    lex_i += 1;

    for(int i = 0; i < lex_i; ++i) {
        printf("%i - %.*s\n", lex[i].len, lex[i].len, lex[i].str);
    }

    const lex_t *net_cmd = &lex[0];
    for(uint8_t i = 0; i < commands_count; ++i) {
        const command_t *cmd = &commands[i];
        bool cmd_match = net_cmd->len == cmd->cmd_len;
        bool short_match = net_cmd->len == cmd->cmd_short_len;
        if(
        (cmd_match && strncmp(net_cmd->str, cmd->cmd, cmd->cmd_len) == 0) ||
        (short_match && strncmp(net_cmd->str, cmd->cmd_short, cmd->cmd_short_len) == 0)
        ) {
            if((*cmd->fn)(sockfd, lex, lex_i) < 0) {
                tx_msg(MSG_ERR_FN);
            }
            return;
        }
    }

    tx_printf(sockfd, "Unknown command \"%s\"\n", net_cmd->str);
}

static void client_handler(int sockfd)
{
    char buf[MAX_PACKET_SZ];

    tx_msg(MSG_HELLO);

    for(;;) {
        tx_msg(MSG_CURSOR);

        memset(buf, 0, MAX_PACKET_SZ);

        size_t sz = read(sockfd, buf, MAX_PACKET_SZ);
        if(sz == 0) break;

        printf("Got: %s", buf);

        decode_msg(sockfd, buf, sz);
    }
}

void dbg_server_loop(uint16_t port)
{
    int sockfd;
    int connfd;
    struct sockaddr_in servaddr, cli;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd == -1) {
        LOG_ERROR("Error creating socket\n");
        return;
    }
    memset(&servaddr, 0, sizeof(servaddr));

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if((bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr))) != 0) {
        LOG_ERROR("Error binding to socket\n");
        return;
    }

    if((listen(sockfd, 5)) != 0) {
        LOG_ERROR("Error listening on socket\n");
        return;
    }

    LOG("Debug server listening on localhost:%i\n", port);

    unsigned int len = sizeof(cli);

    /* reset emu */
    ch8_init(&g_vm);

    while(g_running) {
        connfd = accept(sockfd, (struct sockaddr *)&cli, &len);
        if(connfd < 0) {
            LOG_ERROR("Error accepting client\n");
            return;
        } else {
            LOG("Client connected\n");
        }

        client_handler(connfd);

        close(connfd);

        LOG("Client disconnected\n");
    }

    LOG("Shutting down\n");

    close(sockfd);

    return;
}
