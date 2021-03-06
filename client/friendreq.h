#ifndef CLIENT_FRIENDREQ_H
#define CLIENT_FRIENDREQ_H

#include "../inet/message.h"

#include "login_server.h"
#include "account.h"

struct friendreq {
	uint64_t u_len;
	uint64_t k_len;

	char *uname;
	uint8_t *pkey;
};

struct message *pkey_resp;

int parse_friendreq(uint8_t *sender, uint8_t *payload, uint64_t p_len);
int parse_friendreq_response(uint8_t *sender, uint8_t *payload, uint64_t p_len);

int send_friendreq(struct server_connection *sc, struct account *acc);
void free_friendreq(struct friendreq *freq);

int friendreq_response(struct friendreq *freq);
int friendreq_send_response(struct friendreq *freq);

#endif

