/* fstatat() fdopendir() */
#if !defined(_XOPEN_SOURCE) || _XOPEN_SOURCE-0 < 700
#undef  _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
/* NetBSD dirent.h improperly hides fdopendir() (POSIX.1-2008) declaration
 * which should be visible with _XOPEN_SOURCE 700 or _POSIX_C_SOURCE 200809L */
#ifdef __NetBSD__
#define _NETBSD_SOURCE
#endif
#endif

#include "first.h"

#include "sys-time.h"

#include "base.h"
#include "log.h"
#include "buffer.h"
#include "fdevent.h"
#include "http_chunk.h"
#include "http_header.h"
#include "keyvalue.h"
#include "response.h"

#include "plugin.h"

#include "stat_cache.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef AT_FDCWD
#ifndef _ATFILE_SOURCE
#define _ATFILE_SOURCE
#endif
#endif

#ifndef _D_EXACT_NAMLEN
#ifdef _DIRENT_HAVE_D_NAMLEN
#define _D_EXACT_NAMLEN(d) ((d)->d_namlen)
#else
#define _D_EXACT_NAMLEN(d) (strlen ((d)->d_name))
#endif
#endif

/**
 * this is a dirlisting for a lighttpd plugin
 */

typedef struct {
	char dir_listing;
	char hide_dot_files;
	char hide_readme_file;
	char encode_readme;
	char hide_header_file;
	char encode_header;
	char auto_layout;

	pcre_keyvalue_buffer *excludes;

	const buffer *show_readme;
	const buffer *show_header;
	const buffer *external_css;
	const buffer *external_js;
	const buffer *encoding;
	const buffer *set_footer;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	plugin_config defaults;
	plugin_config conf;
} plugin_data;

static pcre_keyvalue_buffer * mod_dirlisting_parse_excludes(server *srv, const array *a) {
    const int pcre_jit =
      !srv->srvconf.feature_flags
      || config_plugin_value_tobool(
          array_get_element_klen(srv->srvconf.feature_flags,
                                 CONST_STR_LEN("server.pcre_jit")), 1);
    pcre_keyvalue_buffer * const kvb = pcre_keyvalue_buffer_init();
    buffer empty = { NULL, 0, 0 };
    for (uint32_t j = 0; j < a->used; ++j) {
        const data_string *ds = (data_string *)a->data[j];
        if (!pcre_keyvalue_buffer_append(srv->errh, kvb, &ds->value, &empty,
                                         pcre_jit)) {
            log_error(srv->errh, __FILE__, __LINE__,
              "pcre_compile failed for %s", ds->key.ptr);
            pcre_keyvalue_buffer_free(kvb);
            return NULL;
        }
    }
    return kvb;
}

static int mod_dirlisting_exclude(pcre_keyvalue_buffer * const kvb, const char * const name, const uint32_t len) {
    /*(re-use keyvalue.[ch] for match-only;
     *  must have been configured with empty kvb 'value' during init)*/
    buffer input = { NULL, len+1, 0 };
    *(const char **)&input.ptr = name;
    pcre_keyvalue_ctx ctx = { NULL, NULL, 0, -1 };
    /*(fail closed (simulate match to exclude) if there is an error)*/
    return HANDLER_ERROR == pcre_keyvalue_buffer_process(kvb,&ctx,&input,NULL)
        || -1 != ctx.m;
}


INIT_FUNC(mod_dirlisting_init) {
    return calloc(1, sizeof(plugin_data));
}

FREE_FUNC(mod_dirlisting_free) {
    plugin_data * const p = p_d;
    if (NULL == p->cvlist) return;
    /* (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1], used = p->nconfig; i < used; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 2: /* dir-listing.exclude */
                if (cpv->vtype != T_CONFIG_LOCAL) continue;
                pcre_keyvalue_buffer_free(cpv->v.v);
                break;
              default:
                break;
            }
        }
    }
}

static void mod_dirlisting_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* dir-listing.activate */
      case 1: /* server.dir-listing *//*(historical)*/
        pconf->dir_listing = (char)cpv->v.u;
        break;
      case 2: /* dir-listing.exclude */
        if (cpv->vtype == T_CONFIG_LOCAL)
            pconf->excludes = cpv->v.v;
        break;
      case 3: /* dir-listing.hide-dotfiles */
        pconf->hide_dot_files = (char)cpv->v.u;
        break;
      case 4: /* dir-listing.external-css */
        pconf->external_css = cpv->v.b;
        break;
      case 5: /* dir-listing.external-js */
        pconf->external_js = cpv->v.b;
        break;
      case 6: /* dir-listing.encoding */
        pconf->encoding = cpv->v.b;
        break;
      case 7: /* dir-listing.show-readme */
        pconf->show_readme = cpv->v.b;
        break;
      case 8: /* dir-listing.hide-readme-file */
        pconf->hide_readme_file = (char)cpv->v.u;
        break;
      case 9: /* dir-listing.show-header */
        pconf->show_header = cpv->v.b;
        break;
      case 10:/* dir-listing.hide-header-file */
        pconf->hide_header_file = (char)cpv->v.u;
        break;
      case 11:/* dir-listing.set-footer */
        pconf->set_footer = cpv->v.b;
        break;
      case 12:/* dir-listing.encode-readme */
        pconf->encode_readme = (char)cpv->v.u;
        break;
      case 13:/* dir-listing.encode-header */
        pconf->encode_header = (char)cpv->v.u;
        break;
      case 14:/* dir-listing.auto-layout */
        pconf->auto_layout = (char)cpv->v.u;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_dirlisting_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_dirlisting_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_dirlisting_patch_config(request_st * const r, plugin_data * const p) {
    memcpy(&p->conf, &p->defaults, sizeof(plugin_config));
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_dirlisting_merge_config(&p->conf, p->cvlist + p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_dirlisting_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("dir-listing.activate"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("server.dir-listing"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.exclude"),
        T_CONFIG_ARRAY_VLIST,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-dotfiles"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.external-css"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.external-js"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encoding"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.show-readme"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-readme-file"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.show-header"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.hide-header-file"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.set-footer"),
        T_CONFIG_STRING,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encode-readme"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.encode-header"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ CONST_STR_LEN("dir-listing.auto-layout"),
        T_CONFIG_BOOL,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_dirlisting"))
        return HANDLER_ERROR;

    /* process and validate config directives
     * (init i to 0 if global context; to 1 to skip empty global context) */
    for (int i = !p->cvlist[0].v.u2[1]; i < p->nconfig; ++i) {
        config_plugin_value_t *cpv = p->cvlist + p->cvlist[i].v.u2[0];
        for (; -1 != cpv->k_id; ++cpv) {
            switch (cpv->k_id) {
              case 0: /* dir-listing.activate */
              case 1: /* server.dir-listing *//*(historical)*/
                break;
              case 2: /* dir-listing.exclude */
                cpv->v.v = mod_dirlisting_parse_excludes(srv, cpv->v.a);
                if (NULL == cpv->v.v) return HANDLER_ERROR;
                cpv->vtype = T_CONFIG_LOCAL;
                break;
              case 3: /* dir-listing.hide-dotfiles */
              case 4: /* dir-listing.external-css */
              case 5: /* dir-listing.external-js */
              case 6: /* dir-listing.encoding */
                break;
              case 7: /* dir-listing.show-readme */
                if (!buffer_string_is_empty(cpv->v.b)) {
                    buffer *b;
                    *(const buffer **)&b = cpv->v.b;
                    if (buffer_is_equal_string(b, CONST_STR_LEN("enable")))
                        buffer_copy_string_len(b, CONST_STR_LEN("README.txt"));
                    else if (buffer_is_equal_string(b,CONST_STR_LEN("disable")))
                        buffer_clear(b);
                }
                break;
              case 8: /* dir-listing.hide-readme-file */
                break;
              case 9: /* dir-listing.show-header */
                if (!buffer_string_is_empty(cpv->v.b)) {
                    buffer *b;
                    *(const buffer **)&b = cpv->v.b;
                    if (buffer_is_equal_string(b, CONST_STR_LEN("enable")))
                        buffer_copy_string_len(b, CONST_STR_LEN("HEADER.txt"));
                    else if (buffer_is_equal_string(b,CONST_STR_LEN("disable")))
                        buffer_clear(b);
                }
                break;
              case 10:/* dir-listing.hide-header-file */
              case 11:/* dir-listing.set-footer */
              case 12:/* dir-listing.encode-readme */
              case 13:/* dir-listing.encode-header */
              case 14:/* dir-listing.auto-layout */
                break;
              default:/* should not happen */
                break;
            }
        }
    }

    p->defaults.dir_listing = 0;
    p->defaults.hide_dot_files = 1;
    p->defaults.hide_readme_file = 0;
    p->defaults.hide_header_file = 0;
    p->defaults.encode_readme = 1;
    p->defaults.encode_header = 1;
    p->defaults.auto_layout = 1;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_dirlisting_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

typedef struct {
	uint32_t namelen;
	time_t  mtime;
	off_t   size;
} dirls_entry_t;

typedef struct {
	dirls_entry_t **ent;
	uint32_t used;
	uint32_t size;
} dirls_list_t;

#define DIRLIST_ENT_NAME(ent)	((char*)(ent) + sizeof(dirls_entry_t))
#define DIRLIST_BLOB_SIZE		16

/* simple combsort algorithm */
static void http_dirls_sort(dirls_entry_t **ent, int num) {
	int gap = num;
	int i, j;
	int swapped;
	dirls_entry_t *tmp;

	do {
		gap = (gap * 10) / 13;
		if (gap == 9 || gap == 10)
			gap = 11;
		if (gap < 1)
			gap = 1;
		swapped = 0;

		for (i = 0; i < num - gap; i++) {
			j = i + gap;
			if (strcmp(DIRLIST_ENT_NAME(ent[i]), DIRLIST_ENT_NAME(ent[j])) > 0) {
				tmp = ent[i];
				ent[i] = ent[j];
				ent[j] = tmp;
				swapped = 1;
			}
		}

	} while (gap > 1 || swapped);
}

/* buffer must be able to hold "999.9K"
 * conversion is simple but not perfect
 */
static size_t http_list_directory_sizefmt(char *buf, size_t bufsz, off_t size) {
	int remain;
	int u = -1;  /* u will always increment at least once */
	size_t buflen;

	if (0 < size && size < 100)
		size += 99;

	do {
		remain = (int)(size & 1023);
		size >>= 10;
		++u;
	} while (size & ~1023);

	remain /= 100;
	if (remain > 9)
		remain = 9;
	if (size > 999) {
		size   = 0;
		remain = 9;
		u++;
	}

	buflen = li_itostrn(buf, bufsz, size);
	if (buflen + 3 >= bufsz) return buflen;
	buf[buflen+0] = '.';
	buf[buflen+1] = remain + '0';
	buf[buflen+2] = "KMGTPE"[u];  /* Kilo, Mega, Giga, Tera, Peta, Exa */
	buf[buflen+3] = '\0';

	return buflen + 3;
}

static void http_list_directory_include_file(request_st * const r, plugin_data * const p, int is_header) {
    const buffer *path;
    int encode = 0;
    if (is_header) {
        path = p->conf.show_header;
        encode = p->conf.encode_header;
    }
    else {
        path = p->conf.show_readme;
        encode = p->conf.encode_readme;
    }

    uint32_t len = 0;
    if (path->ptr[0] != '/') { /* temporarily extend r->physical.path */
        len = buffer_string_length(&r->physical.path);
        buffer_append_path_len(&r->physical.path, CONST_BUF_LEN(path));
        path = &r->physical.path;
    }
    stat_cache_entry * const sce =
      stat_cache_get_entry_open(path, r->conf.follow_symlink);
    if (len)
        buffer_string_set_length(&r->physical.path, len);
    if (NULL == sce || sce->fd < 0 || 0 != sce->st.st_size)
        return;

    chunkqueue * const cq = &r->write_queue;
    if (encode) {
        if (is_header)
            chunkqueue_append_mem(cq, CONST_STR_LEN("<pre class=\"header\">"));
        else
            chunkqueue_append_mem(cq, CONST_STR_LEN("<pre class=\"readme\">"));

        /* Note: encoding a very large file may cause lighttpd to pause handling
         * other requests while lighttpd encodes the file, especially if file is
         * on a remote filesystem */

        /* encoding can consume 6x file size in worst case scenario,
         * so send encoded contents of files > 32k to tempfiles) */
        buffer * const tb = r->tmp_buf;
        buffer * const out = sce->st.st_size <= 32768
          ? chunkqueue_append_buffer_open(cq)
          : tb;
        buffer_clear(out);
        const int fd = sce->fd;
        ssize_t rd;
        char buf[8192];
        while ((rd = read(fd, buf, sizeof(buf))) > 0) {
            buffer_append_string_encoded(out, buf, (size_t)rd, ENCODING_MINIMAL_XML);
            if (out == tb) {
                if (0 != chunkqueue_append_mem_to_tempfile(cq,
                                                           CONST_BUF_LEN(out),
                                                           r->conf.errh))
                    break;
                buffer_clear(out);
            }
        }
        if (out != tb)
            chunkqueue_append_buffer_commit(cq);

        chunkqueue_append_mem(cq, CONST_STR_LEN("</pre>"));
    }
    else {
        http_chunk_append_file_ref(r, sce);
    }
}

/* portions copied from mod_status
 * modified and specialized for stable dirlist sorting by name */
static const char js_simple_table_resort[] = \
"var click_column;\n" \
"var name_column = 0;\n" \
"var date_column = 1;\n" \
"var size_column = 2;\n" \
"var type_column = 3;\n" \
"var prev_span = null;\n" \
"\n" \
"if (typeof(String.prototype.localeCompare) === 'undefined') {\n" \
" String.prototype.localeCompare = function(str, locale, options) {\n" \
"   return ((this == str) ? 0 : ((this > str) ? 1 : -1));\n" \
" };\n" \
"}\n" \
"\n" \
"if (typeof(String.prototype.toLocaleUpperCase) === 'undefined') {\n" \
" String.prototype.toLocaleUpperCase = function() {\n" \
"  return this.toUpperCase();\n" \
" };\n" \
"}\n" \
"\n" \
"function get_inner_text(el) {\n" \
" if((typeof el == 'string')||(typeof el == 'undefined'))\n" \
"  return el;\n" \
" if(el.innerText)\n" \
"  return el.innerText;\n" \
" else {\n" \
"  var str = \"\";\n" \
"  var cs = el.childNodes;\n" \
"  var l = cs.length;\n" \
"  for (i=0;i<l;i++) {\n" \
"   if (cs[i].nodeType==1) str += get_inner_text(cs[i]);\n" \
"   else if (cs[i].nodeType==3) str += cs[i].nodeValue;\n" \
"  }\n" \
" }\n" \
" return str;\n" \
"}\n" \
"\n" \
"function isdigit(c) {\n" \
" return (c >= '0' && c <= '9');\n" \
"}\n" \
"\n" \
"function unit_multiplier(unit) {\n" \
" return (unit=='K') ? 1000\n" \
"      : (unit=='M') ? 1000000\n" \
"      : (unit=='G') ? 1000000000\n" \
"      : (unit=='T') ? 1000000000000\n" \
"      : (unit=='P') ? 1000000000000000\n" \
"      : (unit=='E') ? 1000000000000000000 : 1;\n" \
"}\n" \
"\n" \
"var li_date_regex=/(\\d{4})-(\\w{3})-(\\d{2}) (\\d{2}):(\\d{2}):(\\d{2})/;\n" \
"\n" \
"var li_mon = ['Jan','Feb','Mar','Apr','May','Jun',\n" \
"              'Jul','Aug','Sep','Oct','Nov','Dec'];\n" \
"\n" \
"function li_mon_num(mon) {\n" \
" var i; for (i = 0; i < 12 && mon != li_mon[i]; ++i); return i;\n" \
"}\n" \
"\n" \
"function li_date_cmp(s1, s2) {\n" \
" var dp1 = li_date_regex.exec(s1)\n" \
" var dp2 = li_date_regex.exec(s2)\n" \
" for (var i = 1; i < 7; ++i) {\n" \
"  var cmp = (2 != i)\n" \
"   ? parseInt(dp1[i]) - parseInt(dp2[i])\n" \
"   : li_mon_num(dp1[2]) - li_mon_num(dp2[2]);\n" \
"  if (0 != cmp) return cmp;\n" \
" }\n" \
" return 0;\n" \
"}\n" \
"\n" \
"function sortfn_then_by_name(a,b,sort_column) {\n" \
" if (sort_column == name_column || sort_column == type_column) {\n" \
"  var ad = (a.cells[type_column].innerHTML === 'Directory');\n" \
"  var bd = (b.cells[type_column].innerHTML === 'Directory');\n" \
"  if (ad != bd) return (ad ? -1 : 1);\n" \
" }\n" \
" var at = get_inner_text(a.cells[sort_column]);\n" \
" var bt = get_inner_text(b.cells[sort_column]);\n" \
" var cmp;\n" \
" if (sort_column == name_column) {\n" \
"  if (at == '..') return -1;\n" \
"  if (bt == '..') return  1;\n" \
" }\n" \
" if (a.cells[sort_column].className == 'int') {\n" \
"  cmp = parseInt(at)-parseInt(bt);\n" \
" } else if (sort_column == date_column) {\n" \
"  var ad = isdigit(at.substr(0,1));\n" \
"  var bd = isdigit(bt.substr(0,1));\n" \
"  if (ad != bd) return (!ad ? -1 : 1);\n" \
"  cmp = li_date_cmp(at,bt);\n" \
" } else if (sort_column == size_column) {\n" \
"  var ai = parseInt(at, 10) * unit_multiplier(at.substr(-1,1));\n" \
"  var bi = parseInt(bt, 10) * unit_multiplier(bt.substr(-1,1));\n" \
"  if (at.substr(0,1) == '-') ai = -1;\n" \
"  if (bt.substr(0,1) == '-') bi = -1;\n" \
"  cmp = ai - bi;\n" \
" } else {\n" \
"  cmp = at.toLocaleUpperCase().localeCompare(bt.toLocaleUpperCase());\n" \
"  if (0 != cmp) return cmp;\n" \
"  cmp = at.localeCompare(bt);\n" \
" }\n" \
" if (0 != cmp || sort_column == name_column) return cmp;\n" \
" return sortfn_then_by_name(a,b,name_column);\n" \
"}\n" \
"\n" \
"function sortfn(a,b) {\n" \
" return sortfn_then_by_name(a,b,click_column);\n" \
"}\n" \
"\n" \
"function resort(lnk) {\n" \
" var span = lnk.childNodes[1];\n" \
" var table = lnk.parentNode.parentNode.parentNode.parentNode;\n" \
" var rows = new Array();\n" \
" for (j=1;j<table.rows.length;j++)\n" \
"  rows[j-1] = table.rows[j];\n" \
" click_column = lnk.parentNode.cellIndex;\n" \
" rows.sort(sortfn);\n" \
"\n" \
" if (prev_span != null) prev_span.innerHTML = '';\n" \
" if (span.getAttribute('sortdir')=='down') {\n" \
"  span.innerHTML = '&uarr;';\n" \
"  span.setAttribute('sortdir','up');\n" \
"  rows.reverse();\n" \
" } else {\n" \
"  span.innerHTML = '&darr;';\n" \
"  span.setAttribute('sortdir','down');\n" \
" }\n" \
" for (i=0;i<rows.length;i++)\n" \
"  table.tBodies[0].appendChild(rows[i]);\n" \
" prev_span = span;\n" \
"}\n";

/* portions copied from mod_dirlist (lighttpd2) */
static const char js_simple_table_init_sort[] = \
"\n" \
"function init_sort(init_sort_column, ascending) {\n" \
" var tables = document.getElementsByTagName(\"table\");\n" \
" for (var i = 0; i < tables.length; i++) {\n" \
"  var table = tables[i];\n" \
"  //var c = table.getAttribute(\"class\")\n" \
"  //if (-1 != c.split(\" \").indexOf(\"sort\")) {\n" \
"   var row = table.rows[0].cells;\n" \
"   for (var j = 0; j < row.length; j++) {\n" \
"    var n = row[j];\n" \
"    if (n.childNodes.length == 1 && n.childNodes[0].nodeType == 3) {\n" \
"     var link = document.createElement(\"a\");\n" \
"     var title = n.childNodes[0].nodeValue.replace(/:$/, \"\");\n" \
"     link.appendChild(document.createTextNode(title));\n" \
"     link.setAttribute(\"href\", \"#\");\n" \
"     link.setAttribute(\"class\", \"sortheader\");\n" \
"     link.setAttribute(\"onclick\", \"resort(this);return false;\");\n" \
"     var arrow = document.createElement(\"span\");\n" \
"     arrow.setAttribute(\"class\", \"sortarrow\");\n" \
"     arrow.appendChild(document.createTextNode(\":\"));\n" \
"     link.appendChild(arrow)\n" \
"     n.replaceChild(link, n.firstChild);\n" \
"    }\n" \
"   }\n" \
"   var lnk = row[init_sort_column].firstChild;\n" \
"   if (ascending) {\n" \
"    var span = lnk.childNodes[1];\n" \
"    span.setAttribute('sortdir','down');\n" \
"   }\n" \
"   resort(lnk);\n" \
"  //}\n" \
" }\n" \
"}\n";

static void http_dirlist_append_js_table_resort (buffer * const b, const request_st * const r) {
	char init_sort[] = "0,0";
	if (!buffer_string_is_empty(&r->uri.query)) {
		const char *qs = r->uri.query.ptr;
		do {
			if (qs[0] == 'C' && qs[1] == '=') {
				const int col = 0;
				switch (qs[2]) {
				case 'N': init_sort[col] = '0'; break;
				case 'M': init_sort[col] = '1'; break;
				case 'S': init_sort[col] = '2'; break;
				case 'T':
				case 'D': init_sort[col] = '3'; break;
				default:  break;
				}
			}
			else if (qs[0] == 'O' && qs[1] == '=') {
				const int order = 2;
				switch (qs[2]) {
				case 'A': init_sort[order] = '1'; break;
				case 'D': init_sort[order] = '0'; break;
				default:  break;
				}
			}
		} while ((qs = strchr(qs, '&')) && *++qs);
	}

	struct const_iovec iov[] = {
	  { CONST_STR_LEN("\n<script type=\"text/javascript\">\n// <!--\n\n") }
	 ,{ CONST_STR_LEN(js_simple_table_resort) }
	 ,{ CONST_STR_LEN(js_simple_table_init_sort) }
	 ,{ CONST_STR_LEN("\ninit_sort(") }
	 ,{ CONST_STR_LEN(init_sort) }
	 ,{ CONST_STR_LEN(");\n\n// -->\n</script>\n\n") }
	};
	buffer_append_iovec(b, iov, sizeof(iov)/sizeof(*iov));
}

static void http_list_directory_header(request_st * const r, plugin_data * const p) {

	chunkqueue * const cq = &r->write_queue;
	if (p->conf.auto_layout) {
		buffer * const out = chunkqueue_append_buffer_open(cq);
		buffer_append_string_len(out, CONST_STR_LEN(
			"<!DOCTYPE html>\n"
			"<html>\n"
			"<head>\n"
		));
		if (!buffer_string_is_empty(p->conf.encoding)) {
			buffer_append_str3(out,
			  CONST_STR_LEN("<meta charset=\""),
			  CONST_BUF_LEN(p->conf.encoding),
			  CONST_STR_LEN("\">\n"));
		}
		buffer_append_string_len(out, CONST_STR_LEN("<title>Index of "));
		buffer_append_string_encoded(out, CONST_BUF_LEN(&r->uri.path), ENCODING_MINIMAL_XML);
		buffer_append_string_len(out, CONST_STR_LEN("</title>\n"));

		if (!buffer_string_is_empty(p->conf.external_css)) {
			buffer_append_str3(out,
			  CONST_STR_LEN("<meta name=\"viewport\" content=\"initial-scale=1\">"
			                "<link rel=\"stylesheet\" type=\"text/css\" href=\""),
			  CONST_BUF_LEN(p->conf.external_css),
			  CONST_STR_LEN("\">\n"));
		} else {
			buffer_append_string_len(out, CONST_STR_LEN(
				"<style type=\"text/css\">\n"
				"a, a:active {text-decoration: none; color: blue;}\n"
				"a:visited {color: #48468F;}\n"
				"a:hover, a:focus {text-decoration: underline; color: red;}\n"
				"body {background-color: #F5F5F5;}\n"
				"h2 {margin-bottom: 12px;}\n"
				"table {margin-left: 12px;}\n"
				"th, td {"
				" font: 90% monospace;"
				" text-align: left;"
				"}\n"
				"th {"
				" font-weight: bold;"
				" padding-right: 14px;"
				" padding-bottom: 3px;"
				"}\n"
				"td {padding-right: 14px;}\n"
				"td.s, th.s {text-align: right;}\n"
				"div.list {"
				" background-color: white;"
				" border-top: 1px solid #646464;"
				" border-bottom: 1px solid #646464;"
				" padding-top: 10px;"
				" padding-bottom: 14px;"
				"}\n"
				"div.foot {"
				" font: 90% monospace;"
				" color: #787878;"
				" padding-top: 4px;"
				"}\n"
				"</style>\n"
			));
		}

		buffer_append_string_len(out, CONST_STR_LEN("</head>\n<body>\n"));
		chunkqueue_append_buffer_commit(cq);
	}

	if (!buffer_string_is_empty(p->conf.show_header)) {
		http_list_directory_include_file(r, p, 1);/*0 for readme; 1 for header*/
	}

	buffer * const out = chunkqueue_append_buffer_open(cq);
	buffer_append_string_len(out, CONST_STR_LEN("<h2>Index of "));
	buffer_append_string_encoded(out, CONST_BUF_LEN(&r->uri.path), ENCODING_MINIMAL_XML);
	buffer_append_string_len(out, CONST_STR_LEN(
		"</h2>\n"
		"<div class=\"list\">\n"
		"<table summary=\"Directory Listing\" cellpadding=\"0\" cellspacing=\"0\">\n"
		"<thead>"
		"<tr>"
			"<th class=\"n\">Name</th>"
			"<th class=\"m\">Last Modified</th>"
			"<th class=\"s\">Size</th>"
			"<th class=\"t\">Type</th>"
		"</tr>"
		"</thead>\n"
		"<tbody>\n"
	));
	if (!buffer_is_equal_string(&r->uri.path, CONST_STR_LEN("/"))) {
		buffer_append_string_len(out, CONST_STR_LEN(
		"<tr class=\"d\">"
			"<td class=\"n\"><a href=\"../\">..</a>/</td>"
			"<td class=\"m\">&nbsp;</td>"
			"<td class=\"s\">- &nbsp;</td>"
			"<td class=\"t\">Directory</td>"
		"</tr>\n"
		));
	}
	chunkqueue_append_buffer_commit(cq);
}

static void http_list_directory_footer(request_st * const r, plugin_data * const p) {

	chunkqueue * const cq = &r->write_queue;
	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"</tbody>\n"
		"</table>\n"
		"</div>\n"
	));

	if (!buffer_string_is_empty(p->conf.show_readme)) {
		http_list_directory_include_file(r, p, 0);/*0 for readme; 1 for header*/
	}

	if (p->conf.auto_layout) {
		buffer * const out = chunkqueue_append_buffer_open(cq);
		const buffer * const footer =
		  !buffer_string_is_empty(p->conf.set_footer)
		    ? p->conf.set_footer
		    : !buffer_string_is_empty(r->conf.server_tag)
		        ? r->conf.server_tag
		        : NULL;
		if (footer)
			buffer_append_str3(out,
			  CONST_STR_LEN("<div class=\"foot\">"),
			  CONST_BUF_LEN(footer),
			  CONST_STR_LEN("</div>\n"));

		if (!buffer_string_is_empty(p->conf.external_js)) {
			buffer_append_str3(out,
			  CONST_STR_LEN("<script type=\"text/javascript\" src=\""),
			  CONST_BUF_LEN(p->conf.external_js),
			  CONST_STR_LEN("\"></script>\n"));
		} else if (buffer_is_empty(p->conf.external_js)) {
			http_dirlist_append_js_table_resort(out, r);
		}

		buffer_append_string_len(out, CONST_STR_LEN(
			"</body>\n"
			"</html>\n"
		));
		chunkqueue_append_buffer_commit(cq);
	}
}

static int http_list_directory(request_st * const r, plugin_data * const p) {
	const uint32_t dlen = buffer_string_length(&r->physical.path);
#if defined __WIN32
	const uint32_t name_max = FILENAME_MAX;
#else
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
	/* allocate based on PATH_MAX rather than pathconf() to get _PC_NAME_MAX */
	const uint32_t name_max = PATH_MAX - dlen - 1;
#endif
	char * const path = malloc(dlen + name_max + 1);
	force_assert(NULL != path);
	memcpy(path, r->physical.path.ptr, dlen+1);
  #if defined(HAVE_XATTR) || defined(HAVE_EXTATTR) || !defined(_ATFILE_SOURCE)
	char *path_file = path + dlen;
  #endif

	struct dirent *dent;
  #ifndef _ATFILE_SOURCE /*(not using fdopendir unless _ATFILE_SOURCE)*/
	const int dfd = -1;
	DIR * const dp = opendir(path);
  #else
	const int dfd = fdevent_open_dirname(path, r->conf.follow_symlink);
	DIR * const dp = (dfd >= 0) ? fdopendir(dfd) : NULL;
  #endif
	if (NULL == dp) {
		log_perror(r->conf.errh, __FILE__, __LINE__, "opendir %s", path);
		if (dfd >= 0) close(dfd);
		free(path);
		return -1;
	}

	dirls_list_t dirs, files;
	dirs.ent   = (dirls_entry_t**) malloc(sizeof(dirls_entry_t*) * DIRLIST_BLOB_SIZE);
	force_assert(dirs.ent);
	dirs.size  = DIRLIST_BLOB_SIZE;
	dirs.used  = 0;
	files.ent  = (dirls_entry_t**) malloc(sizeof(dirls_entry_t*) * DIRLIST_BLOB_SIZE);
	force_assert(files.ent);
	files.size = DIRLIST_BLOB_SIZE;
	files.used = 0;

	const int hide_dotfiles = p->conf.hide_dot_files;
	struct stat st;
	while ((dent = readdir(dp)) != NULL) {
		const char * const d_name = dent->d_name;
		const uint32_t dsz = (uint32_t) _D_EXACT_NAMLEN(dent);
		if (d_name[0] == '.') {
			if (hide_dotfiles)
				continue;
			if (d_name[1] == '\0')
				continue;
			if (d_name[1] == '.' && d_name[2] == '\0')
				continue;
		}

		if (p->conf.hide_readme_file
		    && p->conf.show_readme
		    && buffer_eq_slen(p->conf.show_readme, d_name, dsz))
			continue;
		if (p->conf.hide_header_file
		    && p->conf.show_header
		    && buffer_eq_slen(p->conf.show_header, d_name, dsz))
			continue;

		/* compare d_name against excludes array
		 * elements, skipping any that match.
		 */
		if (p->conf.excludes
		    && mod_dirlisting_exclude(p->conf.excludes, d_name, dsz))
			continue;

		/* NOTE: the manual says, d_name is never more than NAME_MAX
		 *       so this should actually not be a buffer-overflow-risk
		 */
		if (dsz > name_max) continue;
	  #ifdef __COVERITY__
		/* For some reason, Coverity overlooks the strlen() performed
		 * a few lines above and thinks memcpy() below might access
		 * bytes beyond end of d_name[] with dsz+1 */
		force_assert(dsz < sizeof(dent->d_name));
	  #endif

	  #ifndef _ATFILE_SOURCE
		memcpy(path_file, d_name, dsz + 1);
		if (stat(path, &st) != 0)
			continue;
	  #else
		/*(XXX: follow symlinks, like stat(); not using AT_SYMLINK_NOFOLLOW) */
		if (0 != fstatat(dfd, d_name, &st, 0))
			continue; /* file *just* disappeared? */
	  #endif

		dirls_list_t * const list = !S_ISDIR(st.st_mode) ? &files : &dirs;
		if (list->used == list->size) {
			list->size += DIRLIST_BLOB_SIZE;
			list->ent   = (dirls_entry_t**) realloc(list->ent, sizeof(dirls_entry_t*) * list->size);
			force_assert(list->ent);
		}
		dirls_entry_t * const tmp = list->ent[list->used++] =
		  (dirls_entry_t*) malloc(sizeof(dirls_entry_t) + 1 + dsz);
		tmp->mtime = st.st_mtime;
		tmp->size  = st.st_size;
		tmp->namelen = dsz;
		memcpy(DIRLIST_ENT_NAME(tmp), d_name, dsz + 1);
	}
	closedir(dp);

	if (dirs.used) http_dirls_sort(dirs.ent, dirs.used);

	if (files.used) http_dirls_sort(files.ent, files.used);

	char sizebuf[sizeof("999.9K")];
	struct tm tm;

	/* Note: a very large directory may cause lighttpd to pause handling other
	 * requests while lighttpd processes directory, especially if directory is
	 * on a remote filesystem */

	/* generate large directory listings into tempfiles
	 * (estimate approx 200-256 bytes of HTML per item; could be up to ~512) */
	chunkqueue * const cq = &r->write_queue;
	buffer * const tb = r->tmp_buf;
	buffer * const out = (dirs.used + files.used <= 256)
	  ? chunkqueue_append_buffer_open(cq)
	  : tb;
	buffer_clear(out);

	/* directories */
	for (uint32_t i = 0; i < dirs.used; ++i) {
		dirls_entry_t * const tmp = dirs.ent[i];

		buffer_append_string_len(out, CONST_STR_LEN("<tr class=\"d\"><td class=\"n\"><a href=\""));
		buffer_append_string_encoded(out, DIRLIST_ENT_NAME(tmp), tmp->namelen, ENCODING_REL_URI_PART);
		buffer_append_string_len(out, CONST_STR_LEN("/\">"));
		buffer_append_string_encoded(out, DIRLIST_ENT_NAME(tmp), tmp->namelen, ENCODING_MINIMAL_XML);
		buffer_append_string_len(out, CONST_STR_LEN("</a>/</td><td class=\"m\">"));
		buffer_append_strftime(out, "%Y-%b-%d %T", localtime_r(&tmp->mtime, &tm));
		buffer_append_string_len(out, CONST_STR_LEN("</td><td class=\"s\">- &nbsp;</td><td class=\"t\">Directory</td></tr>\n"));

		free(tmp);

		if (buffer_string_space(out) < 256) {
			if (out == tb) {
				if (0 != chunkqueue_append_mem_to_tempfile(cq,
				                                           CONST_BUF_LEN(out),
				                                           r->conf.errh))
					break;
				buffer_clear(out);
			}
		}
	}

	/* files */
	const array * const mimetypes = r->conf.mimetypes;
	for (uint32_t i = 0; i < files.used; ++i) {
		dirls_entry_t * const tmp = files.ent[i];
		const buffer *content_type;
	  #if defined(HAVE_XATTR) || defined(HAVE_EXTATTR) /*(pass full path)*/
		content_type = NULL;
		if (r->conf.use_xattr) {
			memcpy(path_file, DIRLIST_ENT_NAME(tmp), tmp->namelen + 1);
			content_type = stat_cache_mimetype_by_xattr(path);
		}
		if (NULL == content_type)
	  #endif
			content_type = stat_cache_mimetype_by_ext(mimetypes, DIRLIST_ENT_NAME(tmp), tmp->namelen);
		if (NULL == content_type) {
			static const buffer octet_stream =
			  { "application/octet-stream",
			    sizeof("application/octet-stream"), 0 };
			content_type = &octet_stream;
		}

		buffer_append_string_len(out, CONST_STR_LEN("<tr><td class=\"n\"><a href=\""));
		buffer_append_string_encoded(out, DIRLIST_ENT_NAME(tmp), tmp->namelen, ENCODING_REL_URI_PART);
		buffer_append_string_len(out, CONST_STR_LEN("\">"));
		buffer_append_string_encoded(out, DIRLIST_ENT_NAME(tmp), tmp->namelen, ENCODING_MINIMAL_XML);
		buffer_append_string_len(out, CONST_STR_LEN("</a></td><td class=\"m\">"));
		buffer_append_strftime(out, "%Y-%b-%d %T", localtime_r(&tmp->mtime, &tm));
		size_t buflen =
		  http_list_directory_sizefmt(sizebuf, sizeof(sizebuf), tmp->size);
		struct const_iovec iov[] = {
		  { CONST_STR_LEN("</td><td class=\"s\">") }
		 ,{ sizebuf, buflen }
		 ,{ CONST_STR_LEN("</td><td class=\"t\">") }
		 ,{ CONST_BUF_LEN(content_type) }
		 ,{ CONST_STR_LEN("</td></tr>\n") }
		};
		buffer_append_iovec(out, iov, sizeof(iov)/sizeof(*iov));

		free(tmp);

		if (buffer_string_space(out) < 256) {
			if (out == tb) {
				if (0 != chunkqueue_append_mem_to_tempfile(cq,
				                                           CONST_BUF_LEN(out),
				                                           r->conf.errh))
					break;
				buffer_clear(out);
			}
		}
	}

	if (out == tb) {
		if (0 != chunkqueue_append_mem_to_tempfile(cq,
		                                           CONST_BUF_LEN(out),
		                                           r->conf.errh)) {
			/* ignore */
		}
	}
	else {
		chunkqueue_append_buffer_commit(cq);
	}

	free(files.ent);
	free(dirs.ent);
	free(path);

	return 0;
}


SUBREQUEST_FUNC(mod_dirlisting_subrequest);


URIHANDLER_FUNC(mod_dirlisting_subrequest_start) {
	plugin_data *p = p_d;

	if (NULL != r->handler_module) return HANDLER_GO_ON;
	if (!buffer_has_slash_suffix(&r->uri.path)) return HANDLER_GO_ON;
	if (!http_method_get_or_head(r->http_method)) return HANDLER_GO_ON;
	if (buffer_string_is_empty(&r->physical.path)) return HANDLER_GO_ON;

	mod_dirlisting_patch_config(r, p);

	if (!p->conf.dir_listing) return HANDLER_GO_ON;

	if (r->conf.log_request_handling) {
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "-- handling the request as Dir-Listing");
		log_error(r->conf.errh, __FILE__, __LINE__,
		  "URI          : %s", r->uri.path.ptr);
	}

	if (!stat_cache_path_isdir(&r->physical.path)) {
		if (errno == ENOTDIR)
			return HANDLER_GO_ON;
		log_perror(r->conf.errh,__FILE__,__LINE__,"%s",r->physical.path.ptr);
		r->http_status = 500;
		return HANDLER_FINISHED;
	}

	r->handler_module = p->self;
	return HANDLER_GO_ON;
}


SUBREQUEST_FUNC(mod_dirlisting_subrequest) {
	plugin_data *p = p_d;
	if (r->handler_module != p->self) return HANDLER_GO_ON;

	/*(alternatively, could save p->conf in hctx in subrequest start,
	 * but we currently enter here only once when processing dir)*/
	mod_dirlisting_patch_config(r, p);

	http_list_directory_header(r, p);
	if (http_list_directory(r, p)) {
		/* dirlisting failed */
		r->http_status = 403;
		http_response_body_clear(r, 0);
		return HANDLER_FINISHED;
	}
	http_list_directory_footer(r, p);

	r->resp_body_finished = 1;

	buffer * const vb =
	  http_header_response_set_ptr(r, HTTP_HEADER_CONTENT_TYPE,
	                               CONST_STR_LEN("Content-Type"));
	if (buffer_string_is_empty(p->conf.encoding)) {
		buffer_copy_string_len(vb, CONST_STR_LEN("text/html"));
	} else {
		buffer_append_str2(vb, CONST_STR_LEN("text/html; charset="),
		                       CONST_BUF_LEN(p->conf.encoding));
	}

	return HANDLER_FINISHED;
}


int mod_dirlisting_plugin_init(plugin *p);
int mod_dirlisting_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = "dirlisting";

	p->init        = mod_dirlisting_init;
	p->handle_subrequest_start = mod_dirlisting_subrequest_start;
	p->handle_subrequest       = mod_dirlisting_subrequest;
	p->set_defaults  = mod_dirlisting_set_defaults;
	p->cleanup     = mod_dirlisting_free;

	return 0;
}
