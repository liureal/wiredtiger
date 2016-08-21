/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wtperf.h"

static CONFIG_OPT config_opts_desc[] = {	/* Option descriptions */
#define	OPT_DEFINE_DESC
#include "wtperf_opt.i"
#undef OPT_DEFINE_DESC
};

static CONFIG_OPTS config_opts_default = {	/* Option defaults */
#define	OPT_DEFINE_DEFAULT
#include "wtperf_opt.i"
#undef OPT_DEFINE_DEFAULT
};

/*
 * STRING_MATCH --
 *	Return if a string matches a bytestring of a specified length.
 */
#undef	STRING_MATCH
#define	STRING_MATCH(str, bytes, len)					\
	(strncmp(str, bytes, len) == 0 && (str)[(len)] == '\0')

/*
 * config_opt_init --
 *	Initialize the global configuration options.
 */
void
config_opt_init(CONFIG_OPTS **retp)
{
	CONFIG_OPT *desc;
	CONFIG_OPTS *opts;
	size_t i;
	char **strp;
	void *valueloc;

	opts = dmalloc(sizeof(CONFIG_OPTS));
	*opts = config_opts_default;

	/*
	 * Option strings come-and-go as we configure them, so allocate copies
	 * of the default strings now so that we can always free the string as
	 * we allocate new versions.
	 */
	for (i = 0, desc = config_opts_desc;
	    i < WT_ELEMENTS(config_opts_desc); i++, ++desc)
		if (desc->type == CONFIG_STRING_TYPE) {
			valueloc = ((uint8_t *)opts + desc->offset);
			strp = (char **)valueloc;
			*strp = dstrdup(*strp);
		}

	*retp = opts;
}

/*
 * config_unescape --
 *	Modify a string in place, replacing any backslash escape sequences.
 *	The modified string is always shorter.
 */
static int
config_unescape(char *orig)
{
	char ch, *dst, *s;

	for (dst = s = orig; *s != '\0';) {
		if ((ch = *s++) == '\\') {
			ch = *s++;
			switch (ch) {
			case 'b':
				*dst++ = '\b';
				break;
			case 'f':
				*dst++ = '\f';
				break;
			case 'n':
				*dst++ = '\n';
				break;
			case 'r':
				*dst++ = '\r';
				break;
			case 't':
				*dst++ = '\t';
				break;
			case '\\':
			case '/':
			case '\"':	/* Backslash needed for spell check. */
				*dst++ = ch;
				break;
			default:
				/* Note: Unicode (\u) not implemented. */
				fprintf(stderr,
				    "invalid escape in string: %s\n", orig);
				return (EINVAL);
			}
		} else
			*dst++ = ch;
	}
	*dst = '\0';
	return (0);
}

/*
 * config_threads --
 *	Parse the thread configuration.
 */
static int
config_threads(CONFIG *cfg, const char *config, size_t len)
{
	WORKLOAD *workp;
	WT_CONFIG_ITEM groupk, groupv, k, v;
	WT_CONFIG_PARSER *group, *scan;
	int ret;

	group = scan = NULL;
	if (cfg->workload != NULL) {
		/*
		 * This call overrides an earlier call.  Free and
		 * reset everything.
		 */
		free(cfg->workload);
		cfg->workload = NULL;
		cfg->workload_cnt = 0;
		cfg->workers_cnt = 0;
	}
	/* Allocate the workload array. */
	cfg->workload = dcalloc(WORKLOAD_MAX, sizeof(WORKLOAD));
	cfg->workload_cnt = 0;

	/*
	 * The thread configuration may be in multiple groups, that is, we have
	 * to handle configurations like:
	 *	threads=((count=2,reads=1),(count=8,inserts=2,updates=1))
	 *
	 * Start a scan on the original string, then do scans on each string
	 * returned from the original string.
	 */
	if ((ret =
	    wiredtiger_config_parser_open(NULL, config, len, &group)) != 0)
		goto err;
	while ((ret = group->next(group, &groupk, &groupv)) == 0) {
		if ((ret = wiredtiger_config_parser_open(
		    NULL, groupk.str, groupk.len, &scan)) != 0)
			goto err;

		/* Move to the next workload slot. */
		if (cfg->workload_cnt == WORKLOAD_MAX) {
			fprintf(stderr,
			    "too many workloads configured, only %d workloads "
			    "supported\n",
			    WORKLOAD_MAX);
			return (EINVAL);
		}
		workp = &cfg->workload[cfg->workload_cnt++];

		while ((ret = scan->next(scan, &k, &v)) == 0) {
			if (STRING_MATCH("count", k.str, k.len)) {
				if ((workp->threads = v.val) <= 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("insert", k.str, k.len) ||
			    STRING_MATCH("inserts", k.str, k.len)) {
				if ((workp->insert = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("ops_per_txn", k.str, k.len)) {
				if ((workp->ops_per_txn = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("read", k.str, k.len) ||
			    STRING_MATCH("reads", k.str, k.len)) {
				if ((workp->read = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("throttle", k.str, k.len)) {
				workp->throttle = (uint64_t)v.val;
				continue;
			}
			if (STRING_MATCH("truncate", k.str, k.len)) {
				if ((workp->truncate = v.val) != 1)
					goto err;
				/* There can only be one Truncate thread. */
				if (F_ISSET(cfg, CFG_TRUNCATE))
					goto err;
				F_SET(cfg, CFG_TRUNCATE);
				continue;
			}
			if (STRING_MATCH("truncate_pct", k.str, k.len)) {
				if (v.val <= 0)
					goto err;
				workp->truncate_pct = (uint64_t)v.val;
				continue;
			}
			if (STRING_MATCH("truncate_count", k.str, k.len)) {
				if (v.val <= 0)
					goto err;
				workp->truncate_count = (uint64_t)v.val;
				continue;
			}
			if (STRING_MATCH("update", k.str, k.len) ||
			    STRING_MATCH("updates", k.str, k.len)) {
				if ((workp->update = v.val) < 0)
					goto err;
				continue;
			}
			if (STRING_MATCH("update_delta", k.str, k.len)) {
				if (v.type == WT_CONFIG_ITEM_STRING ||
				    v.type == WT_CONFIG_ITEM_ID) {
					if (strncmp(v.str, "rand", 4) != 0)
						goto err;
					/* Special random value */
					workp->update_delta = INT64_MAX;
					F_SET(cfg, CFG_GROW);
				} else {
					workp->update_delta = v.val;
					if (v.val > 0)
						F_SET(cfg, CFG_GROW);
					if (v.val < 0)
						F_SET(cfg, CFG_SHRINK);
				}
				continue;
			}
			goto err;
		}
		if (ret == WT_NOTFOUND)
			ret = 0;
		if (ret != 0 )
			goto err;
		ret = scan->close(scan);
		scan = NULL;
		if (ret != 0)
			goto err;
		if (workp->insert == 0 && workp->read == 0 &&
		    workp->update == 0 && workp->truncate == 0)
			goto err;
		/* Why run with truncate if we don't want any truncation. */
		if (workp->truncate != 0 &&
		    workp->truncate_pct == 0 && workp->truncate_count == 0)
			goto err;
		if (workp->truncate != 0 &&
		    (workp->truncate_pct < 1 || workp->truncate_pct > 99))
			goto err;
		/* Truncate should have its own exclusive thread. */
		if (workp->truncate != 0 && workp->threads > 1)
			goto err;
		if (workp->truncate != 0 &&
		    (workp->insert > 0 || workp->read > 0 || workp->update > 0))
			goto err;
		cfg->workers_cnt += (u_int)workp->threads;
	}

	ret = group->close(group);
	group = NULL;
	if (ret != 0)
		goto err;

	return (0);

err:	if (group != NULL)
		testutil_check(group->close(group));
	if (scan != NULL)
		testutil_check(scan->close(scan));

	fprintf(stderr,
	    "invalid thread configuration or scan error: %.*s\n",
	    (int)len, config);
	return (EINVAL);
}

/*
 * config_opt --
 *	Check a single key=value returned by the config parser against our table
 * of valid keys, along with the expected type.  If everything is okay, set the
 * value.
 */
static int
config_opt(CONFIG *cfg, WT_CONFIG_ITEM *k, WT_CONFIG_ITEM *v)
{
	CONFIG_OPTS *opts;
	CONFIG_OPT *desc;
	char *begin, *newstr, **strp;
	int ret;
	size_t i, newlen;
	void *valueloc;

	opts = cfg->opts;

	desc = NULL;
	for (i = 0; i < WT_ELEMENTS(config_opts_desc); i++)
		if (strlen(config_opts_desc[i].name) == k->len &&
		    strncmp(config_opts_desc[i].name, k->str, k->len) == 0) {
			desc = &config_opts_desc[i];
			break;
		}
	if (desc == NULL) {
		fprintf(stderr, "wtperf: Error: "
		    "unknown option \'%.*s\'\n", (int)k->len, k->str);
		fprintf(stderr, "Options:\n");
		for (i = 0; i < WT_ELEMENTS(config_opts_desc); i++)
			fprintf(stderr, "\t%s\n", config_opts_desc[i].name);
		return (EINVAL);
	}
	valueloc = ((uint8_t *)opts + desc->offset);
	switch (desc->type) {
	case BOOL_TYPE:
		if (v->type != WT_CONFIG_ITEM_BOOL) {
			fprintf(stderr, "wtperf: Error: "
			    "bad bool value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case INT_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad int value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val > INT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "int value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(int *)valueloc = (int)v->val;
		break;
	case UINT32_TYPE:
		if (v->type != WT_CONFIG_ITEM_NUM) {
			fprintf(stderr, "wtperf: Error: "
			    "bad uint32 value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		if (v->val < 0 || v->val > UINT_MAX) {
			fprintf(stderr, "wtperf: Error: "
			    "uint32 value out of range for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		*(uint32_t *)valueloc = (uint32_t)v->val;
		break;
	case CONFIG_STRING_TYPE:
		/*
		 * Configuration parsing uses string/ID to distinguish
		 * between quoted and unquoted values.
		 */
		if (v->type != WT_CONFIG_ITEM_STRING &&
		    v->type != WT_CONFIG_ITEM_ID) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		newlen = v->len + 1;
		if (*strp == NULL)
			begin = newstr = dstrdup(v->str);
		else {
			newlen += strlen(*strp) + 1;
			newstr = dcalloc(newlen, sizeof(char));
			snprintf(newstr, newlen,
			    "%s,%*s", *strp, (int)v->len, v->str);
			/* Free the old value now we've copied it. */
			free(*strp);
			begin = &newstr[(newlen - 1) - v->len];
		}
		if ((ret = config_unescape(begin)) != 0) {
			free(newstr);
			return (ret);
		}
		*strp = newstr;
		break;
	case STRING_TYPE:
		/*
		 * Thread configuration is the one case where the type isn't a
		 * "string", it's a "struct".
		 */
		if (v->type == WT_CONFIG_ITEM_STRUCT &&
		    STRING_MATCH("threads", k->str, k->len))
			return (config_threads(cfg, v->str, v->len));

		if (v->type != WT_CONFIG_ITEM_STRING &&
		    v->type != WT_CONFIG_ITEM_ID) {
			fprintf(stderr, "wtperf: Error: "
			    "bad string value for \'%.*s=%.*s\'\n",
			    (int)k->len, k->str, (int)v->len, v->str);
			return (EINVAL);
		}
		strp = (char **)valueloc;
		free(*strp);
		/*
		 * We duplicate the string to len rather than len+1 as we want
		 * to truncate the trailing quotation mark.
		 */
		newstr = dstrndup(v->str,  v->len);
		*strp = newstr;
		break;
	}
	return (0);
}

/*
 * config_opt_file --
 *	Parse a configuration file.  We recognize comments '#' and continuation
 * via lines ending in '\'.
 */
int
config_opt_file(CONFIG *cfg, const char *filename)
{
	FILE *fp;
	size_t linelen, optionpos;
	int linenum, ret;
	bool contline;
	char line[4 * 1024], option[4 * 1024];
	char *comment, *ltrim, *rtrim;

	ret = 0;

	if ((fp = fopen(filename, "r")) == NULL) {
		fprintf(stderr, "wtperf: %s: %s\n", filename, strerror(errno));
		return (errno);
	}

	optionpos = 0;
	linenum = 0;
	while (fgets(line, sizeof(line), fp) != NULL) {
		linenum++;

		/* Skip leading space. */
		for (ltrim = line; *ltrim && isspace((u_char)*ltrim);
		     ltrim++)
			;

		/*
		 * Find the end of the line; if there's no trailing newline, the
		 * the line is too long for the buffer or the file was corrupted
		 * (there's no terminating newline in the file).
		 */
		for (rtrim = line; *rtrim && *rtrim != '\n'; rtrim++)
			;
		if (*rtrim != '\n') {
			fprintf(stderr,
			    "wtperf: %s: %d: configuration line too long\n",
			    filename, linenum);
			ret = EINVAL;
			break;
		}

		/* Skip trailing space. */
		while (rtrim > ltrim && isspace((u_char)rtrim[-1]))
			rtrim--;

		/*
		 * If the last non-space character in the line is an escape, the
		 * line will be continued. Checked early because the line might
		 * otherwise be empty.
		 */
		contline = rtrim > ltrim && rtrim[-1] == '\\';
		if (contline)
			rtrim--;

		/*
		 * Discard anything after the first hash character. Check after
		 * the escape character, the escape can appear after a comment.
		 */
		if ((comment = strchr(ltrim, '#')) != NULL)
			rtrim = comment;

		/* Skip trailing space again. */
		while (rtrim > ltrim && isspace((u_char)rtrim[-1]))
			rtrim--;

		/*
		 * Check for empty lines: note that the right-hand boundary can
		 * cross over the left-hand boundary, less-than or equal to is
		 * the correct test.
		 */
		if (rtrim <= ltrim) {
			/*
			 * If we're continuing from this line, or we haven't
			 * started building an option, ignore this line.
			 */
			if (contline || optionpos == 0)
				continue;

			/*
			 * An empty line terminating an option we're building;
			 * clean things up so we can proceed.
			 */
			linelen = 0;
		} else
			linelen = (size_t)(rtrim - ltrim);
		ltrim[linelen] = '\0';

		if (linelen + optionpos + 1 > sizeof(option)) {
			fprintf(stderr,
			    "wtperf: %s: %d: option value overflow\n",
			    filename, linenum);
			ret = EINVAL;
			break;
		}

		memcpy(&option[optionpos], ltrim, linelen);
		option[optionpos + linelen] = '\0';
		if (contline)
			optionpos += linelen;
		else {
			if ((ret = config_opt_str(cfg, option)) != 0) {
				fprintf(stderr, "wtperf: %s: %d: parse error\n",
				    filename, linenum);
				break;
			}
			optionpos = 0;
		}
	}
	if (ret == 0) {
		if (ferror(fp)) {
			fprintf(stderr, "wtperf: %s: read error\n", filename);
			ret = errno;
		}
		if (optionpos > 0) {
			fprintf(stderr, "wtperf: %s: %d: last line continues\n",
			    filename, linenum);
			ret = EINVAL;
		}
	}

	(void)fclose(fp);
	return (ret);
}

/*
 * config_opt_str --
 *	Parse a single line of config options.  Continued lines have already
 * been joined.
 */
int
config_opt_str(CONFIG *cfg, const char *optstr)
{
	CONFIG_QUEUE_ENTRY *config_line;
	WT_CONFIG_ITEM k, v;
	WT_CONFIG_PARSER *scan;
	size_t len;
	int ret, t_ret;

	len = strlen(optstr);
	if ((ret = wiredtiger_config_parser_open(
	    NULL, optstr, len, &scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_begin");
		return (ret);
	}

	/*
	 * Append the current line to our copy of the config. The config is
	 * stored in the order it is processed, so added options will be after
	 * any parsed from the original config. We allocate len + 1 to allow for
	 * a null byte to be added.
	 */
	config_line = dcalloc(sizeof(CONFIG_QUEUE_ENTRY), 1);
	config_line->string = dstrdup(optstr);
	TAILQ_INSERT_TAIL(&cfg->config_head, config_line, c);

	while (ret == 0) {
		if ((ret = scan->next(scan, &k, &v)) != 0) {
			/* Any parse error has already been reported. */
			if (ret == WT_NOTFOUND)
				ret = 0;
			break;
		}
		ret = config_opt(cfg, &k, &v);
	}
	if ((t_ret = scan->close(scan)) != 0) {
		lprintf(cfg, ret, 0, "Error in config_scan_end");
		if (ret == 0)
			ret = t_ret;
	}

	return (ret);
}

/*
 * config_opt_name_value --
 *	Set a name/value configuration pair.
 */
int
config_opt_name_value(CONFIG *cfg, const char *name, const char *value)
{
	int ret;
	char *optstr;

							/* name="value" */
	optstr = dmalloc(strlen(name) + strlen(value) + 4);
	sprintf(optstr, "%s=\"%s\"", name, value);
	ret = config_opt_str(cfg, optstr);
	free(optstr);
	return (ret);
}

/*
 * config_sanity --
 *	Configuration sanity checks.
 */
int
config_sanity(CONFIG *cfg)
{
	CONFIG_OPTS *opts;
	WORKLOAD *workp;
	u_int i;

	opts = cfg->opts;

	/* Various intervals should be less than the run-time. */
	if (opts->run_time > 0 &&
	    ((opts->checkpoint_threads != 0 &&
	    opts->checkpoint_interval > opts->run_time) ||
	    opts->report_interval > opts->run_time ||
	    opts->sample_interval > opts->run_time)) {
		fprintf(stderr, "interval value longer than the run-time\n");
		return (EINVAL);
	}
	/* The maximum is here to keep file name construction simple. */
	if (opts->table_count < 1 || opts->table_count > 99999) {
		fprintf(stderr,
		    "invalid table count, less than 1 or greater than 99999\n");
		return (EINVAL);
	}
	if (opts->database_count < 1 || opts->database_count > 99) {
		fprintf(stderr,
		    "invalid database count, less than 1 or greater than 99\n");
		return (EINVAL);
	}

	if (opts->pareto > 100) {
		fprintf(stderr,
		    "Invalid pareto distribution - should be a percentage\n");
		return (EINVAL);
	}

	if (opts->value_sz_max < opts->value_sz) {
		if (F_ISSET(cfg, CFG_GROW)) {
			fprintf(stderr, "value_sz_max %" PRIu32
			    " must be greater than or equal to value_sz %"
			    PRIu32 "\n", opts->value_sz_max, opts->value_sz);
			return (EINVAL);
		} else
			opts->value_sz_max = opts->value_sz;
	}
	if (opts->value_sz_min > opts->value_sz) {
		if (F_ISSET(cfg, CFG_SHRINK)) {
			fprintf(stderr, "value_sz_min %" PRIu32
			    " must be less than or equal to value_sz %"
			    PRIu32 "\n", opts->value_sz_min, opts->value_sz);
			return (EINVAL);
		} else
			opts->value_sz_min = opts->value_sz;
	}

	if (opts->readonly && cfg->workload != NULL)
		for (i = 0, workp = cfg->workload;
		    i < cfg->workload_cnt; ++i, ++workp)
			if (workp->insert != 0 || workp->update != 0 ||
			    workp->truncate != 0) {
				fprintf(stderr,
				    "Invalid workload: insert, update or "
				    "truncate specified with readonly\n");
				return (EINVAL);
			}
	return (0);
}

/*
 * config_consolidate --
 *	Consolidate repeated configuration settings so that it only appears
 *	once in the configuration output file.
 */
static void
config_consolidate(CONFIG *cfg)
{
	CONFIG_QUEUE_ENTRY *conf_line, *test_line, *tmp;
	char *string_key;

	/* 
	 * This loop iterates over the config queue and for each entry checks if
	 * a later queue entry has the same key. If there's a match, the current
	 * queue entry is removed and we continue.
	 */
	conf_line = TAILQ_FIRST(&cfg->config_head);
	while (conf_line != NULL) {
		string_key = strchr(conf_line->string, '=');
		tmp = test_line = TAILQ_NEXT(conf_line, c);
		while (test_line != NULL) {
			/*
			 * The + 1 here forces the '=' sign to be matched
			 * ensuring we don't match keys that have a common
			 * prefix such as "table_count" and "table_count_idle"
			 * as being the same key.
			 */
			if (strncmp(conf_line->string, test_line->string,
			    (size_t)((string_key - conf_line->string) + 1))
			    == 0) {
				TAILQ_REMOVE(&cfg->config_head, conf_line, c);
				free(conf_line->string);
				free(conf_line);
				break;
			}
			test_line = TAILQ_NEXT(test_line, c);
		}
		conf_line = tmp;
	}
}

/*
 * config_opt_log --
 *	Write the final config used in this execution to a file.
 */
void
config_opt_log(CONFIG *cfg, const char *path)
{
	CONFIG_QUEUE_ENTRY *config_line;
	FILE *fp;

	testutil_checkfmt(((fp = fopen(path, "w")) == NULL), "%s", path);

	config_consolidate(cfg);

	fprintf(fp,"# Warning: This config includes "
	    "unwritten, implicit configuration defaults.\n"
	    "# Changes to those values may cause differences in behavior.\n");
	TAILQ_FOREACH(config_line, &cfg->config_head, c)
		fprintf(fp, "%s\n", config_line->string);
	testutil_check(fclose(fp));
}

/*
 * config_opt_print --
 *	Print out the configuration in verbose mode.
 */
void
config_opt_print(CONFIG *cfg)
{
	CONFIG_OPTS *opts;
	WORKLOAD *workp;
	u_int i;

	opts = cfg->opts;

	printf("Workload configuration:\n");
	printf("\t" "Home: %s\n", cfg->home);
	printf("\t" "Table name: %s\n", opts->table_name);
	printf("\t" "Connection configuration: %s\n", opts->conn_config);
	if (opts->sess_config != NULL)
		printf("\t" "Session configuration: %s\n", opts->sess_config);

	printf("\t%s table: %s\n",
	    opts->create ? "Creating new" : "Using existing",
	    opts->table_config);
	printf("\t" "Key size: %" PRIu32 ", value size: %" PRIu32 "\n",
	    opts->key_sz, opts->value_sz);
	if (opts->create)
		printf("\t" "Populate threads: %" PRIu32 ", inserting %" PRIu32
		    " rows\n",
		    opts->populate_threads, opts->icount);

	printf("\t" "Workload seconds, operations: %" PRIu32 ", %" PRIu32 "\n",
	    opts->run_time, opts->run_ops);
	if (cfg->workload != NULL) {
		printf("\t" "Workload configuration(s):\n");
		for (i = 0, workp = cfg->workload;
		    i < cfg->workload_cnt; ++i, ++workp)
			printf("\t\t%" PRId64 " threads (inserts=%" PRId64
			    ", reads=%" PRId64 ", updates=%" PRId64
			    ", truncates=% " PRId64 ")\n",
			    workp->threads,
			    workp->insert, workp->read,
			    workp->update, workp->truncate);
	}

	printf("\t" "Checkpoint threads, interval: %" PRIu32 ", %" PRIu32 "\n",
	    opts->checkpoint_threads, opts->checkpoint_interval);
	printf("\t" "Reporting interval: %" PRIu32 "\n", opts->report_interval);
	printf("\t" "Sampling interval: %" PRIu32 "\n", opts->sample_interval);

	printf("\t" "Verbosity: %" PRIu32 "\n", opts->verbose);
}

/*
 * pretty_print --
 *	Print out lines of text for a 80 character window.
 */
static void
pretty_print(const char *p, const char *indent)
{
	const char *t;

	for (;; p = t + 1) {
		if (strlen(p) <= 70)
			break;
		for (t = p + 70; t > p && *t != ' '; --t)
			;
		if (t == p)			/* No spaces? */
			break;
		printf("%s%.*s\n",
		    indent == NULL ? "" : indent, (int)(t - p), p);
	}
	if (*p != '\0')
		printf("%s%s\n", indent == NULL ? "" : indent, p);
}

/*
 * config_opt_usage --
 *	Configuration usage error message.
 */
void
config_opt_usage(void)
{
	size_t i;
	const char *defaultval, *typestr;

	pretty_print(
	    "The following are options settable using -o or -O, showing the "
	    "type and default value.\n", NULL);
	pretty_print(
	    "String values must be enclosed in \" quotes, boolean values must "
	    "be either true or false.\n", NULL);

	for (i = 0; i < WT_ELEMENTS(config_opts_desc); i++) {
		defaultval = config_opts_desc[i].defaultval;
		typestr = "string";
		switch (config_opts_desc[i].type) {
		case BOOL_TYPE:
			typestr = "boolean";
			if (strcmp(defaultval, "0") == 0)
				defaultval = "false";
			else
				defaultval = "true";
			break;
		case CONFIG_STRING_TYPE:
		case STRING_TYPE:
			break;
		case INT_TYPE:
			typestr = "int";
			break;
		case UINT32_TYPE:
			typestr = "unsigned int";
			break;
		}
		printf("%s (%s, default=%s)\n",
		    config_opts_desc[i].name, typestr, defaultval);
		pretty_print(config_opts_desc[i].description, "\t");
	}
}
