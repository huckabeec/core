/* Copyright (C) 2002-2004 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "message-send.h"
#include "message-size.h"
#include "imap-date.h"
#include "commands.h"
#include "imap-fetch.h"
#include "imap-util.h"

#include <stdlib.h>

const struct imap_fetch_handler default_handlers[7];
static buffer_t *fetch_handlers = NULL;

static int imap_fetch_handler_cmp(const void *p1, const void *p2)
{
        const struct imap_fetch_handler *h1 = p1, *h2 = p2;

	return strcmp(h1->name, h2->name);
}

void imap_fetch_handlers_register(const struct imap_fetch_handler *handlers,
				  size_t count)
{
	void *data;
	size_t size;

	if (fetch_handlers == NULL)
		fetch_handlers = buffer_create_dynamic(default_pool, 128);
	buffer_append(fetch_handlers, handlers, sizeof(*handlers) * count);

	data = buffer_get_modifyable_data(fetch_handlers, &size);
	qsort(data, size / sizeof(*handlers), sizeof(*handlers),
	      imap_fetch_handler_cmp);
}

static int imap_fetch_handler_bsearch(const void *name_p, const void *handler_p)
{
	const char *name = name_p;
        const struct imap_fetch_handler *h = handler_p;
	int i;

	for (i = 0; h->name[i] != '\0'; i++) {
		if (h->name[i] != name[i]) {
			if (name[i] < 'A' || name[i] >= 'Z')
				return -1;
			return name[i] - h->name[i];
		}
	}

	return name[i] < 'A' || name[i] >= 'Z' ? 0 : -1;
}

int imap_fetch_init_handler(struct imap_fetch_context *ctx, const char *name,
			    struct imap_arg **args)
{
	const struct imap_fetch_handler *handler;

	handler = bsearch(name, fetch_handlers->data,
			  fetch_handlers->used /
			  sizeof(struct imap_fetch_handler),
                          sizeof(struct imap_fetch_handler),
			  imap_fetch_handler_bsearch);
	if (handler == NULL) {
		client_send_command_error(ctx->client,
			t_strconcat("Unknown command ", name, NULL));
		return FALSE;
	}

	return handler->init(ctx, name, args);
}

struct imap_fetch_context *imap_fetch_init(struct client *client)
{
	struct imap_fetch_context *ctx;

	if (fetch_handlers == NULL) {
		imap_fetch_handlers_register(default_handlers,
					     sizeof(default_handlers) /
					     sizeof(default_handlers[0]));
	}

	ctx = p_new(client->cmd_pool, struct imap_fetch_context, 1);
	ctx->client = client;
	ctx->box = client->mailbox;

	ctx->cur_str = str_new(default_pool, 8192);
	ctx->seen_flag.flags = MAIL_SEEN;
	ctx->all_headers_buf = buffer_create_dynamic(client->cmd_pool, 128);
	ctx->handlers = buffer_create_dynamic(client->cmd_pool, 128);
	return ctx;
}

void imap_fetch_add_handler(struct imap_fetch_context *ctx,
			    imap_fetch_handler_t *handler, void *context)
{
	const struct imap_fetch_context_handler *handlers;
	struct imap_fetch_context_handler h;
	size_t i, size;

	if (context == NULL) {
		/* don't allow duplicate handlers */
		handlers = buffer_get_data(ctx->handlers, &size);
		size /= sizeof(*handlers);

		for (i = 0; i < size; i++) {
			if (handlers[i].handler == handler &&
			    handlers[i].context == NULL)
				return;
		}
	}

	memset(&h, 0, sizeof(h));
	h.handler = handler;
	h.context = context;

	buffer_append(ctx->handlers, &h, sizeof(h));
}

void imap_fetch_begin(struct imap_fetch_context *ctx,
		      struct mail_search_arg *search_arg)
{
	const void *null = NULL;
	const void *data;

	if (ctx->flags_update_seen) {
		if (mailbox_is_readonly(ctx->box))
			ctx->flags_update_seen = FALSE;
		else if (!ctx->flags_have_handler) {
			ctx->flags_show_only_seen_changes = TRUE;
			(void)imap_fetch_init_handler(ctx, "FLAGS", NULL);
		}
	}

	if (buffer_get_used_size(ctx->all_headers_buf) != 0 &&
	    ((ctx->fetch_data & (MAIL_FETCH_STREAM_HEADER |
				 MAIL_FETCH_STREAM_BODY)) == 0)) {
		buffer_append(ctx->all_headers_buf, &null, sizeof(null));

		data = buffer_get_data(ctx->all_headers_buf, NULL);
		ctx->all_headers_ctx =
			mailbox_header_lookup_init(ctx->box, data);
	}

	ctx->trans = mailbox_transaction_begin(ctx->box, TRUE);
	ctx->select_counter = ctx->client->select_counter;
	ctx->search_ctx =
		mailbox_search_init(ctx->trans, NULL, search_arg, NULL,
				    ctx->fetch_data, ctx->all_headers_ctx);
}

int imap_fetch(struct imap_fetch_context *ctx)
{
	const struct imap_fetch_context_handler *handlers;
	size_t size;
	int ret;

	if (ctx->cont_handler != NULL) {
		if ((ret = ctx->cont_handler(ctx)) <= 0)
			return ret;

		ctx->cont_handler = NULL;
		ctx->cur_offset = 0;
                ctx->cur_handler++;
	}

	handlers = buffer_get_data(ctx->handlers, &size);
	size /= sizeof(*handlers);

	for (;;) {
		if (o_stream_get_buffer_used_size(ctx->client->output) >=
		    CLIENT_OUTPUT_OPTIMAL_SIZE) {
			ret = o_stream_flush(ctx->client->output);
			if (ret <= 0)
				return ret;
		}

		if (ctx->cur_mail == NULL) {
			if (ctx->cur_input != NULL) {
				i_stream_unref(ctx->cur_input);
                                ctx->cur_input = NULL;
			}

			ctx->cur_mail = mailbox_search_next(ctx->search_ctx);
			if (ctx->cur_mail == NULL)
				break;

			str_printfa(ctx->cur_str, "* %u FETCH (",
				    ctx->cur_mail->seq);
			if (o_stream_send(ctx->client->output,
					  str_data(ctx->cur_str),
					  str_len(ctx->cur_str)) < 0)
				return -1;

			str_truncate(ctx->cur_str, 0);
			str_append_c(ctx->cur_str, ' ');
			ctx->first = TRUE;
			ctx->line_finished = FALSE;
		}

		for (; ctx->cur_handler < size; ctx->cur_handler++) {
			t_push();
			ret = handlers[ctx->cur_handler].
				handler(ctx, ctx->cur_mail,
					handlers[ctx->cur_handler].context);
			t_pop();

			if (ret <= 0) {
				i_assert(ret < 0 || ctx->cont_handler != NULL);
				return ret;
			}

			ctx->cont_handler = NULL;
			ctx->cur_offset = 0;
		}

		if (str_len(ctx->cur_str) > 1) {
			if (o_stream_send(ctx->client->output,
					  str_data(ctx->cur_str) + ctx->first,
					  str_len(ctx->cur_str) - 1 -
					  ctx->first) < 0)
				return -1;
			str_truncate(ctx->cur_str, 0);
		}

		ctx->line_finished = TRUE;
		if (o_stream_send(ctx->client->output, ")\r\n", 3) < 0)
			return -1;

		ctx->cur_mail = NULL;
		ctx->cur_handler = 0;
	}

	return 1;
}

int imap_fetch_deinit(struct imap_fetch_context *ctx)
{
	str_free(ctx->cur_str);

	if (!ctx->line_finished) {
		if (o_stream_send(ctx->client->output, ")\r\n", 3) < 0)
			return -1;
	}

	if (ctx->cur_input != NULL) {
		i_stream_unref(ctx->cur_input);
		ctx->cur_input = NULL;
	}

	if (ctx->search_ctx != NULL) {
		if (mailbox_search_deinit(ctx->search_ctx) < 0)
			ctx->failed = TRUE;
	}
	if (ctx->all_headers_ctx != NULL)
		mailbox_header_lookup_deinit(ctx->all_headers_ctx);

	if (ctx->trans != NULL) {
		if (ctx->failed)
			mailbox_transaction_rollback(ctx->trans);
		else {
			if (mailbox_transaction_commit(ctx->trans, 0) < 0)
				ctx->failed = TRUE;
		}
	}
	return ctx->failed ? -1 : 0;
}

static int fetch_body(struct imap_fetch_context *ctx, struct mail *mail,
		      void *context __attr_unused__)
{
	const char *body;

	body = mail->get_special(mail, MAIL_FETCH_IMAP_BODY);
	if (body == NULL)
		return -1;

	if (ctx->first)
		ctx->first = FALSE;
	else {
		if (o_stream_send(ctx->client->output, " ", 1) < 0)
			return -1;
	}

	if (o_stream_send(ctx->client->output, "BODY (", 6) < 0 ||
	    o_stream_send_str(ctx->client->output, body) < 0 ||
	    o_stream_send(ctx->client->output, ")", 1) < 0)
		return -1;
	return 1;
}

static int fetch_body_init(struct imap_fetch_context *ctx, const char *name,
			   struct imap_arg **args)
{
	if (name[4] == '\0') {
		ctx->fetch_data |= MAIL_FETCH_IMAP_BODY;
		imap_fetch_add_handler(ctx, fetch_body, NULL);
		return TRUE;
	}
	return fetch_body_section_init(ctx, name, args);
}

static int fetch_bodystructure(struct imap_fetch_context *ctx,
			       struct mail *mail, void *context __attr_unused__)
{
	const char *bodystructure;

	bodystructure = mail->get_special(mail, MAIL_FETCH_IMAP_BODYSTRUCTURE);
	if (bodystructure == NULL)
		return -1;

	if (ctx->first)
		ctx->first = FALSE;
	else {
		if (o_stream_send(ctx->client->output, " ", 1) < 0)
			return -1;
	}

	if (o_stream_send(ctx->client->output, "BODYSTRUCTURE (", 15) < 0 ||
	    o_stream_send_str(ctx->client->output, bodystructure) < 0 ||
	    o_stream_send(ctx->client->output, ")", 1) < 0)
		return -1;

	return 1;
}

static int fetch_bodystructure_init(struct imap_fetch_context *ctx,
				    const char *name __attr_unused__,
				    struct imap_arg **args __attr_unused__)
{
	ctx->fetch_data |= MAIL_FETCH_IMAP_BODYSTRUCTURE;
	imap_fetch_add_handler(ctx, fetch_bodystructure, NULL);
	return TRUE;
}

static int fetch_envelope(struct imap_fetch_context *ctx, struct mail *mail,
			  void *context __attr_unused__)
{
	const char *envelope;

	envelope = mail->get_special(mail, MAIL_FETCH_IMAP_ENVELOPE);
	if (envelope == NULL)
		return -1;

	if (ctx->first)
		ctx->first = FALSE;
	else {
		if (o_stream_send(ctx->client->output, " ", 1) < 0)
			return -1;
	}

	if (o_stream_send(ctx->client->output, "ENVELOPE (", 10) < 0 ||
	    o_stream_send_str(ctx->client->output, envelope) < 0 ||
	    o_stream_send(ctx->client->output, ")", 1) < 0)
		return -1;
	return 1;
}

static int fetch_envelope_init(struct imap_fetch_context *ctx,
			       const char *name __attr_unused__,
			       struct imap_arg **args __attr_unused__)
{
	ctx->fetch_data |= MAIL_FETCH_IMAP_ENVELOPE;
	imap_fetch_add_handler(ctx, fetch_envelope, NULL);
	return TRUE;
}

static int fetch_flags(struct imap_fetch_context *ctx, struct mail *mail,
		       void *context __attr_unused__)
{
	const struct mail_full_flags *flags;
	struct mail_full_flags full_flags;

	flags = mail->get_flags(mail);
	if (flags == NULL)
		return -1;

	if (ctx->flags_update_seen && (flags->flags & MAIL_SEEN) == 0) {
		/* Add \Seen flag */
		full_flags = *flags;
		full_flags.flags |= MAIL_SEEN;
		flags = &full_flags;

		if (mail->update_flags(mail, &ctx->seen_flag, MODIFY_ADD) < 0)
			return -1;
	} else if (ctx->flags_show_only_seen_changes) {
		return 1;
	}

	str_append(ctx->cur_str, "FLAGS (");
	imap_write_flags(ctx->cur_str, flags);
	str_append(ctx->cur_str, ") ");
	return 1;
}

static int fetch_flags_init(struct imap_fetch_context *ctx,
			    const char *name __attr_unused__,
			    struct imap_arg **args __attr_unused__)
{
	ctx->flags_have_handler = TRUE;
	ctx->fetch_data |= MAIL_FETCH_FLAGS;
	imap_fetch_add_handler(ctx, fetch_flags, NULL);
	return TRUE;
}

static int fetch_internaldate(struct imap_fetch_context *ctx, struct mail *mail,
			      void *context __attr_unused__)
{
	time_t time;

	time = mail->get_received_date(mail);
	if (time == (time_t)-1)
		return -1;

	str_printfa(ctx->cur_str, "INTERNALDATE \"%s\" ",
		    imap_to_datetime(time));
	return 1;
}


static int fetch_internaldate_init(struct imap_fetch_context *ctx,
				   const char *name __attr_unused__,
				   struct imap_arg **args __attr_unused__)
{
	ctx->fetch_data |= MAIL_FETCH_RECEIVED_DATE;
	imap_fetch_add_handler(ctx, fetch_internaldate, NULL);
	return TRUE;
}

static int fetch_uid(struct imap_fetch_context *ctx, struct mail *mail,
		     void *context __attr_unused__)
{
	str_printfa(ctx->cur_str, "UID %u ", mail->uid);
	return 1;
}

static int fetch_uid_init(struct imap_fetch_context *ctx __attr_unused__,
			  const char *name __attr_unused__,
			  struct imap_arg **args __attr_unused__)
{
	imap_fetch_add_handler(ctx, fetch_uid, NULL);
	return TRUE;
}

const struct imap_fetch_handler default_handlers[7] = {
	{ "BODY", fetch_body_init },
	{ "BODYSTRUCTURE", fetch_bodystructure_init },
	{ "ENVELOPE", fetch_envelope_init },
	{ "FLAGS", fetch_flags_init },
	{ "INTERNALDATE", fetch_internaldate_init },
	{ "RFC822", fetch_rfc822_init },
	{ "UID", fetch_uid_init }
};
