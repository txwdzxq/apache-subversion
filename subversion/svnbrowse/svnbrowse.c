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

#include "svn_client.h"
#include "svn_opt.h"
#include "svn_ra.h"
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_error.h"

#include <ncurses.h>

/* Control+ASCII character are represented as values 1-26 according to their
 * alphabetical order. */
#define CTRL(ch) ((ch) - 'a' + 1)

typedef struct svn_browse__item_t {
  const char *name;
  const svn_dirent_t *dirent;
} svn_browse__item_t;

/* a state of a single directory */
typedef struct svn_browse__state_t {
  /* information about this node */
  const char *relpath;
  svn_revnum_t revision;

  /* stores the list of nodes in this state; an array of svn_browse__item_t */
  apr_array_header_t *list;

  /* the index of hovered item */
  int selection;

  /* a pool where the structure is allocated */
  apr_pool_t *pool;
} svn_browse__state_t;

typedef struct svn_browse__model_t {
  const char *root;
  svn_revnum_t revision;

  svn_client_ctx_t *client;
  svn_ra_session_t *session;

  svn_browse__state_t *current;
  apr_pool_t *pool;
} svn_browse__model_t;

static svn_error_t *
list_cb(const char *relpath,
        svn_dirent_t *dirent,
        void *baton,
        apr_pool_t *scratch_pool)
{
  svn_browse__state_t *state = baton;
  svn_browse__item_t *item = apr_pcalloc(state->pool, sizeof(*item));
  item->name = svn_dirent_basename(relpath, state->pool);
  item->dirent = svn_dirent_dup(dirent, state->pool);
  APR_ARRAY_PUSH(state->list, svn_browse__item_t *) = item;
  return SVN_NO_ERROR;
}

static svn_error_t *
state_create(svn_browse__state_t **state_p,
             svn_ra_session_t *session,
             const char *relpath,
             svn_revnum_t revision,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  svn_browse__state_t *state = apr_pcalloc(result_pool, sizeof(*state));
  svn_revnum_t revnum;

  state->relpath = apr_pstrdup(result_pool, relpath);
  state->revision = state->revision;
  state->list = apr_array_make(result_pool, 0, sizeof(svn_browse__item_t *));
  state->selection = 0;
  state->pool = result_pool;

  /* TODO: use svn_ra_get_dir2() as it automatically treats SVN_INVALID_REVNUM
   * as HEAD and returns a list directly. */

  SVN_ERR(svn_ra_get_latest_revnum(session, &revnum, scratch_pool));

  SVN_ERR(svn_ra_list(session, relpath, revnum, NULL, svn_depth_immediates,
                      SVN_DIRENT_ALL, list_cb, state, scratch_pool));

  *state_p = state;
  return SVN_NO_ERROR;
}

static svn_error_t *
enter_path(svn_browse__model_t *ctx, const char *relpath,
           apr_pool_t *scratch_pool)
{
  svn_browse__state_t *newstate;
  apr_pool_t *state_pool = svn_pool_create(ctx->pool);

  SVN_ERR(state_create(&newstate, ctx->session, relpath, ctx->revision,
                       state_pool, scratch_pool));

  /* switch to the next state and nuke the previous one */
  apr_pool_destroy(ctx->current->pool);
  ctx->current = newstate;

  return SVN_NO_ERROR;
}

static svn_error_t *
model_create(svn_browse__model_t **model_p,
             const char *url,
             svn_revnum_t revision,
             apr_pool_t *result_pool,
             apr_pool_t *scratch_pool)
{
  svn_browse__model_t *model = apr_pcalloc(result_pool, sizeof(*model));
  svn_auth_baton_t *auth;
  svn_client_ctx_t *client;
  svn_ra_session_t *session;
  apr_pool_t *state_pool;
  svn_browse__state_t *state;

  /* Set up Authentication stuff. */
  SVN_ERR(svn_cmdline_create_auth_baton2(&auth, FALSE, NULL, NULL, NULL, FALSE,
                                         FALSE, FALSE, FALSE, FALSE, FALSE,
                                         NULL, NULL, NULL, result_pool));

  SVN_ERR(svn_client_create_context2(&client, NULL, result_pool));
  client->auth_baton = auth;

  SVN_ERR(svn_client_open_ra_session2(&session, url, NULL, client, result_pool,
                                      scratch_pool));

  /* the state should be in a separate pool so it's safe to free it */
  state_pool = svn_pool_create(result_pool);
  SVN_ERR(state_create(&state, session, "", revision, state_pool,
                       scratch_pool));
  /* TODO: we must use the repository root URL */
  SVN_ERR(svn_ra_get_session_url(session, &model->root, result_pool));
  model->revision = revision;
  model->client = client;
  model->session = session;
  model->current = state;
  model->pool = result_pool;

  *model_p = model;
  return SVN_NO_ERROR;
}

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

      if (i == 0)
        mvprintw(i + 1, 0, "../");
      else if (item->dirent->kind == svn_node_dir)
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
sub_main(int *code, int argc, char *argv[], apr_pool_t *pool)
{
  const char *url;
  svn_browse__model_t *ctx;
  svn_browse__view_t *view;
  apr_pool_t *iterpool;

  if (argc != 2)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                            "usage: svnbrowse <URL>");

  SVN_ERR(svn_uri_canonicalize_safe(&url, NULL, argv[1], pool, pool));

  SVN_ERR(model_create(&ctx, url, SVN_INVALID_REVNUM, pool, pool));

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
            ctx->current->selection--;
            break;
          case KEY_DOWN:
          case 'j':
            ctx->current->selection++;
            break;
          case '\n':
          case '\r':
            item = APR_ARRAY_IDX(ctx->current->list, ctx->current->selection,
                                 svn_browse__item_t *);
            new_url = svn_relpath_join(ctx->current->relpath, item->name,
                                       iterpool);
            SVN_ERR(enter_path(ctx, new_url, iterpool));
            break;
          case KEY_BACKSPACE:
          case '-':
          case 'u':
            new_url = svn_relpath_dirname(ctx->current->relpath, iterpool);
            SVN_ERR(enter_path(ctx, new_url, iterpool));
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

int main(int argc, char *argv[])
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
