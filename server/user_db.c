/* contains a hash table containing user data */
/* see dirstructure.txt */

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <sys/stat.h>

#include <libibur/endian.h>

#include <ibcrypt/sha256.h>

#include "chat_server.h"

#define TOP_LOAD (0.75)
#define BOT_LOAD (0.5 / 2)

#define MAX_SIZE ((uint64_t)1 << 20)
#define MIN_SIZE ((uint64_t) 16)

#define MAX_READERS INT_MAX - 1

static const char *USER_DIR_SUFFIX = "/users/";

static char *USER_DIR;

static const char USER_FILE_MAGIC[8] = "userdb\0\0";

struct user {

};

struct user_db_ent {
	struct user u;
	struct user_db_ent *next;
};

struct user_db_st {
	struct user_db_ent **buckets;
	uint64_t size;

	uint64_t elements;

	/* the use state indicates whether it can be written to/read from */
	/* see server/client_handler.c for a similar example */
	pthread_mutex_t use_state_mutex;
	pthread_cond_t use_state_cond;
	int use_state;
} db;

static uint64_t hash_id(uint8_t *id) {
	uint8_t shasum[32];
	sha256(id, 32, shasum);

	return  decbe64(&id[ 0]) ^
	        decbe64(&id[ 8]) ^
	        decbe64(&id[16]) ^
		decbe64(&id[24]);
}

static int init_user_db_st() {
	size_t size = MIN_SIZE * sizeof(struct user_db_ent *);
	db.buckets = malloc(size);
	if(db.buckets == NULL) {
		return 1;
	}
	memset(db.buckets, 0, size);

	db.size = MIN_SIZE;
	db.elements = 0;

	db.use_state = 0;
	if(pthread_cond_init(&db.use_state_cond, NULL) != 0) {
		return 1;
	}
	if(pthread_mutex_init(&db.use_state_mutex, NULL) != 0) {
		return 1;
	}

	return 0;
}

static int check_user_dir() {
	struct stat st = {0};
	if(stat(USER_DIR, &st) == -1) {
		if(errno != ENOENT) {
			fprintf(stderr, "failed to open user directory: %s\n", USER_DIR);
			return -1;
		}

		/* directory doesn't exist, create it */
		if(mkdir(USER_DIR, 0700) != 0) {
			fprintf(stderr, "failed to create user directory: %s\n", USER_DIR);
			return -1;
		}
	} else {
		/* make sure its a directory */
		if(!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "specified user directory is not a directory: %s\n", USER_DIR);
			return -1;
		}
	}
	return 0;
}

static int parse_user_file(char *name) {
#define ERR() do { fprintf(stderr, "invalid user file: %s\n", name);\
	goto err; } while(0);
#define READ(f, b, s)                                            \
        if(fread(b, s, 1, f) != 1) {                             \
		ERR();                                           \
        }

	FILE *uf = fopen(name, "rb");
	if(uf == NULL) {
		fprintf(stderr, "failed to open user file: %s\n", name);
		return 1;
	}

	uint8_t prefix[8 + 0x20 + 0x20 + 8];
	uint8_t *magic = prefix;
	uint8_t *uid = magic + 8;
	uint8_t *undelivered = uid + 0x20;
	uint8_t *sizebuf = undelivered + 0x20;
	RSA_PUBLIC_KEY pkey;

	uint64_t pkey_size;

	memset(&pkey, 0, sizeof(pkey));

	READ(uf, magic, 8);
	if(memcmp(magic, USER_FILE_MAGIC, 8) != 0) {
		ERR();
	}

	READ(uf, uid, 0x20);
	READ(uf, undelivered, 0x20);

	READ(uf, sizebuf, 8);

	int valid = 0;
	rsa_pss_verify(&server_pub_key, 

	int ret = 0;
	goto exit;
err:
	ret = 1;
exit:
	fclose(uf);
	memset(prefix, sizeof(prefix);
	rsa_free_pubkey(&pkey);

	return ret;
}

static int load_user_files() {
	if(check_user_dir() != 0) {
		return 1;
	}

	DIR *userdir = opendir(USER_DIR);
	if(userdir == NULL) {
		fprintf(stderr, "failed to open userdir: %s\n", USER_DIR);
		return 1;
	}

	size_t upathlen = strlen(USER_PATH);
	char *path = malloc(upathlen + 64 + 1);
	path[upathlen + 64] = '\0';
	if(path == NULL) {
		fprintf(stderr, "failed to allocate memory\n");
		return 1;
	}

	struct dirent *ent;
	char *name;
	while((ent = readdir(userdir)) != NULL) {
		name = ent->d_name;
		if(name[0] == '.') {
			continue;
		}
		if(strlen(name) != 64) {
			/* names are hex versions of sha256 hashes
			 * so they are 64 characters long */
			fprintf(stderr, "unrecognised file in user dir: %s\n", name);
			continue;
		}
		int valid = 1;
		for(int i = 0; i < 64; i++) {
			valid &= ((name[i] >= '0' && name[i] <= '9') ||
			          (name[i] >= 'a' && name[i] <= 'f'));
			path[upathlen + i] = name[i];
		}
		if(!valid) {
			fprintf(stderr, "unrecognised file in user dir: %s\n", name);
			continue;
		}
		printf("loading user file %s\n", name);

		if(parse_user_file(path) != 0) {
			fprintf(stderr, "failed to parse user file\n");
			continue;
		}
	}

	hash_id(NULL);

	return 0;
}

static int init_user_dir(char *root_dir) {
	USER_DIR = malloc(strlen(root_dir) + strlen(USER_DIR_SUFFIX) + 1);
	if(USER_DIR == NULL) {
		return 1;
	}

	strcpy(USER_DIR, root_dir);
	strcpy(USER_DIR + strlen(root_dir), USER_DIR_SUFFIX);

	return 0;
}

int init_user_db(char *root_dir) {
	/* set up the table */
	if(init_user_db_st() != 0) {
		return 1;
	}

	if(init_user_dir(root_dir) != 0) {
		return 1;
	}

	/* add all existing user files */
	if(load_user_files() != 0) {
		return 1;
	}

	return 0;
}

