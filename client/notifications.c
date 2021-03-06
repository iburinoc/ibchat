#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <limits.h>

#include <sys/stat.h>

#include <ibcrypt/chacha.h>
#include <ibcrypt/rand.h>
#include <ibcrypt/sha256.h>
#include <ibcrypt/zfree.h>

#include <libibur/endian.h>

#include "../util/line_prompt.h"
#include "../util/log.h"

#include "cli.h"
#include "ibchat_client.h"
#include "notifications.h"
#include "datafile.h"
#include "conversation.h"
#include "friendreq.h"

static int nf_p_fill(void *_arg, uint8_t *ptr) {
	return 0;
}

static int nf_s_key(void *_arg, uint8_t *ptr, uint8_t *key) {
	struct account *arg = (struct account *) _arg;
	memcpy(key, arg->n_symm, 0x20);
	return 0;
}

static int nf_h_key(void *_arg, uint8_t *ptr, uint8_t *key) {
	struct account *arg = (struct account *) _arg;
	memcpy(key, arg->n_hmac, 0x20);
	return 0;
}

static uint64_t nf_datalen(void *_data) {
	return notif_bin_len((struct notif *) _data);
}

static uint8_t *nf_datawrite(void *_data, uint8_t *ptr) {
	return notif_bin_write((struct notif *) _data, ptr);
}

static uint8_t *nf_dataread(void **_data, void *arg, uint8_t *ptr) {
	return notif_bin_parse((struct account *) arg, (struct notif **) _data, ptr);
}

static struct format_desc nf_format = {
	0x00,
	nf_p_fill,
	nf_s_key,
	nf_h_key,

	offsetof(struct notif, next),
	nf_datalen,
	nf_datawrite,
	nf_dataread,
};

int notiflist_len(struct notif *n) {
	acquire_readlock(&lock);
	int num = 0;
	struct notif *cur = n;
	while(cur) {
		num++;
		cur = cur->next;
	}

	release_readlock(&lock);

	return num;
}

void notiflist_free(struct notif *n) {	
	struct notif *cur = n;
	while(cur) {
		struct notif *next = cur->next;
		notif_free(cur);
		cur = next;
	}
}

void notif_free(struct notif *n) {
	switch(n->type) {
	case 1:
	case 3:
		break;
	case 2:
		free_friendreq(n->freq);
		break;
	}
	free(n);
}

void print_notif(struct notif *n, int num);
int notif_selected(struct account *acc, struct notif *n);
int view_notifs(struct account *acc) {
	struct notif *n = notifs;
	int ret = -1;
	acquire_writelock(&lock);
	int nnum = 0;
	struct notif *cur = n;
	while(cur) {
		print_notif(cur, nnum+1);
		nnum++;
		cur = cur->next;
	}
	printf("%4d: clear all\n", 0);

	int sel = num_prompt("selection", 0, nnum);
	if(sel == ULLONG_MAX) {
		goto err;
	}

	if(sel > 0) {
		cur = n;
		while(sel - 1) {
			cur = cur->next;
			sel--;
		}
		ret = notif_selected(acc, cur);
	} else {
		/* clear notifications */
		notiflist_free(notifs);
		notifs = NULL;
		ret = write_notiffile(acc, NULL);
		release_writelock(&lock);
	}
end:
	return ret;
err:
	release_writelock(&lock);
	goto end;
}

int notif_selected(struct account *acc, struct notif *n) {
	/* remove the notification and rewrite */
	{
		struct notif **list = &notifs;
		while(*list != n) {
			list = &((*list)->next);
		}
		*list = n->next;

		if(write_notiffile(acc, notifs) != 0) {
			return 1;
		}
	}
	release_writelock(&lock);
	switch(n->type) {
	case 1:
	case 3:
		/* open conversation with said person */
		return start_conversation(n->fr);
		break;
	case 2:
		/* prompt for response to friend request */
		return friendreq_response(n->freq);
		break;
	}
	notif_free(n);

	return 0;
}

void print_notif(struct notif *n, int num) {
	switch(n->type) {
	case 1:
		printf("%4d: %llu unread messages from %s\n",
			num, (long long unsigned) n->nunread, n->fr->uname);
		break;
	case 2:
		printf("%4d: friend request from %s\n",
			num, n->freq->uname);
		break;
	case 3:
		printf("%4d: %s accepted your friend request\n",
			num, n->fr->uname);
		break;
	}
}

int add_notif(struct notif *n) {
	int ret;
	if((ret = insert_notif(n)) != 0) return ret;

	if(get_mode() == 0) set_mode(0xff);
	return 0;
}

int add_new_message(struct friend *f) {
	int ret = -1;
	acquire_writelock(&lock);
	struct notif *cur = notifs;
	while(cur) {
		if(cur->type == 1 && cur->fr == f) {
			cur->nunread++;
			break;
		}
	}
	release_writelock(&lock);
	if(cur != NULL) {
		ret = write_notiffile(acc, notifs);
	} else {
		struct notif *n = malloc(sizeof(struct notif));
		if(n == NULL) {
			ERR("failed to allocate memory");
			goto err;
		}

		n->type = 1;
		n->nunread = 1;
		n->fr = f;

		n->next = NULL;

		ret = insert_notif(n);
		if(get_mode() == 0) set_mode(0xff);
	}

err:
	return ret;
}

int insert_notif(struct notif *n) {
	int ret = 0;
	acquire_writelock(&lock);

	struct notif **cur = &notifs;
	while(*cur) {
		cur = &(*cur)->next;
	}

	*cur = n;

	ret = write_notiffile(acc, notifs);
	release_writelock(&lock);
	return ret;
}

int init_notiffile(struct account *acc) {
	uint8_t buf[96];

	if(cs_rand(buf, 96) != 0) {
		ERR("failed to generate random numbers");
		return -1;
	}

	memcpy(acc->n_file, &buf[ 0], 32);
	memcpy(acc->n_symm, &buf[32], 32);
	memcpy(acc->n_hmac, &buf[64], 32);

	memsets(buf, 0, sizeof(buf));

	char *fname = file_path(acc->n_file);
	if(fname == NULL) {
		return -1;
	}

	struct stat st;
	int ret = stat(fname, &st);
	if(ret != 0) {
		if(errno != ENOENT) {
			ERR("could not access notif file dir: "
				"%s", fname);
			return -1;
		}
	} else {
		ERR("notif file already exists, RNG unsafe: %s",
			fname);
		return -1;
	}

	free(fname);

	return write_notiffile(acc, NULL);
}

int write_notiffile(struct account *acc, struct notif *notifs) {
	int ret;
	char *path = file_path(acc->n_file);
	if(path == NULL) {
		return -1;
	}
	LOG("writing notiffile: %s", path);
	ret = write_datafile(path, acc, notifs, &nf_format);
	free(path);
	return ret;
}

int read_notiffile(struct account *acc, struct notif **notifs) {
	int ret;
	char *path = file_path(acc->n_file);
	if(path == NULL) {
		return -1;
	}
	LOG("reading notiffile: %s", path);
	ret = read_datafile(path, acc, (void **) notifs, &nf_format);
	free(path);
	return ret;
}

uint64_t notif_bin_len(struct notif *n) {
	uint64_t len = 1;
	switch(n->type) {
	case 1:
		len += 0x20;
		len += 0x08;
		break;
	case 2:
		len += 0x08;
		len += 0x08;
		len += n->freq->u_len;
		len += n->freq->k_len;
		break;
	case 3:
		len += 0x20;
		break;
	}

	return len;
}

uint8_t *notif_bin_write(struct notif *n, uint8_t *ptr) {
	*ptr = (uint8_t) n->type;
	ptr++;
	switch(n->type) {
	case 1:
		memcpy(ptr, n->fr->uid, 0x20); ptr += 0x20;
		encbe64(n->nunread, ptr); ptr += 8;
		break;
	case 2:
		encbe64(n->freq->u_len, ptr); ptr += 8;
		encbe64(n->freq->k_len, ptr); ptr += 8;
		memcpy(ptr, n->freq->uname, n->freq->u_len);
			ptr += n->freq->u_len;
		memcpy(ptr, n->freq->pkey, n->freq->k_len);
			ptr += n->freq->k_len;
		break;
	case 3:
		memcpy(ptr, n->fr->uid, 0x20); ptr += 0x20;
		break;
	}

	return ptr;
}

uint8_t *notif_bin_parse(struct account *acc, struct notif **_n, uint8_t *ptr) {
	struct notif *n = malloc(sizeof(struct notif));
	if(n == NULL) {
		goto memfail;
	}
	n->type = *ptr;
	ptr++;

	struct friend *f;

	switch(n->type) {
	case 1:
		n->nunread = decbe64(&ptr[0x20]);
	case 3:
		f = acc->friends;
		while(f) {
			if(memcmp(f->uid, ptr, 0x20) == 0) {
				n->fr = f;
				break;
			}
			f = f->next;
		}
		if(!f) {
			ERR("referenced friend not found");
			return NULL;
		}
		ptr += 0x28;
		break;
	case 2:
		if((n->freq = malloc(sizeof(struct friendreq))) == NULL) {
			goto memfail;
		}
		n->freq->u_len = decbe64(ptr); ptr += 8;
		n->freq->k_len = decbe64(ptr); ptr += 8;
		if((n->freq->uname = malloc(n->freq->u_len+1)) == NULL) {
			goto memfail;
		}
		if((n->freq->pkey = malloc(n->freq->k_len)) == NULL) {
			goto memfail;
		}

		memcpy(n->freq->uname, ptr, n->freq->u_len);
		n->freq->uname[n->freq->u_len] = '\0';
		ptr += n->freq->u_len;

		memcpy(n->freq->pkey, ptr, n->freq->k_len);
		ptr += n->freq->k_len;
		break;
	}

	*_n = n;

	return ptr;

memfail:
	ERR("failed to allocate memory");
	return NULL;
}

