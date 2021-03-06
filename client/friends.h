#ifndef CLIENT_FRIENDS_H
#define CLIENT_FRIENDS_H

#include <stdint.h>

#include "account.h"

struct friend {
	uint64_t u_len;
	uint64_t k_len;
	char *uname;
	uint8_t *pkey;

	uint8_t uid[32];

	uint8_t c_file[32];

	uint8_t f_symm_key[32];
	uint8_t f_hmac_key[32];
	uint8_t s_symm_key[32];
	uint8_t s_hmac_key[32];
	uint8_t r_symm_key[32];
	uint8_t r_hmac_key[32];
	uint64_t s_nonce;
	uint64_t r_nonce;

	struct friend *next;
};

char *friendfile_path(struct account *acc);
int init_friendfile(struct account *acc);
int write_friendfile(struct account *acc);
int read_friendfile(struct account *acc);

struct friend *init_friend(char *uname, uint8_t *pkey,
	uint64_t u_len, uint64_t k_len);
int add_friend(struct friend *f);
int delete_friend(struct friend *f);

uint64_t friend_bin_size(struct friend *f);
uint8_t *friend_write_bin(struct friend *f, uint8_t *ptr);
uint8_t *friend_parse_bin(struct friend **_f, uint8_t *ptr);

void friend_free(struct friend *f);
void friend_free_list(struct friend *f);

#endif

