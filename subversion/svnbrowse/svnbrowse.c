/*
 * svnbrowse.c:  TUI (terminal user interface) client for browsing and
 *               exploring remote Subversion targets.
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */

#include <apr.h>

#include "svn_cmdline.h"
#include "svn_opt.h"
#include "svn_ra.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_error.h"

#include "private/svn_cmdline_private.h"

#include <ncurses.h>

#include "svn_private_config.h"
#include "svnbrowse.h"

/* Option codes and descriptions for the command line client.
 * The entire list must be terminated with an entry of nulls. */
const apr_getopt_option_t svn_browse__options[] =
{
  {"username",      opt_auth_username, 1, N_("specify a username ARG")},
  {"password",      opt_auth_password, 1,
                    N_("specify a password ARG (caution: on many operating\n"
                       "                             "
                       "systems, other users will be able to see this)")},
  {"password-from-stdin",
                    opt_auth_password_from_stdin, 0,
                    N_("read password from stdin")},
  {"revision",      'r', 1,
                    N_("ARG\n"
                       "                             "
                       "A revision argument can be one of:\n"
                       "                             "
                       "   NUMBER       revision number\n"
                       "                             "
                       "   '{' DATE '}' revision at start of the date\n"
                       "                             "
                       "   'HEAD'       latest in repository\n"
                       "                             "
                       "   'BASE'       base rev of item's working copy\n"
                       "                             "
                       "   'COMMITTED'  last commit at or before BASE\n"
                       "                             "
                       "   'PREV'       revision just before COMMITTED")},
  {"help",          'h', 0, N_("show help on a subcommand")},
  {NULL,            '?', 0, N_("show help on a subcommand")},
  {"trust-server-cert", opt_trust_server_cert, 0,
                    N_("deprecated; same as\n"
                       "                             "
                       "--trust-server-cert-failures=unknown-ca")},
  {"trust-server-cert-failures", opt_trust_server_cert_failures, 1,
                    N_("with --non-interactive, accept SSL server\n"
                       "                             "
                       "certificates with failures; ARG is comma-separated\n"
                       "                             "
                       "list of 'unknown-ca' (Unknown Authority),\n"
                       "                             "
                       "'cn-mismatch' (Hostname mismatch), 'expired'\n"
                       "                             "
                       "(Expired certificate), 'not-yet-valid' (Not yet\n"
                       "                             "
                       "valid certificate) and 'other' (all other not\n"
                       "                             "
                       "separately classified certificate errors).")},
  {"config-dir",    opt_config_dir, 1,
                    N_("read user configuration files from directory ARG")},
  {"config-option", opt_config_option, 1,
                    N_("set user configuration option in the format:\n"
                       "                             "
                       "    FILE:SECTION:OPTION=[VALUE]\n"
                       "                             "
                       "For example:\n"
                       "                             "
                       "    servers:global:http-library=serf")},
  {"no-auth-cache", opt_no_auth_cache, 0,
                    N_("do not cache authentication tokens")},
  {"version",       opt_version, 0, N_("show program version information")},
  {"verbose",       'v', 0, N_("print extra information")},
  { NULL, 0, 0, NULL }
};

/* Control+ASCII character are represented as values 1-26 according to their
 * alphabetical order. */
#define CTRL(ch) ((ch) - 'a' + 1)

typedef struct svn_browse__view_t {
  /* TODO: store information about terminal screen (a WINDOW* in curses world) */
  svn_browse__model_t *model;
} svn_browse__view_t;

static svn_browse__view_t *
view_make(svn_browse__model_t *model, apr_pool_t *result_pool)
{
  svn_browse__view_t *view = apr_pcalloc(result_pool, sizeof(*view));
  view->model = model;
  return view;
}

static void
view_draw(svn_browse__view_t *view, apr_pool_t *pool)
{
  int i;
  const char *abspath = svn_path_url_add_component2(
      view->model->root, view->model->current->relpath, pool);

  mvprintw(0, 4, "Browsing: %s", abspath);

  for (i = 0; i < view->model->current->list->nelts; i++)
    {
      svn_browse__item_t *item = APR_ARRAY_IDX(view->model->current->list, i,
                                               svn_browse__item_t *);

      if (i == view->model->current->selection)
        standout();

      if (item->dirent->kind == svn_node_dir)
        mvprintw(i + 1, 0, "%s/", item->name);
      else if (item->dirent->kind == svn_node_file)
        mvprintw(i + 1, 0, "%s", item->name);
      else
        abort();

      mvprintw(i + 1, COLS - 40, "%8ld KiB  r%-8ld  %s",
               item->dirent->size / 1024,
               item->dirent->created_rev,
               item->dirent->last_author);

      if (i == view->model->current->selection)
        standend();
    }
}

static svn_error_t *
show_usage(apr_pool_t *scratch_pool)
{
  fprintf(stderr, "Type 'svnbrowse --help' for usage.\n");
  return SVN_NO_ERROR;
}

static svn_error_t *
show_help(apr_pool_t *scratch_pool,
          const svn_browse__opt_state_t *opt_state)
{
  svn_stringbuf_t *buf = svn_stringbuf_create_empty(scratch_pool);
  const apr_getopt_option_t *opt;

  svn_stringbuf_appendcstr(buf, N_(
      "usage: svnbrowse <target> [options]\n"
      "Interactively browse Subversion repositories\n"
      "\n"
  ));

  svn_stringbuf_appendcstr(buf, N_("Valid options:\n"));
  for (opt = svn_browse__options; opt->description; opt++)
    {
      if (opt->name)
        {
          const char *opts;
          svn_opt_format_option(&opts, opt, TRUE /* doc */, scratch_pool);
          svn_stringbuf_appendcstr(buf, "  ");
          svn_stringbuf_appendcstr(buf, opts);
          svn_stringbuf_appendbyte(buf, '\n');
        }
    }

  return svn_error_trace(svn_cmdline_fputs(buf->data, stderr, scratch_pool));
}

static svn_error_t *
show_version(apr_pool_t *scratch_pool,
             const svn_browse__opt_state_t *opt_state)
{
  const char *ra_desc_start
    = "The following repository access (RA) modules are available:\n\n";
  svn_stringbuf_t *version_footer;

  version_footer = svn_stringbuf_create(ra_desc_start, scratch_pool);
  SVN_ERR(svn_ra_print_modules(version_footer, scratch_pool));

  SVN_ERR(svn_opt_print_help5(NULL,
                              "svnbrowse",
                              TRUE /* print_version */,
                              opt_state->quiet,
                              opt_state->verbose,
                              version_footer->data,
                              NULL, NULL, NULL, NULL, NULL,
                              scratch_pool));

  return SVN_NO_ERROR;
}

static svn_error_t *
sub_main(int *code, int argc, const char *argv[], apr_pool_t *pool)
{
  const char *url;
  svn_client_ctx_t *client;
  svn_auth_baton_t *auth;
  svn_browse__model_t *ctx;
  svn_browse__view_t *view;
  svn_browse__opt_state_t opt_state = { 0 };
  svn_boolean_t read_pass_from_stdin = FALSE;
  apr_pool_t *iterpool;
  apr_getopt_t *os;

  opt_state.revision.kind = svn_opt_revision_head;
  opt_state.config_options =
      apr_array_make(pool, 0, sizeof(svn_cmdline__config_argument_t *));

  SVN_ERR(svn_cmdline__getopt_init(&os, argc, argv, pool));
  os->interleave = 1;

  while (TRUE)
    {
      const char *opt_arg;
      int opt_id;

      /* Parse the next option. */
      apr_status_t status = apr_getopt_long(os, svn_browse__options, &opt_id,
                                            &opt_arg);

      if (APR_STATUS_IS_EOF(status))
        break;
      else if (status != APR_SUCCESS)
        {
          SVN_ERR(show_usage(pool));
          *code = EXIT_FAILURE;
          return SVN_NO_ERROR;
        }

      switch (opt_id)
        {
          case 'r':
            SVN_ERR(svn_opt_parse_one_revision(&opt_state.revision, opt_arg, pool));
            break;
          case 'h':
          case '?':
            opt_state.help = TRUE;
            break;
          case opt_version:
            opt_state.version = TRUE;
            break;
          case opt_auth_username:
            opt_state.auth_username = apr_pstrdup(pool, opt_arg);
            break;
          case opt_auth_password:
            opt_state.auth_password = apr_pstrdup(pool, opt_arg);
            break;
          case opt_auth_password_from_stdin:
            read_pass_from_stdin = TRUE;
            break;
          case opt_no_auth_cache:
            opt_state.no_auth_cache = TRUE;
            break;
          /* ### can we drop it in a 1.16 tool? */
          case opt_trust_server_cert: /* backwards compat to 1.8 */
            opt_state.trust_server_cert_unknown_ca = TRUE;
            break;
          case opt_trust_server_cert_failures:
            SVN_ERR(svn_cmdline__parse_trust_options(
                          &opt_state.trust_server_cert_unknown_ca,
                          &opt_state.trust_server_cert_cn_mismatch,
                          &opt_state.trust_server_cert_expired,
                          &opt_state.trust_server_cert_not_yet_valid,
                          &opt_state.trust_server_cert_other_failure,
                          opt_arg, pool));
            break;
          case opt_config_dir:
            opt_state.config_dir = svn_dirent_internal_style(opt_arg, pool);
            break;
          case opt_config_option:
            SVN_ERR(svn_cmdline__parse_config_option(opt_state.config_options,
                                                     opt_arg, "svnbrowse: ", pool));
            break;
          default:
            break;
        }
    }

  if (opt_state.version)
    {
      SVN_ERR(show_version(pool, &opt_state));
      return SVN_NO_ERROR;
    }

  if (opt_state.help)
    {
      SVN_ERR(show_help(pool, &opt_state));
      return SVN_NO_ERROR;
    }

  /* TODO: WC paths are not implemented; svn_uri_canonicalize_safe() will just
   * fail in case of one */
  url = (os->ind < argc) ? os->argv[os->ind++] : ".";
  SVN_ERR(svn_uri_canonicalize_safe(&url, NULL, url, pool, pool));

  /* we must fail if there are extra arguments */
  if (os->ind < argc - 1)
    {
      printf("%d\n", os->ind);
      *code = EXIT_FAILURE;
      SVN_ERR(show_usage(pool));
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_config_ensure(opt_state.config_dir, pool));

  /* Set up Authentication stuff. */
  SVN_ERR(svn_cmdline_create_auth_baton2(&auth, FALSE, NULL, NULL, NULL, FALSE,
                                         FALSE, FALSE, FALSE, FALSE, FALSE,
                                         NULL, NULL, NULL, pool));

  SVN_ERR(svn_client_create_context2(&client, NULL, pool));
  client->auth_baton = auth;

  SVN_ERR(svn_browse__model_create(&ctx, client, url, SVN_INVALID_REVNUM, pool,
                                   pool));

  /* init the display */
  initscr();

  /* put ncurses into keypad mode to handle arrow inputs */
  intrflush(stdscr, FALSE);
  keypad(stdscr, TRUE);
  nonl();

  view = view_make(ctx, pool);

  iterpool = svn_pool_create(pool);

  while (TRUE)
    {
      svn_browse__item_t *item;
      const char *new_url;

      svn_pool_clear(iterpool);

      clear();
      view_draw(view, iterpool);
      refresh();

      /* getch() reads the next character/key with the following additional
       * rules:
       * 1. as we configured it to use keypad(), arrows and other special keys
       *    are handled as KEY_XXX.
       * 2. Control (CTRL) version are handled as literal 1-26 values of ch where
       *    1 is <C-A> and 26 is <C-Z>.
       * 3. The rest of keys remain as their equivalents on the current layout.
       * 4. If shift is held, they just become uppercased.
       */
      switch (getch())
        {
          case KEY_UP:
          case 'k':
            SVN_ERR(svn_browse__model_move_selection(ctx, -1));
            break;
          case KEY_DOWN:
          case 'j':
            SVN_ERR(svn_browse__model_move_selection(ctx, 1));
            break;
          case '\n':
          case '\r':
            SVN_ERR(svn_browse__model_go_enter(ctx, iterpool));
            break;
          case KEY_BACKSPACE:
          case '-':
          case 'u':
            SVN_ERR(svn_browse__model_go_up(ctx, iterpool));
            break;
          /* TODO: quit via escape. some say just check for 27, but it I think it's
           * a bit ugly. */
          case 'q':
            goto quit;
        }
    }

quit:
	endwin();

  return SVN_NO_ERROR;
}

int main(int argc, const char *argv[])
{
  apr_pool_t *pool;
  int exit_code = EXIT_SUCCESS;
  svn_error_t *err;

  if (svn_cmdline_init("svnbrowse", stderr) != EXIT_SUCCESS)
    return EXIT_FAILURE;

  pool = apr_allocator_owner_get(svn_pool_create_allocator(FALSE));

  err = sub_main(&exit_code, argc, argv, pool);

  if (err)
    {
      exit_code = EXIT_FAILURE;
      svn_cmdline_handle_exit_error(err, NULL, "svnbrowse: ");
    }

  svn_pool_destroy(pool);
  return exit_code;
}
