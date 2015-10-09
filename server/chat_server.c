#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wordexp.h>

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <ibcrypt/rsa.h>
#include <ibcrypt/rsa_util.h>
#include <ibcrypt/sha256.h>
#include <ibcrypt/zfree.h>

#include <libibur/util.h>

#include "client_handler.h"
#include "user_db.h"
#include "../crypto/keyfile.h"
#include "../inet/connect.h"
#include "../util/line_prompt.h"
#include "../util/defaults.h"

/* private info */
RSA_KEY server_key;
char *password;
/* ------------ */

RSA_PUBLIC_KEY server_pub_key;

void usage(char *argv0) {
	fprintf(stderr, "usage: %s [-p port] "
		"[-d server_root_directory] [--no-pw] <key file>\n", argv0);
}

static struct option longopts[] = {
	{ "port", 1, NULL, 'p' },
	{ "root-dir", 1, NULL, 'd' },
	{ "no-pw", 0, NULL, 'n' },
	{ NULL, 0, NULL, 0 },
};
static char *optstring = "p:d:";
int process_opts(int argc, char **argv);
void print_opts();

static volatile sig_atomic_t stop;
void init_sighandlers();
void signal_stop(int signum);

int load_server_key(char *keyfile, char *password, RSA_KEY *server_key);
int check_root_dir(char *fname);
int server_bind_err(struct sock server_socket);

int handle_connections(int server_socket);

static struct {
	char *port;
	char *root_dir;
	char *keyfile;
	int use_password;
} opts;

/* program entry point */
int chat_server(int argc, char **argv) {
	if(process_opts(argc, argv) != 0) {
		return 1;
	}
	print_opts();

	password = NULL;
	memset(&server_key, 0, sizeof(RSA_KEY));

	if(opts.use_password) {
		password = line_prompt("Server password", NULL, 1);
		if(password == NULL) {
			fprintf(stderr, "failed to read password\n");
			return 1;
		}
	}

	/* TODO: loading of the database and initialization of the delivery queues */
	if(load_server_key(opts.keyfile, password, &server_key) != 0) {
		goto err1;
	}

	/* make sure we have a root directory */
	if(check_root_dir(opts.root_dir) != 0) {
		goto err2;
	}

	/* load up the database */
	if(user_db_init(opts.root_dir) != 0) {
		goto err2;
	}

	/* set up the server */
	struct sock server_socket = server_bind(opts.port);
	if(server_bind_err(server_socket) != 0) {
		goto err2;
	}

	printf("server opened on port %s\n", opts.port);

	/* program main body */
	init_sighandlers();
	/* TODO: start the manager thread */
	if(handle_connections(server_socket.fd) != 0) {
		fprintf(stderr, "handle connections error: %s\n", strerror(errno));
		goto err3;
	}

	close(server_socket.fd);
	if(password) zfree(password, strlen(password));
	rsa_free_prikey(&server_key);

	return 0;

err3:
	close(server_socket.fd);
err2:
err1:
	/* cleanup */
	if(password) zfree(password, strlen(password));
	rsa_free_prikey(&server_key);

	return 1;
}

int handle_connections(int server_socket) {
	stop = 0;

	if(init_handler_table() != 0) {
		fprintf(stderr, "failed to initialize handler table: %s\n",
			strerror(errno));
		return 1;
	}

	fd_set rd_set;
	struct timeval timeout;

	while(stop == 0) {
		FD_ZERO(&rd_set);
		FD_SET(server_socket, &rd_set);
		timeout.tv_sec = 0;
		timeout.tv_usec = 1000;

		if(select(FD_SETSIZE, &rd_set, NULL, NULL, &timeout) == -1) {
			if(errno == EINTR) {
				continue;
			} else {
				goto err;
			}
		}

		if(FD_ISSET(server_socket, &rd_set)) {
			/* accept a connection and set it up */
			struct sock client = server_accept(server_socket);
			printf("received connection from %s with fd %d\n",
				client.address, client.fd);

			if(spawn_handler(client.fd) != 0) {
				goto err;
			}
		}
	}

	

	return 0;
err:
	return 1;
}

int process_opts(int argc, char **argv) {
	opts.port = DFLT_PORT;
	opts.root_dir = DFLT_ROOT_DIR;
	opts.use_password = 1;

	char option;
	do {
		option = getopt_long(argc, argv, optstring, longopts, NULL);
		switch(option) {
		case 'p':
			opts.port = optarg;
			break;
		case 'd':
			opts.root_dir = optarg;
			break;
		case 'n':
			opts.use_password = 0;
			break;
		}
	} while(option != -1);

	if(optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	opts.keyfile = argv[optind];

	/* expand the root dir */
	{ 
		wordexp_t expanded;
		if(wordexp(opts.root_dir, &expanded, WRDE_UNDEF) != 0) {
			fprintf(stderr, "failed to expand root dir\n");
			return 1;
		}

		if(expanded.we_wordc != 1) {
			fprintf(stderr, "invalid root dir\n");
			return 1;
		}

		opts.root_dir = strdup(expanded.we_wordv[0]);
		if(opts.root_dir == NULL) {
			fprintf(stderr, "strdup failed on root dir\n");
			return 1;
		}

		wordfree(&expanded);
	}

	return 0;
}

void print_opts() {
	printf("option values:\n"
	       "port    :%s\n"
	       "root_dir:%s\n"
	       "keyfile :%s\n"
	       "use_pass:%d\n",
	       opts.port,
	       opts.root_dir,
	       opts.keyfile,
	       opts.use_password);
}

int load_server_key(char *keyfile, char *password, RSA_KEY *server_key) {
	int ret = read_pri_key(keyfile, server_key, password);
	char *estr = NULL;
	if(ret != 0) {
		switch(ret) {
		case MEM_FAIL:
			estr = "failed to allocate memory";
			break;
		case CRYPTOGRAPHY_FAIL:
			estr = "cryptography error occurred";
			break;
		case OPEN_FAIL:
			estr = "failed to open key file";
			break;
		case READ_FAIL:
			estr = "failed to read from key file";
			break;
		case INVALID_FILE:
			estr = "invalid key file";
			break;
		case INVALID_MAC:
			estr = "invalid file or bad file";
			break;
		case NO_PASSWORD:
			estr = "password required and not given";
			break;
		}

		fprintf(stderr, "%s\n", estr);

		return 1;
	}

	if(rsa_pub_key(server_key, &server_pub_key) != 0) {
		fprintf(stderr, "failed to create public key\n");
		return 1;
	}

	/* print the fingerprint of the key */
	{
		uint64_t len = rsa_pubkey_bufsize(server_pub_key.bits);
		uint8_t* key_bin = malloc(len);
		if(key_bin == NULL) {
			fprintf(stderr, "failed to allocate memory\n");
			return 1;
		}
		uint8_t hash[32];
		char hex[65];

		rsa_pubkey2wire(&server_pub_key, key_bin, len);
		sha256(key_bin, len, hash);
		to_hex(hash, 32, hex);
		hex[64] = '\0';

		printf("server fingerprint:\n"
			"%s\n",
			hex);

		free(key_bin);
	}

	return 0;
}

int check_root_dir(char *fname) {
	struct stat st = {0};
	if(stat(fname, &st) == -1) {
		if(errno != ENOENT) {
			fprintf(stderr, "failed to open root directory: %s\n", fname);
			return -1;
		}

		/* directory doesn't exist, create it */
		if(mkdir(fname, 0700) != 0) {
			fprintf(stderr, "failed to create root directory: %s\n", fname);
			return -1;
		}
	} else {
		/* make sure its a directory */
		if(!S_ISDIR(st.st_mode)) {
			fprintf(stderr, "specified root is not a directory: %s\n", fname);
			return -1;
		}
	}
	return 0;
}

int server_bind_err(struct sock server_socket) {
	if(server_socket.fd == -1) {
		fprintf(stderr, "failed to bind: %s\n", strerror(errno));
		return 1;
	}
	if(server_socket.fd == -2) {
		fprintf(stderr, "failed to bind: %s\n", gai_strerror(errno));
		return 1;
	}

	return 0;
}

void init_sighandlers() {
	signal(SIGPIPE, SIG_IGN);
	signal(SIGINT, signal_stop);
	signal(SIGQUIT, signal_stop);
	signal(SIGHUP, signal_stop);
	signal(SIGTERM, signal_stop);
}

void signal_stop(int signal) {
	stop = 1;
}

