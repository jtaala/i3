/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3bar - an xcb-based status- and ws-bar for i3
 * © 2010 Axel Wagner and contributors (see also: LICENSE)
 *
 * ipc.c: Communicating with i3
 *
 */
#include "common.h"

#include <errno.h>
#include <ev.h>
#include <i3/ipc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef I3_ASAN_ENABLED
#include <sanitizer/lsan_interface.h>
#endif

ev_io *i3_connection;

const char *sock_path;

typedef void (*handler_t)(char *);

/*
 * Called, when we get a reply to a command from i3.
 * Since i3 does not give us much feedback on commands, we do not much
 *
 */
static void got_command_reply(char *reply) {
    /* TODO: Error handling for command replies */
}

/*
 * Called, when we get a reply with workspaces data
 *
 */
static void got_workspace_reply(char *reply) {
    DLOG("Got workspace data!\n");
    parse_workspaces_json(reply);
    draw_bars(false);
}

/*
 * Called, when we get a reply for a subscription.
 * Since i3 does not give us much feedback on commands, we do not much
 *
 */
static void got_subscribe_reply(char *reply) {
    DLOG("Got subscribe reply: %s\n", reply);
    /* TODO: Error handling for subscribe commands */
}

/*
 * Called, when we get a reply with outputs data
 *
 */
static void got_output_reply(char *reply) {
    DLOG("Clearing old output configuration...\n");
    free_outputs();

    DLOG("Parsing outputs JSON...\n");
    parse_outputs_json(reply);
    DLOG("Reconfiguring windows...\n");
    reconfig_windows(false);

    i3_output *o_walk;
    SLIST_FOREACH (o_walk, outputs, slist) {
        kick_tray_clients(o_walk);
    }

    if (!config.disable_ws) {
        i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_WORKSPACES, NULL);
    }

    draw_bars(false);
}

/*
 * Called when we get the configuration for our bar instance
 *
 */
static void got_bar_config(char *reply) {
    if (!config.bar_id) {
        DLOG("Received bar list \"%s\"\n", reply);
        parse_get_first_i3bar_config(reply);

        if (!config.bar_id) {
            ELOG("No bar configuration found, please configure a bar block in your i3 config file.\n");
            exit(EXIT_FAILURE);
        }

        LOG("Using first bar config: %s. Use --bar_id to manually select a different bar configuration.\n", config.bar_id);
        i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_BAR_CONFIG, config.bar_id);
        return;
    }

    DLOG("Received bar config \"%s\"\n", reply);
    /* We initiate the main function by requesting infos about the outputs and
     * workspaces. Everything else (creating the bars, showing the right workspace-
     * buttons and more) is taken care of by the event-drivenness of the code */
    i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_OUTPUTS, NULL);

    free_colors(&(config.colors));
    parse_config_json(reply);

    /* Now we can actually use 'config', so let's subscribe to the appropriate
     * events and request the workspaces if necessary. */
    subscribe_events();
    if (!config.disable_ws)
        i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_WORKSPACES, NULL);

    /* Initialize the rest of XCB */
    init_xcb_late(config.fontname);

    /* Resolve color strings to colorpixels and save them, then free the strings. */
    init_colors(&(config.colors));

    start_child(config.command);
}

/* Data structure to easily call the reply handlers later */
handler_t reply_handlers[] = {
    &got_command_reply,   /* I3_IPC_REPLY_TYPE_COMMAND */
    &got_workspace_reply, /* I3_IPC_REPLY_TYPE_WORKSPACES */
    &got_subscribe_reply, /* I3_IPC_REPLY_TYPE_SUBSCRIBE */
    &got_output_reply,    /* I3_IPC_REPLY_TYPE_OUTPUTS */
    NULL,                 /* I3_IPC_REPLY_TYPE_TREE */
    NULL,                 /* I3_IPC_REPLY_TYPE_MARKS */
    &got_bar_config,      /* I3_IPC_REPLY_TYPE_BAR_CONFIG */
    NULL,                 /* I3_IPC_REPLY_TYPE_VERSION */
    NULL,                 /* I3_IPC_REPLY_TYPE_BINDING_MODES */
    NULL,                 /* I3_IPC_REPLY_TYPE_CONFIG */
    NULL,                 /* I3_IPC_REPLY_TYPE_TICK */
    NULL,                 /* I3_IPC_REPLY_TYPE_SYNC */
};

/*
 * Called, when a workspace event arrives (i.e. the user changed the workspace)
 *
 */
static void got_workspace_event(char *event) {
    DLOG("Got workspace event!\n");
    i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_WORKSPACES, NULL);
}

/*
 * Called, when an output event arrives (i.e. the screen configuration changed)
 *
 */
static void got_output_event(char *event) {
    DLOG("Got output event!\n");
    i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_OUTPUTS, NULL);
}

/*
 * Called, when a mode event arrives (i3 changed binding mode).
 *
 */
static void got_mode_event(char *event) {
    DLOG("Got mode event!\n");
    parse_mode_json(event);
    draw_bars(false);
}

static bool strings_differ(char *a, char *b) {
    const bool a_null = (a == NULL);
    const bool b_null = (b == NULL);
    if (a_null != b_null) {
        return true;
    }
    if (a_null && b_null) {
        return false;
    }
    return strcmp(a, b) != 0;
}

/*
 * Called, when a barconfig_update event arrives (i.e. i3 changed the bar hidden_state or mode)
 *
 */
static void got_bar_config_update(char *event) {
    /* check whether this affect this bar instance by checking the bar_id */
    char *expected_id;
    sasprintf(&expected_id, "\"id\":\"%s\"", config.bar_id);
    char *found_id = strstr(event, expected_id);
    FREE(expected_id);
    if (found_id == NULL)
        return;

    /* reconfigure the bar based on the current outputs */
    i3_send_msg(I3_IPC_MESSAGE_TYPE_GET_OUTPUTS, NULL);

    free_colors(&(config.colors));

    /* update the configuration with the received settings */
    DLOG("Received bar config update \"%s\"\n", event);

    char *old_command = config.command;
    config.command = NULL;
    bar_display_mode_t old_mode = config.hide_on_modifier;

    parse_config_json(event);
    if (old_mode != config.hide_on_modifier) {
        reconfig_windows(true);
    }

    /* update fonts and colors */
    init_xcb_late(config.fontname);
    init_colors(&(config.colors));

    /* restart status command process */
    if (strings_differ(old_command, config.command)) {
        kill_child();
        clear_statusline(&statusline_head, true);
        start_child(config.command);
    }
    free(old_command);

    draw_bars(false);
}

/* Data structure to easily call the event handlers later */
handler_t event_handlers[] = {
    &got_workspace_event,
    &got_output_event,
    &got_mode_event,
    NULL,
    &got_bar_config_update,
};

/*
 * Called, when we get a message from i3
 *
 */
static void got_data(struct ev_loop *loop, ev_io *watcher, int events) {
    DLOG("Got data!\n");
    int fd = watcher->fd;

    /* First we only read the header, because we know its length */
    uint32_t header_len = strlen(I3_IPC_MAGIC) + sizeof(uint32_t) * 2;
    char *header = smalloc(header_len);

    /* We first parse the fixed-length IPC header, to know, how much data
     * we have to expect */
    uint32_t rec = 0;
    while (rec < header_len) {
        int n = read(fd, header + rec, header_len - rec);
        if (n == -1) {
            ELOG("read() failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (n == 0) {
            /* EOF received. Since i3 will restart i3bar instances as appropriate,
             * we exit here. */
            DLOG("EOF received, exiting...\n");
#ifdef I3_ASAN_ENABLED
            __lsan_do_leak_check();
#endif
            clean_xcb();
            exit(EXIT_SUCCESS);
        }
        rec += n;
    }

    if (strncmp(header, I3_IPC_MAGIC, strlen(I3_IPC_MAGIC))) {
        ELOG("Wrong magic code: %.*s\n Expected: %s\n",
             (int)strlen(I3_IPC_MAGIC),
             header,
             I3_IPC_MAGIC);
        exit(EXIT_FAILURE);
    }

    char *walk = header + strlen(I3_IPC_MAGIC);
    uint32_t size;
    memcpy(&size, (uint32_t *)walk, sizeof(uint32_t));
    walk += sizeof(uint32_t);
    uint32_t type;
    memcpy(&type, (uint32_t *)walk, sizeof(uint32_t));

    /* Now that we know, what to expect, we can start read()ing the rest
     * of the message */
    char *buffer = smalloc(size + 1);
    rec = 0;

    while (rec < size) {
        int n = read(fd, buffer + rec, size - rec);
        if (n == -1) {
            ELOG("read() failed: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        if (n == 0) {
            ELOG("Nothing to read!\n");
            exit(EXIT_FAILURE);
        }
        rec += n;
    }
    buffer[size] = '\0';

    /* And call the callback (indexed by the type) */
    if (type & (1UL << 31)) {
        type ^= 1UL << 31;
        event_handlers[type](buffer);
    } else {
        if (reply_handlers[type])
            reply_handlers[type](buffer);
    }

    FREE(header);
    FREE(buffer);
}

/*
 * Sends a message to i3.
 * type must be a valid I3_IPC_MESSAGE_TYPE (see i3/ipc.h for further information)
 *
 */
int i3_send_msg(uint32_t type, const char *payload) {
    uint32_t len = 0;
    if (payload != NULL) {
        len = strlen(payload);
    }

    /* We are a wellbehaved client and send a proper header first */
    uint32_t to_write = strlen(I3_IPC_MAGIC) + sizeof(uint32_t) * 2 + len;
    /* TODO: I'm not entirely sure if this buffer really has to contain more
     * than the pure header (why not just write() the payload from *payload?),
     * but we leave it for now */
    char *buffer = smalloc(to_write);
    char *walk = buffer;

    memcpy(buffer, I3_IPC_MAGIC, strlen(I3_IPC_MAGIC));
    walk += strlen(I3_IPC_MAGIC);
    memcpy(walk, &len, sizeof(uint32_t));
    walk += sizeof(uint32_t);
    memcpy(walk, &type, sizeof(uint32_t));
    walk += sizeof(uint32_t);

    if (payload != NULL) {
        memcpy(walk, payload, len);
    }

    swrite(i3_connection->fd, buffer, to_write);

    FREE(buffer);

    return 1;
}

/*
 * Initiate a connection to i3.
 * socket_path must be a valid path to the ipc_socket of i3
 *
 */
void init_connection(const char *socket_path) {
    sock_path = socket_path;
    int sockfd = ipc_connect(socket_path);
    i3_connection = smalloc(sizeof(ev_io));
    ev_io_init(i3_connection, &got_data, sockfd, EV_READ);
    ev_io_start(main_loop, i3_connection);
}

/*
 * Destroy the connection to i3.
 */
void destroy_connection(void) {
    close(i3_connection->fd);
    ev_io_stop(main_loop, i3_connection);
}

/*
 * Subscribe to all the i3-events, we need
 *
 */
void subscribe_events(void) {
    if (config.disable_ws) {
        i3_send_msg(I3_IPC_MESSAGE_TYPE_SUBSCRIBE, "[ \"output\", \"mode\", \"barconfig_update\" ]");
    } else {
        i3_send_msg(I3_IPC_MESSAGE_TYPE_SUBSCRIBE, "[ \"workspace\", \"output\", \"mode\", \"barconfig_update\" ]");
    }
}
