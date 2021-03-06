#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ibcrypt/sha256.h>
#include <ibcrypt/rsa_util.h>

#include <libibur/util.h>
#include <libibur/endian.h>

#include "../util/lock.h"
#include "../util/log.h"
#include "../util/line_prompt.h"

#include "cli.h"
#include "account.h"
#include "profile.h"
#include "connect_server.h"
#include "ibchat_client.h"
#include "login_server.h"
#include "friends.h"
#include "notifications.h"
#include "bg_manager.h"
#include "conversation.h"
#include "friendreq.h"

struct profile prof;
struct account *acc;
struct server_connection sc;

struct notif *notifs;

struct lock lock = LOCK_STRUCT_INIT;

/* 0: default mode, 1: in conversation, 2: in friendreq, -1: stop */
int mode;

int stop;

static char keysig[65];

int init(int argc, char **argv);
int deinit();
int select_profile();
int handle_user();

int main(int argc, char **argv) {
	/* initialize variables, etc. */
	if(init(argc, argv) != 0) {
		fprintf(stderr, "failed to initialize ibchat client\n");
		return 1;
	}

	/* log the user in */
	if(login_profile(NULL, &prof) != 0) {
		ERR("failed to login");
		return 1;
	}

	if(select_profile() != 0) {
		return 1;
	}

	if(handle_user() != 0) {
		return 1;
	}

	/* TODO: disconnect and clean up */

	deinit();

	return 0;
}

int select_profile() {
	int ret;
	if((ret = pick_account(&prof, &acc)) < 0) {
		ERR("failed to pick account");
		return 1;
	}
	if(ret == 1) {
		printf("there are no accounts to use, exiting.\n");
		return 1;
	}

	if(ret == 0x55) { /* register a new account */
		acc = malloc(sizeof(*acc));
		if(acc == NULL) {
			ERR("failed to allocate memory");
			return 1;
		}
		if(create_account(acc, &sc) != 0) {
			ERR("failed to register account");
			return 1;
		}

		/* we should write the user file again */
		if(add_account(&prof, acc) != 0) {
			ERR("failed to add account to user file");
			return 1;
		}
	} else { /* login an existing account */
		if(login_account(acc, &sc) != 0) {
			ERR("failed to login account");
			return 1;
		}

		if(check_userfile(&prof) != 0) {
			return 1;
		}

		/* load the friend file */
		if(read_friendfile(acc) != 0) {
			ERR("failed to read friend file");
			return 1;
		}

		/* load the notiffile */
		if(read_notiffile(acc, &notifs) != 0) {
			ERR("failed to read notiffs");
			return 1;
		}
	}

	return 0;
}

int handler_init();
int handler_select();

int handle_user() {
	if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		ERR("ibchat must be run in a tty");
		return 1;
	}

	if(handler_init() != 0) {
		return 1;
	}

	while(handler_status(sc.ch) == 0 && stop == 0) {
		/* print status and options */
		handler_select();
	}
	if(handler_status(sc.ch) != 0) {
		LOG("server disconnected");
		printf("server disconnected\n");
	}
	set_mode(-1);
	LOG("exiting");
	printf("exiting\n");
	return 0;
}

int handler_init() {
	stop = 0;
	mode = 0;
	{
		uint64_t keylen = rsa_pubkey_bufsize(decbe64(acc->key_bin));
		uint8_t *pkey = malloc(keylen);
		if(pkey == NULL) {
			ERR("failed to allocate memory");
			return 1;
		}
		if(rsa_wire_prikey2pubkey(acc->key_bin, acc->k_len,
			pkey, keylen) != 0) {
			ERR("failed to convert key to public");
			return 1;
		}
		uint8_t hash[32];
		sha256(pkey, keylen, hash);
		to_hex(hash, 32, keysig);

		free(pkey);
	}

	/* we should spawn the manager thread here */
	if(start_bg_thread(&sc) != 0) {
		return 1;
	}

	return 0;
}

void wait_for_event();

int handler_select() {
	if(get_mode() == 0xff) {
		printf("new notification\n");
		set_mode(0);
	}
	int notiflen = notiflist_len(notifs);

	printf("user: %s\n"
		"fingerprint: %s\n",
		acc->uname,
		keysig);

	printf("%1d: message friend\n", 1);
	printf("%1d: view %d notification(s)\n", 2, notiflen);
	printf("%1d: add friend\n", 3);
	printf("%1d: exit\n", 0);

	uint64_t sel;
	printf("selection: ");
	do {
		/* wait for input or system to exit */
		fflush(stdout);
		wait_for_event();
		if(get_mode() != 0) {
			return 0;
		}

		sel = num_prompt_no_retry(NULL, 0, 3);
		if(sel == ULLONG_MAX - 1) {
			printf("invalid response, try again: ");
		}
	} while(sel == ULLONG_MAX - 1);

	if(sel == ULLONG_MAX) {
		return 0;
	}

	switch(sel) {
	case 0:
		stop = 1; break;
	case 1:
		if(select_conversation(acc) != 0) {
			stop = 1;
		}
		break;
	case 2:
		if(view_notifs(acc) != 0) {
			stop = 1;
		}
		break;
	case 3:
		if(send_friendreq(&sc, acc) != 0) {
			stop = 1;
		}
		break;
	default:
		ERR("error occurred in selection");
		stop = 1;
		break;
	}

	return 0;
}

void wait_for_event() {
	while(get_mode() == 0) {
		fd_set fds;
		FD_SET(STDIN_FILENO, &fds);
		struct timeval wait;
		wait.tv_sec = 0;
		wait.tv_usec = 100000L;

		select(STDIN_FILENO + 1, &fds, NULL, NULL, &wait);
		if(FD_ISSET(STDIN_FILENO, &fds)) {
			break;
		}
	}
}

void set_mode(int v) {
	acquire_writelock(&lock);
	set_mode_no_lock(v);
	release_writelock(&lock);
}

void set_mode_no_lock(int v) {
	mode = v;
}

int get_mode() {
	acquire_readlock(&lock);
	int v = mode;
	release_readlock(&lock);
	return v;
}

int get_mode_no_lock() {
	return mode;
}

