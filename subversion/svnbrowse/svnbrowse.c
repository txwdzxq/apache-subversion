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
#include "svn_path.h"
#include "svn_pools.h"
#include "svn_cmdline.h"
#include "svn_error.h"

#include <ncurses.h>

/* Control+ASCII character are represented as values 1-26 according to their
 * alphabetical order. */
#define CTRL(ch) (ch - 'a' + 1)

typedef struct item_t {
  const char *relpath;
  const svn_dirent_t *dirent;
} item_t;

typedef struct svn_browse__ctx_t {
  const char *root;
  const char *relpath;
  const char *abspath;
  svn_opt_revision_t revision;

  svn_client_ctx_t *client;

  apr_array_header_t *list;
  int selection;
  apr_pool_t *list_pool;
} svn_browse__ctx_t;

static svn_error_t *
init_client(svn_browse__ctx_t *ctx, apr_pool_t *pool)
{
  svn_auth_baton_t *auth;

  SVN_ERR(svn_client_create_context2(&ctx->client, NULL, pool));

  /* Set up Authentication stuff. */
  SVN_ERR(svn_cmdline_create_auth_baton2(&auth, FALSE, NULL, NULL, NULL, FALSE,
                                         FALSE, FALSE, FALSE, FALSE, FALSE,
                                         NULL, NULL, NULL, pool));

  ctx->client->auth_baton = auth;

  return SVN_NO_ERROR;
}

static svn_error_t *
list_cb(void *baton,
        const char *path,
        const svn_dirent_t *dirent,
        const svn_lock_t *lock,
        const char *abs_path,
        const char *external_parent_url,
        const char *external_target,
        apr_pool_t *scratch_pool)
{
  svn_browse__ctx_t *ctx = baton;
  item_t *item = apr_pcalloc(ctx->list_pool, sizeof(*item));
  item->relpath = apr_pstrdup(ctx->list_pool, path);
  item->dirent = svn_dirent_dup(dirent, ctx->list_pool);
  APR_ARRAY_PUSH(ctx->list, item_t *) = item;
  return SVN_NO_ERROR;
}

static svn_error_t *
enter_path(svn_browse__ctx_t *ctx, const char *relpath, apr_pool_t *pool)
{
  ctx->relpath = apr_pstrdup(pool, relpath);
  ctx->abspath = svn_path_url_add_component2(ctx->root, relpath, pool);

  ctx->list = apr_array_make(pool, 0, sizeof(item_t *));
  ctx->selection = 0;

  SVN_ERR(svn_client_list4(ctx->abspath, &ctx->revision, &ctx->revision, NULL,
                           svn_depth_immediates, SVN_DIRENT_ALL, TRUE, TRUE,
                           list_cb, ctx, ctx->client, pool));

  return SVN_NO_ERROR;
}

static void
ui_draw(svn_browse__ctx_t *ctx, apr_pool_t *pool)
{
  int i;

  mvprintw(0, 4, "Browsing: %s", ctx->abspath);

  for (i = 0; i < ctx->list->nelts; i++)
    {
      item_t *item = APR_ARRAY_IDX(ctx->list, i, item_t *);

      if (i == ctx->selection)
        standout();

      if (i == 0)
        mvprintw(i + 1, 0, "../");
      else if (item->dirent->kind == svn_node_dir)
        mvprintw(i + 1, 0, "%s/", item->relpath);
      else if (item->dirent->kind == svn_node_file)
        mvprintw(i + 1, 0, "%s", item->relpath);
      else
        abort();

			mvprintw(i + 1, COLS - 40, "%8ld KiB  r%-8ld  %s",
               item->dirent->size / 1024,
               item->dirent->created_rev,
               item->dirent->last_author);

      if (i == ctx->selection)
        standend();
    }
}

static svn_error_t *
sub_main(int *code, int argc, char *argv[], apr_pool_t *pool)
{
  svn_browse__ctx_t ctx = { 0 };

  if (argc != 2)
    return svn_error_create(SVN_ERR_CL_ARG_PARSING_ERROR, NULL,
                            "usage: svnbrowse <URL>");

  SVN_ERR(svn_uri_canonicalize_safe(&ctx.root, NULL, argv[1], pool, pool));
  ctx.revision.kind = svn_opt_revision_head;
  ctx.list_pool = pool;

  SVN_ERR(init_client(&ctx, pool));
  SVN_ERR(enter_path(&ctx, "", pool));

  /* init the display */
  initscr();

  /* put ncurses into keypad mode to handle arrow inputs */
  intrflush(stdscr, FALSE);
  keypad(stdscr, TRUE);
  nonl();

  while (TRUE)
    {
      int ch;

      clear();
      ui_draw(&ctx, pool);
      refresh();

      ch = getch();

      /* getch() reads the next character/key with the following additional
       * rules:
       * 1. as we configured it to use keypad(), arrows and other special keys
       *    are handled as KEY_XXX.
       * 2. Control (CTRL) version are handled as literal 1-26 values of ch where
       *    1 is <C-A> and 26 is <C-Z>.
       * 3. The rest of keys remain as their equivalents on the current layout.
       * 4. If shift is held, they just become uppercased.
       */

      if (ch == KEY_UP || ch == 'k')
        {
          ctx.selection--;
        }
      else if (ch == KEY_DOWN || ch == 'j')
        {
          ctx.selection++;
        }
      else if (ch == '\n' || ch == '\r')
        {
          item_t *item = APR_ARRAY_IDX(ctx.list, ctx.selection, item_t *);
          const char *new_url = svn_relpath_join(ctx.relpath, item->relpath, pool);
          SVN_ERR(enter_path(&ctx, new_url, pool));
        }
      else if (ch == KEY_BACKSPACE || ch == '-' || ch == 'u')
        {
          const char *new_url = svn_relpath_dirname(ctx.relpath, pool);
          SVN_ERR(enter_path(&ctx, new_url, pool));
        }
      /* TODO: quit via escape. some say just check for 27, but it I think it's
       * a bit ugly. */
      else if (ch == 'q')
        {
          break;
        }
    }

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
