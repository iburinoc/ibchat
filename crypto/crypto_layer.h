#ifndef IBCHAT_CRYPTO_CRYPTO_LAYER_H
#define IBCHAT_CRYPTO_CRYPTO_LAYER_H

#include <stdint.h>

#include "../inet/protocol.h"

struct keyset {
	uint64_t nonce;
	uint8_t send_symm_key[32];
	uint8_t recv_symm_key[32];
	uint8_t send_hmac_key[32];
	uint8_t recv_hmac_key[32];
};

struct connection {
	struct con_handle handler;
	struct keyset     keys;
};

struct message *encrypt_message(struct keyset *keys, uint8_t *ptext, uint64_t plen);
int decrypt_message(struct keyset *keys, struct message *m, uint8_t *out);

#endif
