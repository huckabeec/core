#ifndef CLIENT_H
#define CLIENT_H

#include "net.h"
#include "smtp-params.h"

#define CLIENT_MAIL_DATA_MAX_INMEMORY_SIZE (1024*128)

struct mail_recipient {
	struct client *client;
	const char *session_id;

	struct smtp_address *address;
	const char *detail; /* +detail part is also in address */
	struct smtp_params_rcpt params;

	struct anvil_query *anvil_query;
	bool anvil_connect_sent;
	struct mail_storage_service_user *service_user;
};

struct client_state {
	const char *name;
	const char *session_id;
	struct smtp_address *mail_from;
	struct smtp_params_mail mail_params;
	ARRAY(struct mail_recipient *) rcpt_to;
	unsigned int rcpt_idx;

	unsigned int data_end_idx;

	/* Initially we start writing to mail_data. If it grows too large,
	   start using mail_data_fd. */
	buffer_t *mail_data;
	int mail_data_fd;
	struct ostream *mail_data_output;
	const char *added_headers;

	struct timeval mail_from_timeval, data_end_timeval;

	struct mail *raw_mail;

	struct mail_user *dest_user;
	struct mail *first_saved_mail;
};

struct client {
	struct client *prev, *next;
	pool_t pool;

	const struct setting_parser_info *user_set_info;
	const struct lda_settings *unexpanded_lda_set;
	const struct lmtp_settings *lmtp_set;
	const struct master_service_settings *service_set;
	int fd_in, fd_out;
	struct io *io;
	struct istream *input;
	struct ostream *output;
	struct ssl_iostream *ssl_iostream;

	struct timeout *to_idle;
	time_t last_input;

	struct ip_addr remote_ip, local_ip;
	in_port_t remote_port, local_port;

	struct mail_user *raw_mail_user;
	const char *my_domain;
	char *lhlo;

	pool_t state_pool;
	struct client_state state;
	struct istream *dot_input;
	struct lmtp_proxy *proxy;
	unsigned int proxy_ttl;
	unsigned int proxy_timeout_secs;

	bool disconnected:1;
};

extern unsigned int clients_count;

struct client *client_create(int fd_in, int fd_out,
			     const struct master_service_connection *conn);
void client_destroy(struct client *client, const char *prefix,
		    const char *reason) ATTR_NULL(2, 3);
void client_disconnect(struct client *client, const char *prefix,
		       const char *reason);
void client_state_reset(struct client *client, const char *state_name);
void client_state_set(struct client *client, const char *name, const char *args);
const char *client_remote_id(struct client *client);

bool client_is_trusted(struct client *client);

void clients_destroy(void);

void client_input_handle(struct client *client);
int client_input_read(struct client *client);
void client_io_reset(struct client *client);

void client_send_line(struct client *client, const char *fmt, ...)
	ATTR_FORMAT(2, 3);

#endif
