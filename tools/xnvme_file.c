#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <libxnvme.h>
#include <libxnvmec.h>
#include <libxnvme_file.h>

#define IOSIZE_DEF 4096
#define QDEPTH_MAX 256
#define QDEPTH_DEF 16

struct cb_args {
	uint64_t nerrors;
	uint64_t ncompletions;
	uint64_t nsubmissions;
};

static void
cb_func(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct cb_args *work = cb_arg;

	work->ncompletions += 1;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		xnvmec_perr("cb_func()", errno);
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
		work->nerrors += 1;
	}

	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

int
read_write(struct xnvmec *cli)
{
	int size = cli->args.data_nbytes;
	const char *uri = cli->args.sys_uri;
	struct xnvme_dev *dev;
	ssize_t err;

	xnvmec_pinf("opening '%s'\n", uri);
	dev = xnvme_file_open(uri, XNVME_FILE_OFLG_CREATE | XNVME_FILE_OFLG_RDWR);
	if (dev == NULL) {
		xnvmec_perr("no xnvme device. abort mission!\n", errno);
		return errno;
	}
	xnvmec_pinf("opened nvme device %s", uri);

	char *buf;
	struct xnvme_cmd_ctx ctx;

	buf = xnvme_buf_alloc(dev, size);
	for (int u = 0; u < size; u++) {
		buf[u] = 'A';
	}

	ctx = xnvme_file_get_cmd_ctx(dev);
	err = xnvme_file_pwrite(&ctx, buf, size, 0);
	if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
		xnvmec_perr("xnvme_file_pwrite()", err);
		xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
		goto exit;
	}

	err = xnvme_file_pread(&ctx, buf, size, 0);
	if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
		xnvmec_perr("xnvme_file_pread()", err);
		xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
		goto exit;
	}

	xnvmec_pinf("Successfully wrote and read %d bytes to/from %s!", size, uri);

exit:
	xnvme_buf_free(dev, buf);
	xnvme_dev_close(dev);
	return 0;
}

int
dump_sync(struct xnvmec *cli)
{
	const char *fpath;
	struct xnvme_dev *fh;
	int flags;
	size_t buf_nbytes, tbytes, iosize;
	char *buf;

	fpath = cli->args.data_output;
	flags = XNVME_FILE_OFLG_CREATE | XNVME_FILE_OFLG_WRONLY;
	flags |= cli->given[XNVMEC_OPT_DIRECT] ? XNVME_FILE_OFLG_DIRECT_ON : 0x0;
	iosize = cli->given[XNVMEC_OPT_IOSIZE] ? cli->args.iosize : IOSIZE_DEF;

	fh = xnvme_file_open(fpath, flags);
	if (!fh) {
		xnvmec_perr("xnvme_file_open(fh)", errno);
		return errno;
	}
	tbytes = cli->args.data_nbytes;

	buf_nbytes = tbytes;
	buf = xnvme_buf_alloc(fh, buf_nbytes);
	if (!buf) {
		xnvmec_perr("xnvme_buf_alloc()", errno);
		goto exit;
	}
	xnvmec_buf_fill(buf, buf_nbytes, "anum");

	xnvmec_pinf("dump-sync: {fpath: %s, tbytes: %zu, buf_nbytes: %zu iosize: %zu}",
		    fpath, tbytes, buf_nbytes, iosize);

	xnvmec_timer_start(cli);

	for (size_t ofz = 0; ofz < tbytes; ofz += iosize) {
		struct xnvme_cmd_ctx ctx = xnvme_file_get_cmd_ctx(fh);
		size_t nbytes = XNVME_MIN_U64(iosize, tbytes - ofz);
		ssize_t res;

		res = xnvme_file_pwrite(&ctx, buf + ofz, nbytes, ofz);
		if (res || xnvme_cmd_ctx_cpl_status(&ctx)) {
			xnvmec_perr("xnvme_file_pwrite(fh)", res);
			xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
			goto exit;
		}
	}

	xnvmec_timer_stop(cli);
	xnvmec_timer_bw_pr(cli, "wall-clock", tbytes);

exit:
	xnvme_buf_free(fh, buf);
	xnvme_file_close(fh);
	return 0;
}

int
dump_async(struct xnvmec *cli)
{
	const char *fpath;
	struct xnvme_dev *fh;
	int flags;
	size_t buf_nbytes, tbytes, iosize;
	char *buf;

	struct cb_args cb_args = { 0 };
	struct xnvme_queue *queue = NULL;
	uint32_t qdepth;
	int err;

	fpath = cli->args.data_output;
	flags = XNVME_FILE_OFLG_CREATE | XNVME_FILE_OFLG_WRONLY;
	flags |= cli->given[XNVMEC_OPT_DIRECT] ? XNVME_FILE_OFLG_DIRECT_ON : 0x0;
	iosize = cli->given[XNVMEC_OPT_IOSIZE] ? cli->args.iosize : IOSIZE_DEF;
	qdepth = cli->given[XNVMEC_OPT_QDEPTH] ? cli->args.qdepth : QDEPTH_DEF;

	fh = xnvme_file_open(fpath, flags);
	if (fh == NULL) {
		xnvmec_perr("xnvme_file_open(fh)", errno);
		return errno;
	}
	tbytes = cli->args.data_nbytes;

	buf_nbytes = tbytes;
	buf = xnvme_buf_alloc(fh, buf_nbytes);
	if (!buf) {
		xnvmec_perr("xnvme_buf_alloc()", errno);
		goto exit;
	}
	xnvmec_buf_fill(buf, buf_nbytes, "anum");

	err = xnvme_queue_init(fh, qdepth, 0, &queue);
	if (err) {
		xnvmec_perr("xnvme_queue_init()", err);
		goto exit;
	}
	xnvme_queue_set_cb(queue, cb_func, &cb_args);

	xnvmec_pinf("dump-async{fpath: %s, tbytes: %zu, buf_nbytes: %zu, iosize: %zu, qdepth: %d}",
		    fpath, tbytes, buf_nbytes, iosize, qdepth);

	xnvmec_timer_start(cli);

	for (size_t ofz = 0; (ofz < tbytes) && !cb_args.nerrors;) {
		struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(queue);
		size_t nbytes = XNVME_MIN_U64(iosize, tbytes - ofz);
		ssize_t res;

submit:
		res = xnvme_file_pwrite(ctx, buf + ofz, nbytes, ofz);
		switch (res) {
		case 0:
			cb_args.nsubmissions += 1;
			goto next;

		case -EBUSY:
		case -EAGAIN:
			xnvme_queue_poke(queue, 0);
			goto submit;

		default:
			xnvmec_perr("submission-error", err);
			xnvme_queue_put_cmd_ctx(queue, ctx);
			goto exit;
		}

next:
		ofz += iosize;
	}

	err = xnvme_queue_wait(queue);
	if (err < 0) {
		xnvmec_perr("xnvme_queue_wait()", err);
		goto exit;
	}

	xnvmec_timer_stop(cli);
	xnvmec_timer_bw_pr(cli, "wall-clock", tbytes);

exit:
	xnvmec_pinf("cb_args: {nsubmissions: %zu, ncompletions: %zu, nerrors: %zu}",
		    cb_args.nsubmissions, cb_args.ncompletions, cb_args.nerrors);
	if (queue) {
		int err_exit = xnvme_queue_term(queue);
		if (err_exit) {
			xnvmec_perr("xnvme_queue_term()", err_exit);
		}
	}

	xnvme_buf_free(fh, buf);
	xnvme_file_close(fh);
	return 0;
}

int
load_sync(struct xnvmec *cli)
{
	const char *fpath;
	struct xnvme_dev *fh;
	int flags;
	size_t buf_nbytes, tbytes, iosize;
	char *buf;

	fpath = cli->args.data_input;
	flags = XNVME_FILE_OFLG_RDONLY;
	flags |= cli->given[XNVMEC_OPT_DIRECT] ? XNVME_FILE_OFLG_DIRECT_ON : 0x0;
	iosize = cli->given[XNVMEC_OPT_IOSIZE] ? cli->args.iosize : IOSIZE_DEF;

	fh = xnvme_file_open(fpath, flags);
	if (!fh) {
		xnvmec_perr("xnvme_file_open(fh)", errno);
		return errno;
	}
	tbytes = xnvme_dev_get_geo(fh)->tbytes;

	buf_nbytes = tbytes;
	buf = xnvme_buf_alloc(fh, buf_nbytes);
	if (!buf) {
		xnvmec_perr("xnvme_buf_alloc()", errno);
		goto exit;
	}
	xnvmec_buf_fill(buf, buf_nbytes, "zero");

	xnvmec_pinf("load-sync: {fpath: %s, tbytes: %zu, buf_nbytes: %zu iosize: %zu}",
		    fpath, tbytes, buf_nbytes, iosize);

	xnvmec_timer_start(cli);

	for (size_t ofz = 0; ofz < tbytes; ofz += iosize) {
		struct xnvme_cmd_ctx ctx = xnvme_file_get_cmd_ctx(fh);
		size_t nbytes = XNVME_MIN_U64(iosize, tbytes - ofz);
		ssize_t res;

		res = xnvme_file_pread(&ctx, buf + ofz, nbytes, ofz);
		if (res || xnvme_cmd_ctx_cpl_status(&ctx)) {
			xnvmec_perr("xnvme_file_pread(fh)", res);
			xnvme_cmd_ctx_pr(&ctx, XNVME_PR_DEF);
			goto exit;
		}
	}

	xnvmec_timer_stop(cli);
	xnvmec_timer_bw_pr(cli, "wall-clock", tbytes);

exit:
	xnvme_buf_free(fh, buf);
	xnvme_file_close(fh);

	return 0;
}

int
load_async(struct xnvmec *cli)
{
	const char *fpath;
	struct xnvme_dev *fh;
	int flags;
	size_t buf_nbytes, tbytes, iosize;
	char *buf;

	struct cb_args cb_args = { 0 };
	struct xnvme_queue *queue = NULL;
	uint32_t qdepth;
	int err;

	fpath = cli->args.data_input;
	flags = XNVME_FILE_OFLG_RDONLY;
	flags |= cli->given[XNVMEC_OPT_DIRECT] ? XNVME_FILE_OFLG_DIRECT_ON : 0x0;
	iosize = cli->given[XNVMEC_OPT_IOSIZE] ? cli->args.iosize : IOSIZE_DEF;
	qdepth = cli->given[XNVMEC_OPT_QDEPTH] ? cli->args.qdepth : QDEPTH_DEF;

	fh = xnvme_file_open(fpath, flags);
	if (fh == NULL) {
		xnvmec_perr("xnvme_file_open(fh)", errno);
		return errno;
	}
	tbytes = xnvme_dev_get_geo(fh)->tbytes;

	buf_nbytes = tbytes;
	buf = xnvme_buf_alloc(fh, buf_nbytes);
	if (!buf) {
		xnvmec_perr("xnvme_buf_alloc()", errno);
		goto exit;
	}
	xnvmec_buf_fill(buf, buf_nbytes, "zero");

	err = xnvme_queue_init(fh, qdepth, 0, &queue);
	if (err) {
		xnvmec_perr("xnvme_queue_init()", err);
		goto exit;
	}
	xnvme_queue_set_cb(queue, cb_func, &cb_args);

	xnvmec_pinf("load-async{fpath: %s, tbytes: %zu, buf_nbytes: %zu, iosize: %zu, qdepth: %d}",
		    fpath, tbytes, buf_nbytes, iosize, qdepth);

	xnvmec_timer_start(cli);

	for (size_t ofz = 0; (ofz < tbytes) && !cb_args.nerrors;) {
		struct xnvme_cmd_ctx *ctx = xnvme_queue_get_cmd_ctx(queue);
		size_t nbytes = XNVME_MIN_U64(iosize, tbytes - ofz);
		ssize_t res;

submit:
		res = xnvme_file_pread(ctx, buf + ofz, nbytes, ofz);
		switch (res) {
		case 0:
			cb_args.nsubmissions += 1;
			goto next;

		case -EBUSY:
		case -EAGAIN:
			xnvme_queue_poke(queue, 0);
			goto submit;

		default:
			xnvmec_perr("submission-error", err);
			xnvme_queue_put_cmd_ctx(queue, ctx);
			goto exit;
		}

next:
		ofz += iosize;
	}

	err = xnvme_queue_wait(queue);
	if (err < 0) {
		xnvmec_perr("xnvme_queue_wait()", err);
		goto exit;
	}

	xnvmec_timer_stop(cli);
	xnvmec_timer_bw_pr(cli, "wall-clock", tbytes);

exit:
	xnvmec_pinf("cb_args: {nsubmissions: %zu, ncompletions: %zu, nerrors: %zu}",
		    cb_args.nsubmissions, cb_args.ncompletions, cb_args.nerrors);
	if (queue) {
		int err_exit = xnvme_queue_term(queue);
		if (err_exit) {
			xnvmec_perr("xnvme_queue_term()", err_exit);
		}
	}

	xnvme_buf_free(fh, buf);
	xnvme_file_close(fh);

	return 0;
}

static struct xnvmec_sub g_subs[] = {
	{
		"write-read", "Write and read a file",
		"Write and read a file", read_write, {
			{XNVMEC_OPT_SYS_URI, XNVMEC_POSA},
			{XNVMEC_OPT_DATA_NBYTES, XNVMEC_POSA},
		}
	},
	{
		"dump-sync", "Write a buffer of 'data-nbytes' to file",
		"Write a buffer of 'data-nbytes' to file", dump_sync, {
			{XNVMEC_OPT_DATA_OUTPUT, XNVMEC_POSA},
			{XNVMEC_OPT_DATA_NBYTES, XNVMEC_LREQ},
			{XNVMEC_OPT_IOSIZE, XNVMEC_LOPT},
			{XNVMEC_OPT_DIRECT, XNVMEC_LFLG},
		}
	},
	{
		"dump-async", "Write a buffer of 'data-nbytes' to file --data-output",
		"Write a buffer of 'data-nbytes' to file --data-output", dump_async, {
			{XNVMEC_OPT_DATA_OUTPUT, XNVMEC_POSA},
			{XNVMEC_OPT_DATA_NBYTES, XNVMEC_LREQ},
			{XNVMEC_OPT_IOSIZE, XNVMEC_LOPT},
			{XNVMEC_OPT_QDEPTH, XNVMEC_LOPT},
			{XNVMEC_OPT_DIRECT, XNVMEC_LFLG},
		}
	},
	{
		"load-sync", "Read the entire file into memory",
		"Read the entire file into memory", load_sync, {
			{XNVMEC_OPT_DATA_INPUT, XNVMEC_POSA},
			{XNVMEC_OPT_IOSIZE, XNVMEC_LOPT},
			{XNVMEC_OPT_DIRECT, XNVMEC_LFLG},
		}
	},
	{
		"load-async", "Read the entire file into memory",
		"Read the entire file into memory", load_async, {
			{XNVMEC_OPT_DATA_INPUT, XNVMEC_POSA},
			{XNVMEC_OPT_IOSIZE, XNVMEC_LOPT},
			{XNVMEC_OPT_QDEPTH, XNVMEC_LOPT},
			{XNVMEC_OPT_DIRECT, XNVMEC_LFLG},
		}
	},
};

static struct xnvmec g_cli = {
	.title = "xNVMe file - Exercise the xnvme_file API",
	.descr_short = "Exercise the xnvme_file API",
	.descr_long = "",
	.subs = g_subs,
	.nsubs = sizeof g_subs / sizeof(*g_subs),
};


int
main(int argc, char **argv)
{
	return xnvmec(&g_cli, argc, argv, XNVMEC_INIT_NONE);
}
