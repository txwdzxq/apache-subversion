/*
 * model.c: the application state
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
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_ra.h"
#include "svn_pools.h"
#include "svn_error.h"

#include "private/svn_client_private.h"

#include "svnbrowse.h"

svn_error_t *
svn_browse__state_create(svn_browse__state_t **state_p,
                         svn_ra_session_t *session,
                         const char *relpath,
                         svn_revnum_t revision,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_browse__state_t *state = apr_pcalloc(result_pool, sizeof(*state));
  svn_revnum_t fetched_revnum;
  apr_hash_t *dirents;
  apr_hash_index_t *hi;

  SVN_ERR(svn_ra_get_dir2(session, &dirents, &fetched_revnum, NULL, relpath,
                          revision, SVN_DIRENT_ALL, scratch_pool));

  state->relpath = apr_pstrdup(result_pool, relpath);
  state->revision = fetched_revnum;
  state->selection = 0;
  state->pool = result_pool;

  state->list = apr_array_make(result_pool, 0, sizeof(svn_browse__item_t *));
  for (hi = apr_hash_first(scratch_pool, dirents); hi; hi = apr_hash_next(hi))
    {
      const char *name = apr_hash_this_key(hi);
      const svn_dirent_t *dirent = apr_hash_this_val(hi);

      svn_browse__item_t *item = apr_pcalloc(result_pool, sizeof(*item));
      item->name = apr_pstrdup(result_pool, name);
      item->dirent = svn_dirent_dup(dirent, result_pool);

      APR_ARRAY_PUSH(state->list, svn_browse__item_t *) = item;
    }

  *state_p = state;
  return SVN_NO_ERROR;
}

svn_error_t *
svn_browse__model_enter_path(svn_browse__model_t *ctx, const char *relpath,
                 apr_pool_t *scratch_pool)
{
  svn_browse__state_t *newstate;
  apr_pool_t *state_pool = svn_pool_create(ctx->pool);

  SVN_ERR(svn_browse__state_create(&newstate, ctx->session, relpath,
                                   ctx->revision, state_pool, scratch_pool));

  /* switch to the next state and nuke the previous one */
  apr_pool_destroy(ctx->current->pool);
  ctx->current = newstate;

  return SVN_NO_ERROR;
}

svn_browse__item_t *
svn_browse__model_get_selected_item(svn_browse__model_t *model)
{
  return APR_ARRAY_IDX(model->current->list, model->current->selection,
                       svn_browse__item_t *);
}

svn_error_t *
svn_browse__model_go_enter(svn_browse__model_t *model, apr_pool_t *scratch_pool)
{
  svn_browse__item_t *item = svn_browse__model_get_selected_item(model);
  const char *new_url = svn_relpath_join(model->current->relpath, item->name,
                                         scratch_pool);
  return svn_error_trace(svn_browse__model_enter_path(model, new_url,
                                                      scratch_pool));
}

svn_error_t *
svn_browse__model_go_up(svn_browse__model_t *model, apr_pool_t *scratch_pool)
{
  const char *new_url = svn_relpath_dirname(model->current->relpath,
                                            scratch_pool);
  return svn_error_trace(svn_browse__model_enter_path(model, new_url,
                                                      scratch_pool));
}

svn_error_t *
svn_browse__model_move_selection(svn_browse__model_t *model, int delta)
{
  model->current->selection += delta;

  if (model->current->selection >= model->current->list->nelts)
    model->current->selection = model->current->list->nelts - 1;

  if (model->current->selection < 0)
    model->current->selection = 0;

  return SVN_NO_ERROR;
}

svn_error_t *
svn_browse__model_create(svn_browse__model_t **model_p,
                         svn_client_ctx_t *ctx,
                         const char *path_or_url,
                         const svn_opt_revision_t *peg_revision,
                         const svn_opt_revision_t *revision,
                         apr_pool_t *result_pool,
                         apr_pool_t *scratch_pool)
{
  svn_browse__model_t *model = apr_pcalloc(result_pool, sizeof(*model));
  svn_ra_session_t *session;
  apr_pool_t *state_pool;
  const char *root, *relpath;

  svn_opt_revision_t peg_rev = *peg_revision;
  svn_opt_revision_t start_rev = *revision;
  svn_client__pathrev_t *loc;

  /* Default revisions: peg -> working or head; operative -> peg. */
  SVN_ERR(svn_opt_resolve_revisions(&peg_rev, &start_rev,
                                    svn_path_is_url(path_or_url),
                                    TRUE /* notice_local_mods */,
                                    scratch_pool));

  SVN_ERR(svn_client__ra_session_from_path2(&session, &loc, path_or_url, NULL,
                                            &peg_rev, &start_rev, ctx,
                                            result_pool));

  model->root = apr_pstrdup(result_pool, loc->repos_root_url);
  model->client = ctx;
  model->session = session;

  /* If the revision is ovbiously HEAD, we are not going to use the resolved
   * revnum, but rely on svn_ra_get_dir2() doing that for us. It allows the
   * browser to always browse on the latest revision and catch-up recent
   * commits as session is active.
   *
   * Note: only head+head combination of peg_revision and revision should be
   * considered as the "real" head. If we were to for example go to r123 and
   * then resolve HEAD, its value may change as session progresses. This logic
   * would be too complicated and rather confusing. That's why we will fallback
   * to what the client is doing instead of reinventing the wheel.
   *
   * When invoked from a working copy, it's not HEAD either. It's should point
   * to WORKING, i.e. revnum should not change. Again rescanning the working
   * copy is possible but we assume it never changes.
   */

  if (peg_rev.kind == svn_opt_revision_head &&
      start_rev.kind == svn_opt_revision_head)
    model->revision = SVN_INVALID_REVNUM;
  else
    model->revision = loc->rev;

  SVN_ERR(svn_ra_reparent(session, loc->repos_root_url, scratch_pool));

  relpath = svn_uri_skip_ancestor(loc->repos_root_url, loc->url, scratch_pool);

  /* the state should be in a separate pool so it's safe to free it */
  state_pool = svn_pool_create(result_pool);
  SVN_ERR(svn_browse__state_create(&model->current, session, relpath,
                                   model->revision, state_pool, scratch_pool));

  *model_p = model;
  return SVN_NO_ERROR;
}
