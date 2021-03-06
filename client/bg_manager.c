#include <stdio.h>

#include <sys/time.h>

#include <ibcrypt/chacha.h>
#include <ibcrypt/rsa.h>
#include <ibcrypt/rsa_util.h>
#include <ibcrypt/sha256.h>
#include <ibcrypt/zfree.h>

#include <libibur/util.h>
#include <libibur/endian.h>

#include "../util/log.h"

#include "bg_manager.h"
#include "login_server.h"
#include "cli.h"
#include "friendreq.h"
#include "conversation.h"

pthread_t bg_manager;

pthread_mutex_t bg_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t bg_wait  = PTHREAD_COND_INITIALIZER;

pthread_mutex_t net_lock = PTHREAD_MUTEX_INITIALIZER;

#define WAITTIME ((uint64_t) 10000ULL)

int acquire_netlock() {
	pthread_mutex_lock(&net_lock);
	int s = get_mode();
	if(s == -1) {
		pthread_mutex_unlock(&net_lock);
		return 1;
	}
	return 0;
}

void release_netlock() {
	pthread_mutex_unlock(&net_lock);
}

int add_umessage(struct message *m) {
	int ret = 0;

	uint8_t *sender = &m->message[0x01];
	uint8_t *payload = &m->message[0x29];
	uint8_t type = m->message[0x29];

	char s_hex[65];
	to_hex(sender, 0x20, s_hex);

	LOG("message from %s of length %llu", s_hex, m->length);

	uint64_t p_len = decbe64(&m->message[0x21]);

	/* check the lengths */
	if(p_len + 0x29 != m->length) {
		/* server lying is a crashing error */
		return -1;
	}

	switch(type) {
	case 0:
		/* typical message from friend */
		ret = parse_conv_message(sender, payload, p_len);
		break;
	case 1:
		/* friend request */
		ret = parse_friendreq(sender, payload, p_len);
		break;
	case 2:
		ret = parse_friendreq_response(sender, payload, p_len);
		break;
	}

	return ret;
}

int add_pkeyresp(struct message *m) {
	pthread_mutex_lock(&bg_lock);
	if(get_mode() != 2) {
		pthread_mutex_unlock(&bg_lock);
		return -1;
	}

	pkey_resp = m;
	pthread_cond_broadcast(&bg_wait);
	pthread_mutex_unlock(&bg_lock);
	return 0;
}

int add_unotfound(struct message *m) {
	pthread_mutex_lock(&bg_lock);
	if(get_mode() != 2) {
		pthread_mutex_unlock(&bg_lock);
		return -1;
	}

	pkey_resp = m;
	pthread_cond_broadcast(&bg_wait);
	pthread_mutex_unlock(&bg_lock);
	return 0;
}

void *background_thread(void *_arg) {
	struct server_connection *sc = (struct server_connection *) _arg;

	while(get_mode() != -1) {
		if(acquire_netlock() != 0) break;
		struct message *m = recv_message(sc->ch, &sc->keys, WAITTIME);
		if(handler_status(sc->ch) != 0) {
			set_mode(-1);
		}
		if(m == NULL) {
			release_netlock();
			continue;
		}

		int ret = 0;
		switch(m->message[0]) {
		case 0:
			ret = add_umessage(m);
			break;
		case 1:
			ret = add_pkeyresp(m);
			break;
		case 0xff:
			ret = add_unotfound(m);
			break;
		}
		if(ret != 0) {
			break;
		}
		release_netlock();
	}
	release_netlock();

	ERR("background thread crashed");
	acquire_writelock(&lock);
	stop = 1;
	release_writelock(&lock);

	set_mode(-1);
	pthread_cond_broadcast(&bg_wait);

	return NULL;
}

int start_bg_thread(struct server_connection *sc) {
	if(pthread_create(&bg_manager, NULL, background_thread, sc) != 0) {
		ERR("failed to start background thread");
		return -1;
	}

	return 0;
}

