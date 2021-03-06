#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <ibcrypt/chacha.h>
#include <ibcrypt/rand.h>
#include <ibcrypt/rsa_util.h>
#include <ibcrypt/rsa.h>
#include <ibcrypt/sha256.h>
#include <ibcrypt/zfree.h>

#include <libibur/util.h>
#include <libibur/endian.h>

#include "../util/line_prompt.h"
#include "../util/lock.h"
#include "../util/log.h"

#include "cli.h"
#include "friendreq.h"
#include "uname.h"
#include "bg_manager.h"
#include "uname.h"

static int send_pkey_req(struct server_connection *sc, uint8_t target[32]);
static int send_friendreq_message(struct server_connection *sc,
	struct account *acc, uint8_t target[32],
	uint8_t *pkey, uint64_t pkeylen);
static int verify_pkey(char *target, uint8_t *pkey_bin, uint64_t pkey_len);

int send_friendreq(struct server_connection *sc, struct account *acc) {
	int ret = 0;
	pkey_resp = NULL;

	/* prompt for a username */
	char *uname = getusername("friend name", stdout);
	if(uname == NULL) {
		ERR("failed to get friend name");
		return -1;
	}

	/* get the uid */
	uint8_t uid[32];
	gen_uid(uname, uid);

	if(send_pkey_req(sc, uid) != 0) {
		goto err;
	}

	set_mode(2);

	pthread_mutex_lock(&bg_lock);
	while(pkey_resp == NULL && get_mode() == 2) {
		pthread_cond_wait(&bg_wait, &bg_lock);
	}
	if(get_mode() == -1) {
		goto err;
	}
	set_mode(0);
	pthread_mutex_unlock(&bg_lock);

	if(pkey_resp->length < 0x21) {
		ERR("server returned invalid message");
		goto err;
	}

	if(pkey_resp->message[0] == 0xff) {
		printf("the server could not find the user you specified\n");
		goto end;
	}

	if(pkey_resp->length < 0x29) {
		ERR("server returned invalid message");
		goto err;
	}

	if(memcmp(&pkey_resp->message[1], uid, 32) != 0) {
		ERR("server returned public key for wrong user");
		goto err;
	}

	uint64_t pkeysize = rsa_pubkey_bufsize(decbe64(
		&pkey_resp->message[0x21]));

	if(pkeysize + 0x21 != pkey_resp->length) {
		ERR("server returned invalid message");
	}

	uint8_t *pkey_bin = &pkey_resp->message[0x21];

	/* verify the public key */
	int ver;
	if((ver = verify_pkey(uname, pkey_bin, pkeysize)) != 0) {
		if(ver == -1) {
			goto err;
		} else {
			goto end;
		}
	}

	if(send_friendreq_message(sc, acc, uid, pkey_bin, pkeysize) != 0) {
		goto err;
	}

	LOG("friend request send to %s", uname);

	return 0;

	goto end;
err:
	ret = -1;
end:
	if(pkey_resp) free_message(pkey_resp);
	free(uname);
	return ret;
}

static int verify_pkey(char *target, uint8_t *pkey_bin, uint64_t pkey_len) {
	uint8_t hash[32];
	sha256(pkey_bin, pkey_len, hash);

	char hex[65];
	to_hex(hash, 32, hex);

	printf("please verify %s's public key fingerprint:\n"
		"%s\n"
		"does this match external verification? [y/n] ",
		target, hex);

	int ans = yn_prompt();
	if(ans == -1) {
		return -1;
	}
	if(ans == 0) {
		printf("friend request canceled\n");
		return 1;
	}

	return 0;
}

static int send_pkey_req(struct server_connection *sc, uint8_t target[32]) {
	int ret = -1;
	if(acquire_netlock() != 0) return -1;
	uint8_t *message = malloc(1 + 0x20);
	if(message == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}

	message[0] = 1;
	memcpy(&message[1], target, 0x20);

	if(send_message(sc->ch, &sc->keys, message, 0x21) != 0) {
		ERR("failed to send pkey request");
		goto err;
	}

	free(message);
	ret = 0;
err:
	release_netlock();
	return ret;
}

static int send_rsa_message(struct server_connection *sc, uint8_t m_type,
	struct account *acc, uint8_t target[32], uint8_t *pkey,
	uint64_t pkeylen, uint8_t *ex_data, uint64_t ed_len) {

	int ret = -1;

	uint64_t encblen = (decbe64(pkey) + 7) / 8;
	uint64_t siglen = (decbe64(acc->key_bin) + 7) / 8;

	if(!ex_data) ed_len = 0;

	uint64_t reqlen = 0;
	reqlen += 0x29; /* message type and destination/length */
	reqlen += 0x11; /* friend message prefix */
	reqlen += encblen; /* encrypted block */
	reqlen += 0x10; /* payload prefix */
	reqlen += acc->u_len; /* username */
	reqlen += rsa_pubkey_bufsize(decbe64(acc->key_bin)); /* pkey */
	reqlen += ed_len; /* extra data */
	reqlen += 0x20; /* MAC */
	reqlen += siglen; /* sig */
	uint8_t *reqbody = malloc(reqlen);
	if(reqbody == NULL) {
		ERR("failed to allocate memory");
		return -1;
	}

	RSA_KEY sig_key;
	memset(&sig_key, 0, sizeof(sig_key));
	RSA_PUBLIC_KEY rec_key;
	memset(&rec_key, 0, sizeof(rec_key));
	uint8_t *my_key = NULL;
	uint64_t my_keylen = rsa_pubkey_bufsize(decbe64(acc->key_bin));
	uint8_t keys[64];
	uint8_t *payload = NULL;
	uint64_t payloadlen = 0x10 + acc->u_len + my_keylen + ed_len;

	uint8_t *ptr = reqbody;

	ptr[0] = 0; ptr++;
	memcpy(ptr, target, 0x20); ptr += 0x20;
	encbe64(reqlen - 0x29, ptr); ptr += 8;

	ptr[0] = m_type; ptr++;
	encbe64(encblen, ptr); ptr += 8;
	encbe64(payloadlen + 0x20, ptr); ptr += 8;

	if(cs_rand(keys, 64) != 0) {
		ERR("failed to generate encryption keys");
		goto err;
	}

	if(rsa_wire2pubkey(pkey, pkeylen, &rec_key) != 0) {
		ERR("failed to expand public key");
		goto err;
	}

	if(rsa_oaep_encrypt(&rec_key, keys, 64, ptr, encblen) != 0) {
		ERR("failed to encrypt keys");
		goto err;
	}
	ptr += encblen;

	my_key = malloc(my_keylen);
	if((my_key = malloc(my_keylen)) == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}
	if(rsa_wire_prikey2pubkey(acc->key_bin, acc->k_len,
		my_key, my_keylen) != 0) {
		ERR("failed to convert private key");
		goto err;
	}

	payload = ptr;

	encbe64(acc->u_len, ptr); ptr += 0x08;
	encbe64(my_keylen, ptr); ptr += 0x08;
	memcpy(ptr, acc->uname, acc->u_len); ptr += acc->u_len;
	memcpy(ptr, my_key, my_keylen); ptr += my_keylen;
	if(ex_data) memcpy(ptr, ex_data, ed_len); ptr += ed_len;

	chacha_enc(&keys[0], 32, 0, payload, payload, payloadlen);
	hmac_sha256(&keys[32], 32, payload, payloadlen, &payload[payloadlen]);
	ptr += 32;

	if(rsa_wire2prikey(acc->key_bin, acc->k_len, &sig_key) != 0) {
		ERR("failed to expand private key");
		goto err;
	}
	if(rsa_pss_sign(&sig_key, &reqbody[0x29], reqlen - siglen - 0x29,
		ptr, siglen) != 0) {
		ERR("failed to sign");
		goto err;
	}
	ptr += siglen;

	if(ptr - reqbody != reqlen) {
		ERR("invalid payload length");
		goto err;
	}

	if(acquire_netlock() != 0) goto err;
	if(send_message(sc->ch, &sc->keys, reqbody, reqlen) != 0) {
		ERR("failed to send message");
		goto err;
	}
	release_netlock();

	ret = 0;

err:;
	zfree(reqbody, reqlen);
	if(my_key) zfree(my_key, my_keylen);
	memsets(keys, 0, sizeof(keys));
	rsa_free_pubkey(&rec_key);
	rsa_free_prikey(&sig_key);

	return ret;
}

static int parse_rsa_message(struct account *acc,
	uint8_t *sender, uint8_t *payload, uint64_t p_len,
	char **u_data, uint64_t *u_len,
	uint8_t **k_data, uint64_t *k_len,
	uint8_t *ex_data, uint64_t ed_len) {

	int ret = -1, inv = -1;

	char s_hex[65];
	to_hex(sender, 0x20, s_hex);

	RSA_KEY rkey;
	memset(&rkey, 0, sizeof(rkey));

	RSA_PUBLIC_KEY pkey;
	memset(&pkey, 0, sizeof(pkey));

	uint8_t keys[64];
	uint8_t *symm = &keys[ 0];
	uint8_t *hmac = &keys[32];
	uint8_t mac[32];

	/* keyblock and datablock */
	uint64_t kb_len = decbe64(&payload[1]);
	uint64_t db_len = decbe64(&payload[9]);

	if(kb_len + db_len + 17 >= p_len) {
		goto inv;
	}

	/* expand the private key */
	if(rsa_wire2prikey(acc->key_bin, acc->k_len, &rkey) != 0) {
		ERR("failed to expand private key");
		goto err;
	}

	/* decrypt the message */
	if(rsa_oaep_decrypt(&rkey, &payload[0x11], kb_len, keys, 64) != 0) {
		goto inv;
	}

	uint8_t *data = &payload[17 + kb_len];

	hmac_sha256(hmac, 32, data, db_len - 32, mac);
	if(memcmp_ct(mac, &data[db_len-32], 32) != 0) {
		goto inv;
	}

	chacha_dec(symm, 32, 0, data, data, db_len - 32);

	*u_len = decbe64(&data[0]);
	*k_len = decbe64(&data[8]);
	*u_data = malloc(*u_len + 1);
	*k_data = malloc(*k_len);

	if(*u_data == NULL || *k_data == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}

	memcpy(*u_data, &data[16], *u_len); (*u_data)[*u_len] = '\0';
	memcpy(*k_data, &data[16+ *u_len], *k_len);
	if(ex_data) memcpy(ex_data, &data[16 + *u_len + *k_len], ed_len);

	uint64_t siglen = (decbe64(*k_data) + 7) / 8;

	if(p_len != kb_len + db_len + 17 + siglen) {
		LOG("actual len: %d, expected len: %d", p_len,
			kb_len + db_len + 17 + siglen);
		goto inv;
	}

	/* now verify the message */
	if(rsa_wire2pubkey(*k_data, *k_len, &pkey) != 0) {
		goto inv;
	}

	/* reencrypt the payload so we can verify the sig */
	chacha_enc(symm, 32, 0, data, data, db_len - 32);
	int valid = 0;
	if(rsa_pss_verify(&pkey, &payload[p_len-siglen], siglen, payload,
		p_len-siglen, &valid) != 0) {
		goto inv;
	}

	if(!valid) {
		LOG("signature invalid");
		goto inv;
	}

	inv = 0;
end:
	ret = 0;
err:
	rsa_free_prikey(&rkey);
	rsa_free_pubkey(&pkey);
	memsets(keys, 0, sizeof(keys));
	memsets(mac, 0, sizeof(mac));
	if(inv && !ret) ret = 1;
	return ret;
inv:
/* invalid message reject it but do not error */
	goto end;
}

static int send_friendreq_message(struct server_connection *sc,
	struct account *acc, uint8_t target[32], uint8_t *pkey,
	uint64_t pkeylen) {
	return send_rsa_message(sc, 1, acc, target, pkey, pkeylen, NULL, 0);
}

int parse_friendreq(uint8_t *sender, uint8_t *payload, uint64_t p_len) {
	LOG("parsing friend request");
	int ret = -1, inv = -1;
	struct friendreq *freq = NULL;
	/* start building the friendreq struct */
	freq = malloc(sizeof(*freq));
	if(freq == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}

	int parse_ret = parse_rsa_message(acc, sender, payload, p_len,
		&freq->uname, &freq->u_len,
		&freq->pkey, &freq->k_len,
		NULL, 0);
	if(parse_ret < 0) {
		ERR("failed to parse friend request");
		goto err;
	}
	if(parse_ret > 0) {
		LOG("invalid message");
		goto end;
	}

	/* everything is valid, message is parsed, place it in the queue */
	struct notif *n = malloc(sizeof(struct notif));
	if(n == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}

	n->type = 2;
	n->freq = freq;

	n->next = NULL;

	add_notif(n);

	inv = 0;
end:
	ret = 0;
err:
	if(ret || inv) free_friendreq(freq);
	return ret;
}

int parse_friendreq_response(uint8_t *sender, uint8_t *payload, uint64_t p_len) {
	LOG("parsing friend request response");
	int ret = -1, inv = -1;

	uint8_t keys[0x80];
	char *uname; uint64_t u_len;
	uint8_t *pkey; uint64_t k_len;

	struct friend *f = NULL;

	int parse_ret = parse_rsa_message(acc, sender, payload, p_len,
		&uname, &u_len,
		&pkey, &k_len,
		keys, 0x80);
	if(parse_ret < 0) {
		ERR("failed to parse friend request response");
		goto err;
	}
	if(parse_ret > 0) {
		LOG("invalid message");
		goto end;
	}

	/* build the friend structure */
	f = init_friend(uname, pkey, u_len, k_len);

	memcpy(f->r_symm_key, &keys[0x00], 0x20);
	memcpy(f->r_hmac_key, &keys[0x20], 0x20);
	memcpy(f->s_symm_key, &keys[0x40], 0x20);
	memcpy(f->s_hmac_key, &keys[0x60], 0x20);

	if(add_friend(f) != 0) {
		goto err;
	}

	/* everything is valid, message is parsed, place it in the queue */
	struct notif *n = malloc(sizeof(struct notif));
	if(n == NULL) {
		ERR("failed to allocate memory");
		goto err;
	}

	n->type = 3;
	n->fr = f;

	n->next = NULL;

	add_notif(n);

	inv = 0;
end:
	ret = 0;
err:
	if(ret || inv) delete_friend(f);
	return ret;
}

int friendreq_response(struct friendreq *freq) {
	uint8_t hash[32];
	char sig[65];
	sha256(freq->pkey, freq->k_len, hash);
	to_hex(hash, 32, sig);
	printf("friend request from %s\n"
		"key signature: %s\n"
		"accept friend request? [y/n] ",
		freq->uname, sig);
	LOG("friend request: %s %s", freq->uname, sig);
	int res = yn_prompt();
	if(res == -1) {
		ERR("failed to get response");
	}

	if(res == 0) {
		printf("friend request rejected\n");
		return 0;
	}

	/* send the message */
	return friendreq_send_response(freq);
}

static int friendreq_send_resp_message(struct friendreq *freq,
	struct friend *f) {
	uint8_t keys[0x80];

	if(cs_rand(keys, 0x80) != 0) {
		ERR("failed to generate random keys");
		return 1;
	}

	uint8_t target[0x20];
	gen_uid(freq->uname, target);

	int ret = send_rsa_message(&sc, 2, acc, target, freq->pkey, freq->k_len,
		keys, 0x80);
	if(ret != 0) {
		ERR("failed to send response message");
		return 1;
	}

	memcpy(f->s_symm_key, &keys[0x00], 0x20);
	memcpy(f->s_hmac_key, &keys[0x20], 0x20);
	memcpy(f->r_symm_key, &keys[0x40], 0x20);
	memcpy(f->r_hmac_key, &keys[0x60], 0x20);

	memsets(keys, 0, sizeof(keys));
	memsets(target, 0, sizeof(target));

	return 0;
}

int friendreq_send_response(struct friendreq *freq) {
	LOG("responding to friend request from %s", freq->uname);
	struct friend *f = init_friend(freq->uname, freq->pkey,
		freq->u_len, freq->k_len);
	if(f == NULL) {
		ERR("failed to initialize friend");
		return 1;
	}
	/* send the response and then add them as a friend */
	if(friendreq_send_resp_message(freq, f) != 0) {
		goto err;
	}

	if(add_friend(f) != 0) {
		goto err;
	}

	return 0;
err:
	delete_friend(f);
	return 1;
}

void free_friendreq(struct friendreq *freq) {
	if(!freq) return;
	zfree(freq->uname, freq->u_len);
	zfree(freq->pkey, freq->k_len);
	zfree(freq, sizeof(*freq));
}

