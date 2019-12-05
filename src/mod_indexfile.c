#include "first.h"

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "http_header.h"

#include "plugin.h"

#include "stat_cache.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* plugin config for all request/connections */

typedef struct {
    const array *indexfiles;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config defaults;
    plugin_config conf;
} plugin_data;

/* init the plugin data */
INIT_FUNC(mod_indexfile_init) {
    return calloc(1, sizeof(plugin_data));
}

static void mod_indexfile_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* index-file.names */
      case 1: /* server.indexfiles */
        pconf->indexfiles = cpv->v.a;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_indexfile_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_indexfile_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_indexfile_patch_config(connection * const con, plugin_data * const p) {
    memcpy(&p->conf, &p->defaults, sizeof(plugin_config));
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(con, (uint32_t)p->cvlist[i].k_id))
            mod_indexfile_merge_config(&p->conf,p->cvlist+p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_indexfile_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("index-file.names"),
        T_CONFIG_ARRAY,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("server.indexfiles"),
        T_CONFIG_ARRAY,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_indexfile"))
        return HANDLER_ERROR;

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 0: /* index-file.names */
              case 1: /* server.indexfiles */
                if (!array_is_vlist(cpv->v.a)) {
                    log_error(srv->errh, __FILE__, __LINE__,
                      "unexpected value for %s; "
                      "expected list of \"file\"", cpk[cpv->k_id].k);
                    return HANDLER_ERROR;
                }
                break;
              default:/* should not happen */
                break;
            }
        }
    }

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_indexfile_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

URIHANDLER_FUNC(mod_indexfile_subrequest) {
	plugin_data *p = p_d;

	if (con->mode != DIRECT) return HANDLER_GO_ON;

	if (buffer_is_empty(con->uri.path)) return HANDLER_GO_ON;
	if (con->uri.path->ptr[buffer_string_length(con->uri.path) - 1] != '/') return HANDLER_GO_ON;

	mod_indexfile_patch_config(con, p);
	if (NULL == p->conf.indexfiles) return HANDLER_GO_ON;

	if (con->conf.log_request_handling) {
		log_error(con->conf.errh, __FILE__, __LINE__, "-- handling the request as Indexfile");
		log_error(con->conf.errh, __FILE__, __LINE__, "URI          : %s", con->uri.path->ptr);
	}

	/* indexfile */
	buffer * const b = con->srv->tmp_buf;
	for (uint32_t k = 0; k < p->conf.indexfiles->used; ++k) {
		const data_string * const ds = (data_string *)p->conf.indexfiles->data[k];

		if (ds->value.ptr[0] == '/') {
			/* if the index-file starts with a prefix as use this file as
			 * index-generator */
			buffer_copy_buffer(b, con->physical.doc_root);
		} else {
			buffer_copy_buffer(b, con->physical.path);
		}
		buffer_append_string_buffer(b, &ds->value);

		stat_cache_entry * const sce = stat_cache_get_entry(b);
		if (NULL == sce) {
			if (errno == EACCES) {
				con->http_status = 403;
				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			}

			if (errno != ENOENT &&
			    errno != ENOTDIR) {
				/* we have no idea what happend. let's tell the user so. */
				con->http_status = 500;
				log_perror(con->conf.errh, __FILE__, __LINE__,
				  "file not found ... or so: %s -> %s",
				  con->uri.path->ptr, con->physical.path->ptr);
				buffer_reset(con->physical.path);
				return HANDLER_FINISHED;
			}
			continue;
		}

		if (ds->value.ptr[0] == '/') {
			/* replace uri.path */
			buffer_copy_buffer(con->uri.path, &ds->value);
			http_header_env_set(con, CONST_STR_LEN("PATH_TRANSLATED_DIRINDEX"), CONST_BUF_LEN(con->physical.path));
		} else {
			/* append to uri.path the relative path to index file (/ -> /index.php) */
			buffer_append_string_buffer(con->uri.path, &ds->value);
		}

		buffer_copy_buffer(con->physical.path, b);
		return HANDLER_GO_ON;
	}

	/* not found */
	return HANDLER_GO_ON;
}


int mod_indexfile_plugin_init(plugin *p);
int mod_indexfile_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = "indexfile";

	p->init        = mod_indexfile_init;
	p->handle_subrequest_start = mod_indexfile_subrequest;
	p->set_defaults  = mod_indexfile_set_defaults;

	return 0;
}
