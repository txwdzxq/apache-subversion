/*
 * merge_processor.c: svn_diff_tree_processor implementation for merge
 *                    apply operation.
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

/* ==================================================================== */



/*** Includes ***/

#include <assert.h>
#include <apr_strings.h>
#include <apr_tables.h>
#include <apr_hash.h>
#include "svn_types.h"
#include "svn_hash.h"
#include "svn_wc.h"
#include "svn_delta.h"
#include "svn_diff.h"
#include "svn_mergeinfo.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_io.h"
#include "svn_utf.h"
#include "svn_pools.h"
#include "svn_config.h"
#include "svn_props.h"
#include "svn_time.h"
#include "svn_sorts.h"
#include "svn_subst.h"
#include "svn_ra.h"
#include "svn_version.h"
#include "client.h"
#include "mergeinfo.h"

#include "private/svn_fspath.h"
#include "private/svn_mergeinfo_private.h"
#include "private/svn_client_private.h"
#include "private/svn_sorts_private.h"
#include "private/svn_subr_private.h"
#include "private/svn_wc_private.h"

#include "svn_private_config.h"



/*** Repos-Diff Editor Callbacks ***/

typedef struct merge_apply_processor_baton_t
{
  svn_boolean_t force_delete;         /* Delete a file/dir even if modified */
  svn_boolean_t dry_run;
  svn_boolean_t record_only;          /* Whether to merge only mergeinfo
                                         differences. */
  svn_boolean_t same_repos;           /* Whether the merge source repository
                                         is the same repository as the
                                         target.  Defaults to FALSE if DRY_RUN
                                         is TRUE.*/

  /* Description of merge target node */
  const svn_client__merge_target_t *target;

  /* The left and right URLs and revs.  */
  svn_client__merge_source_t merge_source;

  svn_client_ctx_t *ctx;              /* Client context for callbacks, etc. */

  /* The diff3_cmd in ctx->config, if any, else null.  We could just
     extract this as needed, but since more than one caller uses it,
     we just set it up when this baton is created. */
  const char *diff3_cmd;
  const apr_array_header_t *merge_options;

  /* Array of file extension patterns to preserve as extensions in
     generated conflict files. */
  const apr_array_header_t *ext_patterns;

  /* Our notification callback, that adds a 'begin' notification */
  svn_wc_notify_func2_t notify_func;
  void *notify_baton;

  const svn_client__apply_processor_callbacks_t *cb_table;
  void *cb_baton;
} merge_apply_processor_baton_t;


/* Return SVN_ERR_UNSUPPORTED_FEATURE if URL is not inside the repository
   of LOCAL_ABSPATH.  Use SCRATCH_POOL for temporary allocations. */
static svn_error_t *
check_repos_match(const svn_client__merge_target_t *target,
                  const char *local_abspath,
                  const char *url,
                  apr_pool_t *scratch_pool)
{
  if (!svn_uri__is_ancestor(target->loc.repos_root_url, url))
    return svn_error_createf(
        SVN_ERR_UNSUPPORTED_FEATURE, NULL,
         _("URL '%s' of '%s' is not in repository '%s'"),
         url, svn_dirent_local_style(local_abspath, scratch_pool),
         target->loc.repos_root_url);

  return SVN_NO_ERROR;
}

/* Store LOCAL_ABSPATH in PATH_HASH after duplicating it into the pool
   containing PATH_HASH. */
/* TODO: deduplicate with merge.c  */
static APR_INLINE void
store_path(apr_hash_t *path_hash, const char *local_abspath)
{
  const char *dup_path = apr_pstrdup(apr_hash_pool_get(path_hash),
                                     local_abspath);

  svn_hash_sets(path_hash, dup_path, dup_path);
}

/* Return a state indicating whether the WC metadata matches the
 * node kind on disk of the local path LOCAL_ABSPATH.
 * Use MERGE_B to determine the dry-run details; particularly, if a dry run
 * noted that it deleted this path, assume matching node kinds (as if both
 * kinds were svn_node_none).
 *
 *   - Return svn_wc_notify_state_inapplicable if the node kind matches.
 *   - Return 'obstructed' if there is a node on disk where none or a
 *     different kind is expected, or if the disk node cannot be read.
 *   - Return 'missing' if there is no node on disk but one is expected.
 *     Also return 'missing' for server-excluded nodes (not here due to
 *     authz or other reasons determined by the server).
 *
 * Optionally return a bit more info for interested users.
 **/
/* TODO: deduplicate with merge.c  */
static svn_error_t *
perform_obstruction_check(svn_wc_notify_state_t *obstruction_state,
                          svn_boolean_t *deleted,
                          svn_boolean_t *excluded,
                          svn_node_kind_t *kind,
                          svn_depth_t *parent_depth,
                          const merge_apply_processor_baton_t *merge_b,
                          const char *local_abspath,
                          apr_pool_t *scratch_pool)
{
  svn_wc_context_t *wc_ctx = merge_b->ctx->wc_ctx;
  svn_node_kind_t wc_kind;
  svn_boolean_t check_root;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  *obstruction_state = svn_wc_notify_state_inapplicable;

  if (deleted)
    *deleted = FALSE;
  if (kind)
    *kind = svn_node_none;

  if (kind == NULL)
    kind = &wc_kind;

  check_root = ! strcmp(local_abspath, merge_b->target->abspath);

  SVN_ERR(svn_wc__check_for_obstructions(obstruction_state,
                                         kind,
                                         deleted,
                                         excluded,
                                         parent_depth,
                                         wc_ctx, local_abspath,
                                         check_root,
                                         scratch_pool));
  return SVN_NO_ERROR;
}

/* Create *LEFT and *RIGHT conflict versions for conflict victim
 * at VICTIM_ABSPATH, with merge-left node kind MERGE_LEFT_NODE_KIND
 * and merge-right node kind MERGE_RIGHT_NODE_KIND, using information
 * obtained from MERGE_SOURCE and TARGET.
 * Allocate returned conflict versions in RESULT_POOL. */
static svn_error_t *
make_conflict_versions(const svn_wc_conflict_version_t **left,
                       const svn_wc_conflict_version_t **right,
                       const char *victim_abspath,
                       svn_node_kind_t merge_left_node_kind,
                       svn_node_kind_t merge_right_node_kind,
                       const svn_client__merge_source_t *merge_source,
                       const svn_client__merge_target_t *target,
                       apr_pool_t *result_pool,
                       apr_pool_t *scratch_pool)
{
  const char *child = svn_dirent_skip_ancestor(target->abspath,
                                               victim_abspath);
  const char *left_relpath, *right_relpath;

  SVN_ERR_ASSERT(child != NULL);
  left_relpath = svn_client__pathrev_relpath(merge_source->loc1,
                                             scratch_pool);
  right_relpath = svn_client__pathrev_relpath(merge_source->loc2,
                                              scratch_pool);

  *left = svn_wc_conflict_version_create2(
            merge_source->loc1->repos_root_url,
            merge_source->loc1->repos_uuid,
            svn_relpath_join(left_relpath, child, scratch_pool),
            merge_source->loc1->rev,
            merge_left_node_kind, result_pool);

  *right = svn_wc_conflict_version_create2(
             merge_source->loc2->repos_root_url,
             merge_source->loc2->repos_uuid,
             svn_relpath_join(right_relpath, child, scratch_pool),
             merge_source->loc2->rev,
             merge_right_node_kind, result_pool);

  return SVN_NO_ERROR;
}


/* Make a copy of PROPCHANGES (array of svn_prop_t) into *TRIMMED_PROPCHANGES,
   omitting any svn:mergeinfo changes.  */
static svn_error_t *
omit_mergeinfo_changes(apr_array_header_t **trimmed_propchanges,
                       const apr_array_header_t *propchanges,
                       apr_pool_t *result_pool)
{
  int i;

  *trimmed_propchanges = apr_array_make(result_pool,
                                        propchanges->nelts,
                                        sizeof(svn_prop_t));

  for (i = 0; i < propchanges->nelts; ++i)
    {
      const svn_prop_t *change = &APR_ARRAY_IDX(propchanges, i, svn_prop_t);

      /* If this property is not svn:mergeinfo, then copy it.  */
      if (strcmp(change->name, SVN_PROP_MERGEINFO) != 0)
        APR_ARRAY_PUSH(*trimmed_propchanges, svn_prop_t) = *change;
    }

  return SVN_NO_ERROR;
}


/* Prepare a set of property changes PROPCHANGES to be used for a merge
   operation on LOCAL_ABSPATH.

   Remove all non-regular prop-changes (entry-props and WC-props).
   Remove all non-mergeinfo prop-changes if it's a record-only merge.
   Remove self-referential mergeinfo (### in some cases...)
   Remove foreign-repository mergeinfo (### in some cases...)

   Store the resulting property changes in *PROP_UPDATES.
   Store information on where mergeinfo is updated in MERGE_B.

   Used for both file and directory property merges. */
static svn_error_t *
prepare_merge_props_changed(const apr_array_header_t **prop_updates,
                            const char *local_abspath,
                            const apr_array_header_t *propchanges,
                            merge_apply_processor_baton_t *merge_b,
                            apr_pool_t *result_pool,
                            apr_pool_t *scratch_pool)
{
  apr_array_header_t *props;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  /* We only want to merge "regular" version properties:  by
     definition, 'svn merge' shouldn't touch any data within .svn/  */
  SVN_ERR(svn_categorize_props(propchanges, NULL, NULL, &props,
                               result_pool));

  /* If we are only applying mergeinfo changes then we need to do
     additional filtering of PROPS so it contains only mergeinfo changes. */
  if (merge_b->record_only && props->nelts)
    {
      apr_array_header_t *mergeinfo_props =
        apr_array_make(result_pool, 1, sizeof(svn_prop_t));
      int i;

      for (i = 0; i < props->nelts; i++)
        {
          svn_prop_t *prop = &APR_ARRAY_IDX(props, i, svn_prop_t);

          if (strcmp(prop->name, SVN_PROP_MERGEINFO) == 0)
            {
              APR_ARRAY_PUSH(mergeinfo_props, svn_prop_t) = *prop;
              break;
            }
        }
      props = mergeinfo_props;
    }

  if (props->nelts)
    {
      /* Issue #3383: We don't want mergeinfo from a foreign repos.

         If this is a merge from a foreign repository we must strip all
         incoming mergeinfo (including mergeinfo deletions). */
      if (! merge_b->same_repos)
        SVN_ERR(omit_mergeinfo_changes(&props, props, result_pool));

      if (merge_b->cb_table && merge_b->cb_table->adjust_mergeinfo)
        {
          SVN_ERR(merge_b->cb_table->adjust_mergeinfo(merge_b->cb_baton,
                                                      &props,
                                                      local_abspath,
                                                      scratch_pool,
                                                      result_pool));
        }
    }
  *prop_updates = props;

  /* Make a record in BATON if we find a PATH where mergeinfo is added
     where none existed previously or PATH is having its existing
     mergeinfo deleted. */
  if (merge_b->cb_table && merge_b->cb_table->mergeinfo_changed
      && props->nelts)
    {
      int i;

      for (i = 0; i < props->nelts; ++i)
        {
          svn_prop_t *prop = &APR_ARRAY_IDX(props, i, svn_prop_t);

          if (strcmp(prop->name, SVN_PROP_MERGEINFO) == 0)
            {
              /* Does LOCAL_ABSPATH have any pristine mergeinfo? */
              const svn_string_t *old_mergeinfo;
              apr_hash_t *pristine_props;

              SVN_ERR(svn_wc_get_pristine_props(&pristine_props,
                                                merge_b->ctx->wc_ctx,
                                                local_abspath,
                                                scratch_pool,
                                                scratch_pool));

              if (pristine_props)
                old_mergeinfo = svn_hash_gets(pristine_props, SVN_PROP_MERGEINFO);
              else
                old_mergeinfo = NULL;

              SVN_ERR(merge_b->cb_table->mergeinfo_changed(merge_b->cb_baton,
                                                           local_abspath,
                                                           old_mergeinfo,
                                                           prop->value,
                                                           scratch_pool));
            }
        }
    }

  return SVN_NO_ERROR;
}

#define CONFLICT_REASON_NONE       ((svn_wc_conflict_reason_t)-1)
#define CONFLICT_REASON_SKIP       ((svn_wc_conflict_reason_t)-2)
#define CONFLICT_REASON_SKIP_WC    ((svn_wc_conflict_reason_t)-3)

/* Baton used for testing trees for being editted while performing tree
   conflict detection for incoming deletes */
struct dir_delete_baton_t
{
  /* Reference to dir baton of directory that is the root of the deletion */
  struct merge_dir_baton_t *del_root;

  /* Boolean indicating that some edit is found. Allows avoiding more work */
  svn_boolean_t found_edit;

  /* A list of paths that are compared. Kept up to date until FOUND_EDIT is
     set to TRUE */
  apr_hash_t *compared_abspaths;
};

/* Baton for the merge_dir_*() functions. Initialized in merge_dir_opened() */
struct merge_dir_baton_t
{
  /* Reference to the parent baton, unless the parent is the anchor, in which
     case PARENT_BATON is NULL */
  struct merge_dir_baton_t *parent_baton;

  /* The pool containing this baton. Use for RESULT_POOL for storing in this
     baton */
  apr_pool_t *pool;

  /* This directory doesn't have a representation in the working copy, so any
     operation on it will be skipped and possibly cause a tree conflict on the
     shadow root */
  svn_boolean_t shadowed;

  /* This node or one of its descendants received operational changes from the
     merge. If this node is the shadow root its tree conflict status has been
     applied */
  svn_boolean_t edited;

  /* If a tree conflict will be installed once edited, it's reason. If a skip
     should be produced its reason. Otherwise CONFLICT_REASON_NONE for no tree
     conflict.

     Special values:
       CONFLICT_REASON_SKIP:
            The node will be skipped with content and property state as stored in
            SKIP_REASON.

       CONFLICT_REASON_SKIP_WC:
            The node will be skipped as an obstructing working copy.
   */
  svn_wc_conflict_reason_t tree_conflict_reason;
  svn_wc_conflict_action_t tree_conflict_action;
  svn_node_kind_t tree_conflict_local_node_kind;
  svn_node_kind_t tree_conflict_merge_left_node_kind;
  svn_node_kind_t tree_conflict_merge_right_node_kind;

  /* When TREE_CONFLICT_REASON is CONFLICT_REASON_SKIP, the skip state to
     add to the notification */
  svn_wc_notify_state_t skip_reason;

  /* TRUE if the node was added by this merge. Otherwise FALSE */
  svn_boolean_t added;
  svn_boolean_t add_is_replace; /* Add is second part of replace */

  /* TRUE if we are taking over an existing directory as addition, otherwise
     FALSE. */
  svn_boolean_t add_existing;

  /* NULL, or an hashtable mapping const char * local_abspaths to
     const char *kind mapping, containing deleted nodes that still need a delete
     notification (which may be a replaced notification if the node is not just
     deleted) */
  apr_hash_t *pending_deletes;

  /* NULL, or an hashtable mapping const char * LOCAL_ABSPATHs to
     a const svn_wc_conflict_description2_t * instance, describing the just
     installed conflict */
  apr_hash_t *new_tree_conflicts;

  /* If not NULL, a reference to the information of the delete test that is
     currently in progress. Allocated in the root-directory baton, referenced
     from all descendants */
  struct dir_delete_baton_t *delete_state;
};

/* Allocate new #merge_dir_baton_t structure in @a result_pool */
static struct merge_dir_baton_t *
create_dir_baton(apr_pool_t *result_pool)
{
  struct merge_dir_baton_t *db;

  db = apr_pcalloc(result_pool, sizeof(*db));
  db->pool = result_pool;
  db->tree_conflict_reason = CONFLICT_REASON_NONE;
  db->tree_conflict_action = svn_wc_conflict_action_edit;
  db->skip_reason = svn_wc_notify_state_unknown;

  return db;
}

/* Baton for the merge_dir_*() functions. Initialized in merge_file_opened() */
struct merge_file_baton_t
{
  /* Reference to the parent baton, unless the parent is the anchor, in which
     case PARENT_BATON is NULL */
  struct merge_dir_baton_t *parent_baton;

  /* This file doesn't have a representation in the working copy, so any
     operation on it will be skipped and possibly cause a tree conflict
     on the shadow root */
  svn_boolean_t shadowed;

  /* This node received operational changes from the merge. If this node
     is the shadow root its tree conflict status has been applied */
  svn_boolean_t edited;

  /* If a tree conflict will be installed once edited, it's reason. If a skip
     should be produced its reason. Some special values are defined. See the
     merge_dir_baton_t for an explanation. */
  svn_wc_conflict_reason_t tree_conflict_reason;
  svn_wc_conflict_action_t tree_conflict_action;
  svn_node_kind_t tree_conflict_local_node_kind;
  svn_node_kind_t tree_conflict_merge_left_node_kind;
  svn_node_kind_t tree_conflict_merge_right_node_kind;

  /* When TREE_CONFLICT_REASON is CONFLICT_REASON_SKIP, the skip state to
     add to the notification */
  svn_wc_notify_state_t skip_reason;

  /* TRUE if the node was added by this merge. Otherwise FALSE */
  svn_boolean_t added;
  svn_boolean_t add_is_replace; /* Add is second part of replace */
};

/* Allocate new #merge_file_baton_t structure in @a result_pool */
static struct merge_file_baton_t *
create_file_baton(apr_pool_t *result_pool)
{
  struct merge_file_baton_t *fb;

  fb = apr_pcalloc(result_pool, sizeof(*fb));
  fb->tree_conflict_reason = CONFLICT_REASON_NONE;
  fb->tree_conflict_action = svn_wc_conflict_action_edit;
  fb->skip_reason = svn_wc_notify_state_unknown;

  return fb;
}

/* Wrapper around the merge_b->cb_table->conflicted_path() function */
static svn_error_t *
notify_conflicted_path(merge_apply_processor_baton_t *merge_b,
                       const char *local_abspath,
                       svn_boolean_t tree_conflict,
                       apr_pool_t *scratch_pool)
{
  if (merge_b->cb_table && merge_b->cb_table->conflicted_path)
    {
      SVN_ERR(merge_b->cb_table->conflicted_path(merge_b->cb_baton,
                                                 local_abspath,
                                                 tree_conflict,
                                                 scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Wrapper around the merge_b->cb_table->updated_path() function */
static svn_error_t *
notify_updated_path(merge_apply_processor_baton_t *merge_b,
                    const char *local_abspath,
                    svn_wc_notify_action_t action,
                    svn_boolean_t parent_added,
                    apr_pool_t *scratch_pool)
{
  if (merge_b->cb_table && merge_b->cb_table->updated_path)
    {
      SVN_ERR(merge_b->cb_table->updated_path(merge_b->cb_baton,
                                              local_abspath,
                                              action,
                                              parent_added,
                                              scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Wrapper around the merge_b->cb_table->skipped_path() function */
static svn_error_t *
notify_skipped_path(merge_apply_processor_baton_t *merge_b,
                    const char *local_abspath,
                    apr_pool_t *scratch_pool)
{
  if (merge_b->cb_table && merge_b->cb_table->skipped_path)
    {
      SVN_ERR(merge_b->cb_table->skipped_path(merge_b->cb_baton,
                                              local_abspath,
                                              scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Record the skip for future processing and (later) produce the
   skip notification */
static svn_error_t *
record_skip(merge_apply_processor_baton_t *merge_b,
            const char *local_abspath,
            svn_node_kind_t kind,
            svn_wc_notify_action_t action,
            svn_wc_notify_state_t state,
            struct merge_dir_baton_t *pdb,
            apr_pool_t *scratch_pool)
{
  if (merge_b->record_only)
    return SVN_NO_ERROR; /* ### Why? - Legacy compatibility */

  if (!(pdb && pdb->shadowed))
    SVN_ERR(notify_skipped_path(merge_b, local_abspath, scratch_pool));

  if (merge_b->notify_func)
    {
      svn_wc_notify_t *notify;

      notify = svn_wc_create_notify(local_abspath, action, scratch_pool);
      notify->kind = kind;
      notify->content_state = notify->prop_state = state;

      merge_b->notify_func(merge_b->notify_baton, notify,
                           scratch_pool);
    }
  return SVN_NO_ERROR;
}

/* Record a tree conflict in the WC, unless this is a dry run or a record-
 * only merge, or if a tree conflict is already flagged for the VICTIM_PATH.
 * (The latter can happen if a merge-tracking-aware merge is doing multiple
 * editor drives because of a gap in the range of eligible revisions.)
 *
 * The tree conflict, with its victim specified by VICTIM_PATH, is
 * assumed to have happened during a merge using merge baton MERGE_B.
 *
 * ACTION and REASON correspond to the fields
 * of the same names in svn_wc_tree_conflict_description_t.
 */
static svn_error_t *
record_tree_conflict(merge_apply_processor_baton_t *merge_b,
                     const char *local_abspath,
                     struct merge_dir_baton_t *parent_baton,
                     svn_node_kind_t local_node_kind,
                     svn_node_kind_t merge_left_node_kind,
                     svn_node_kind_t merge_right_node_kind,
                     svn_wc_conflict_action_t action,
                     svn_wc_conflict_reason_t reason,
                     const svn_wc_conflict_description2_t *existing_conflict,
                     svn_boolean_t notify_tc,
                     apr_pool_t *scratch_pool)
{
  svn_wc_context_t *wc_ctx = merge_b->ctx->wc_ctx;

  if (merge_b->record_only)
    return SVN_NO_ERROR;

  SVN_ERR(notify_conflicted_path(merge_b, local_abspath, TRUE, scratch_pool));

  if (!merge_b->dry_run)
    {
       svn_wc_conflict_description2_t *conflict;
       const svn_wc_conflict_version_t *left;
       const svn_wc_conflict_version_t *right;
       apr_pool_t *result_pool = parent_baton ? parent_baton->pool
                                              : scratch_pool;
       svn_client__merge_source_t *source;

      if (reason == svn_wc_conflict_reason_deleted)
        {
          const char *moved_to_abspath;

          SVN_ERR(svn_wc__node_was_moved_away(&moved_to_abspath, NULL,
                                              wc_ctx, local_abspath,
                                              scratch_pool, scratch_pool));

          if (moved_to_abspath)
            {
              /* Local abspath itself has been moved away. If only a
                 descendant is moved away, we call the node itself deleted */
              reason = svn_wc_conflict_reason_moved_away;
            }
        }
      else if (reason == svn_wc_conflict_reason_added)
        {
          const char *moved_from_abspath;
          SVN_ERR(svn_wc__node_was_moved_here(&moved_from_abspath, NULL,
                                              wc_ctx, local_abspath,
                                              scratch_pool, scratch_pool));
          if (moved_from_abspath)
            reason = svn_wc_conflict_reason_moved_here;
        }

      source = &merge_b->merge_source;

      if (merge_b->cb_table && merge_b->cb_table->adjust_merge_source)
        {
          SVN_ERR(merge_b->cb_table->adjust_merge_source(
            merge_b->cb_baton, &source,
            local_abspath, action,
            scratch_pool, scratch_pool));
        }

      SVN_ERR(make_conflict_versions(&left, &right, local_abspath,
                                     merge_left_node_kind,
                                     merge_right_node_kind,
                                     source, merge_b->target,
                                     result_pool, scratch_pool));

      /* Fix up delete of file, add of dir replacement (or other way around) */
      if (existing_conflict != NULL && existing_conflict->src_left_version)
          left = existing_conflict->src_left_version;

      conflict = svn_wc_conflict_description_create_tree2(
                        local_abspath, local_node_kind,
                        svn_wc_operation_merge,
                        left, right, result_pool);

      conflict->action = action;
      conflict->reason = reason;

      /* May return SVN_ERR_WC_PATH_UNEXPECTED_STATUS */
      if (existing_conflict)
        SVN_ERR(svn_wc__del_tree_conflict(wc_ctx, local_abspath,
                                          scratch_pool));

      SVN_ERR(svn_wc__add_tree_conflict(merge_b->ctx->wc_ctx, conflict,
                                        scratch_pool));

      if (parent_baton)
        {
          if (! parent_baton->new_tree_conflicts)
            parent_baton->new_tree_conflicts = apr_hash_make(result_pool);

          svn_hash_sets(parent_baton->new_tree_conflicts,
                        apr_pstrdup(result_pool, local_abspath),
                        conflict);
        }

      /* ### TODO: Store in parent baton */
    }

  /* On a replacement we currently get two tree conflicts */
  if (merge_b->notify_func && notify_tc)
    {
      svn_wc_notify_t *notify;

      notify = svn_wc_create_notify(local_abspath, svn_wc_notify_tree_conflict,
                                    scratch_pool);
      notify->kind = local_node_kind;

      merge_b->notify_func(merge_b->notify_baton, notify,
                           scratch_pool);
    }

  return SVN_NO_ERROR;
}

/* Record the add for future processing and produce the
   update_add notification
 */
static svn_error_t *
record_update_add(merge_apply_processor_baton_t *merge_b,
                  const char *local_abspath,
                  svn_node_kind_t kind,
                  svn_boolean_t notify_replaced,
                  svn_boolean_t parent_added,
                  apr_pool_t *scratch_pool)
{
  SVN_ERR(notify_updated_path(merge_b, local_abspath,
                              svn_wc_notify_update_add,
                              parent_added,
                              scratch_pool));

  if (merge_b->notify_func)
    {
      svn_wc_notify_t *notify;
      svn_wc_notify_action_t action = svn_wc_notify_update_add;

      if (notify_replaced)
        action = svn_wc_notify_update_replace;

      notify = svn_wc_create_notify(local_abspath, action, scratch_pool);
      notify->kind = kind;

      merge_b->notify_func(merge_b->notify_baton, notify,
                           scratch_pool);
    }

  return SVN_NO_ERROR;
}

/* Record the update for future processing and produce the
   update_update notification */
static svn_error_t *
record_update_update(merge_apply_processor_baton_t *merge_b,
                     const char *local_abspath,
                     svn_node_kind_t kind,
                     svn_wc_notify_state_t content_state,
                     svn_wc_notify_state_t prop_state,
                     svn_boolean_t parent_added,
                     apr_pool_t *scratch_pool)
{
  SVN_ERR(notify_updated_path(merge_b, local_abspath,
                              svn_wc_notify_update_update,
                              parent_added,
                              scratch_pool));

  if (merge_b->notify_func)
    {
      svn_wc_notify_t *notify;

      notify = svn_wc_create_notify(local_abspath, svn_wc_notify_update_update,
                                    scratch_pool);
      notify->kind = kind;
      notify->content_state = content_state;
      notify->prop_state = prop_state;

      merge_b->notify_func(merge_b->notify_baton, notify,
                           scratch_pool);
    }

  return SVN_NO_ERROR;
}

/* Record the delete for future processing and for (later) producing the
   update_delete notification */
static svn_error_t *
record_update_delete(merge_apply_processor_baton_t *merge_b,
                     struct merge_dir_baton_t *parent_db,
                     const char *local_abspath,
                     svn_node_kind_t kind,
                     apr_pool_t *scratch_pool)
{
  SVN_ERR(notify_updated_path(merge_b, local_abspath,
                              svn_wc_notify_update_delete,
                              FALSE,
                              scratch_pool));

  if (parent_db)
    {
      const char *dup_abspath = apr_pstrdup(parent_db->pool, local_abspath);

      if (!parent_db->pending_deletes)
        parent_db->pending_deletes = apr_hash_make(parent_db->pool);

      svn_hash_sets(parent_db->pending_deletes, dup_abspath,
                    svn_node_kind_to_word(kind));
    }

  return SVN_NO_ERROR;
}

/* Notify the pending 'D'eletes, that were waiting to see if a matching 'A'dd
   might make them a 'R'eplace. */
static svn_error_t *
handle_pending_notifications(merge_apply_processor_baton_t *merge_b,
                             struct merge_dir_baton_t *db,
                             apr_pool_t *scratch_pool)
{
  if (merge_b->notify_func && db->pending_deletes)
    {
      apr_hash_index_t *hi;

      for (hi = apr_hash_first(scratch_pool, db->pending_deletes);
           hi;
           hi = apr_hash_next(hi))
        {
          const char *del_abspath = apr_hash_this_key(hi);
          svn_wc_notify_t *notify;

          notify = svn_wc_create_notify(del_abspath,
                                        svn_wc_notify_update_delete,
                                        scratch_pool);
          notify->kind = svn_node_kind_from_word(
                                    apr_hash_this_val(hi));

          merge_b->notify_func(merge_b->notify_baton,
                               notify, scratch_pool);
        }

      db->pending_deletes = NULL;
    }
  return SVN_NO_ERROR;
}

/* Helper function for the merge_dir_*() and merge_file_*() functions.

   Installs and notifies pre-recorded tree conflicts and skips for
   ancestors of operational merges
 */
static svn_error_t *
mark_dir_edited(merge_apply_processor_baton_t *merge_b,
                struct merge_dir_baton_t *db,
                const char *local_abspath,
                apr_pool_t *scratch_pool)
{
  /* ### Too much common code with mark_file_edited */
  if (db->edited)
    return SVN_NO_ERROR;

  if (db->parent_baton && !db->parent_baton->edited)
    {
      const char *dir_abspath = svn_dirent_dirname(local_abspath,
                                                   scratch_pool);

      SVN_ERR(mark_dir_edited(merge_b, db->parent_baton, dir_abspath,
                              scratch_pool));
    }

  db->edited = TRUE;

  if (! db->shadowed)
    return SVN_NO_ERROR; /* Easy out */

  if (db->parent_baton
      && db->parent_baton->delete_state
      && db->tree_conflict_reason != CONFLICT_REASON_NONE)
    {
      db->parent_baton->delete_state->found_edit = TRUE;
    }
  else if (db->tree_conflict_reason == CONFLICT_REASON_SKIP
           || db->tree_conflict_reason == CONFLICT_REASON_SKIP_WC)
    {
      /* open_directory() decided not to flag a tree conflict, but
         for clarity we produce a skip for this node that
         most likely isn't touched by the merge itself */

      if (merge_b->notify_func)
        {
          svn_wc_notify_t *notify;

          notify = svn_wc_create_notify(
                            local_abspath,
                            (db->tree_conflict_reason == CONFLICT_REASON_SKIP)
                                ? svn_wc_notify_skip
                                : svn_wc_notify_update_skip_obstruction,
                            scratch_pool);
          notify->kind = svn_node_dir;
          notify->content_state = notify->prop_state = db->skip_reason;

          merge_b->notify_func(merge_b->notify_baton,
                               notify,
                               scratch_pool);
        }

        SVN_ERR(notify_skipped_path(merge_b, local_abspath, scratch_pool));
    }
  else if (db->tree_conflict_reason != CONFLICT_REASON_NONE)
    {
      /* open_directory() decided that a tree conflict should be raised */

      SVN_ERR(record_tree_conflict(merge_b, local_abspath, db->parent_baton,
                                   db->tree_conflict_local_node_kind,
                                   db->tree_conflict_merge_left_node_kind,
                                   db->tree_conflict_merge_right_node_kind,
                                   db->tree_conflict_action,
                                   db->tree_conflict_reason,
                                   NULL, TRUE,
                                   scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* Helper function for the merge_file_*() functions.

   Installs and notifies pre-recorded tree conflicts and skips for
   ancestors of operational merges
 */
static svn_error_t *
mark_file_edited(merge_apply_processor_baton_t *merge_b,
                 struct merge_file_baton_t *fb,
                 const char *local_abspath,
                 apr_pool_t *scratch_pool)
{
  /* ### Too much common code with mark_dir_edited */
  if (fb->edited)
    return SVN_NO_ERROR;

  if (fb->parent_baton && !fb->parent_baton->edited)
    {
      const char *dir_abspath = svn_dirent_dirname(local_abspath,
                                                   scratch_pool);

      SVN_ERR(mark_dir_edited(merge_b, fb->parent_baton, dir_abspath,
                              scratch_pool));
    }

  fb->edited = TRUE;

  if (! fb->shadowed)
    return SVN_NO_ERROR; /* Easy out */

  if (fb->parent_baton
      && fb->parent_baton->delete_state
      && fb->tree_conflict_reason != CONFLICT_REASON_NONE)
    {
      fb->parent_baton->delete_state->found_edit = TRUE;
    }
  else if (fb->tree_conflict_reason == CONFLICT_REASON_SKIP
           || fb->tree_conflict_reason == CONFLICT_REASON_SKIP_WC)
    {
      /* open_directory() decided not to flag a tree conflict, but
         for clarity we produce a skip for this node that
         most likely isn't touched by the merge itself */

      if (merge_b->notify_func)
        {
          svn_wc_notify_t *notify;

          notify = svn_wc_create_notify(local_abspath, svn_wc_notify_skip,
                                        scratch_pool);
          notify->kind = svn_node_file;
          notify->content_state = notify->prop_state = fb->skip_reason;

          merge_b->notify_func(merge_b->notify_baton,
                               notify,
                               scratch_pool);
        }

      SVN_ERR(notify_skipped_path(merge_b, local_abspath, scratch_pool));
    }
  else if (fb->tree_conflict_reason != CONFLICT_REASON_NONE)
    {
      /* open_file() decided that a tree conflict should be raised */

      SVN_ERR(record_tree_conflict(merge_b, local_abspath, fb->parent_baton,
                                   fb->tree_conflict_local_node_kind,
                                   fb->tree_conflict_merge_left_node_kind,
                                   fb->tree_conflict_merge_right_node_kind,
                                   fb->tree_conflict_action,
                                   fb->tree_conflict_reason,
                                   NULL, TRUE,
                                   scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.

   Called before either merge_file_changed(), merge_file_added(),
   merge_file_deleted() or merge_file_closed(), unless it sets *SKIP to TRUE.

   When *SKIP is TRUE, the diff driver avoids work on getting the details
   for the closing callbacks.
 */
static svn_error_t *
merge_file_opened(void **new_file_baton,
                  svn_boolean_t *skip,
                  const char *relpath,
                  const svn_diff_source_t *left_source,
                  const svn_diff_source_t *right_source,
                  const svn_diff_source_t *copyfrom_source,
                  void *dir_baton,
                  const struct svn_diff_tree_processor_t *processor,
                  apr_pool_t *result_pool,
                  apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *pdb = dir_baton;
  struct merge_file_baton_t *fb = create_file_baton(result_pool);
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);

  if (left_source)
    fb->tree_conflict_merge_left_node_kind = svn_node_file;
  else
    fb->tree_conflict_merge_left_node_kind = svn_node_none;

  if (right_source)
    fb->tree_conflict_merge_right_node_kind = svn_node_file;
  else
    fb->tree_conflict_merge_right_node_kind = svn_node_none;

  *new_file_baton = fb;

  if (pdb)
    {
      fb->parent_baton = pdb;
      fb->shadowed = pdb->shadowed;
      fb->skip_reason = pdb->skip_reason;
    }

  if (fb->shadowed)
    {
      /* An ancestor is tree conflicted. Nothing to do here. */
    }
  else if (left_source != NULL)
    {
      /* Node is expected to be a file, which will be changed or deleted. */
      svn_boolean_t is_deleted;
      svn_boolean_t excluded;
      svn_depth_t parent_depth;

      if (! right_source)
        fb->tree_conflict_action = svn_wc_conflict_action_delete;

      {
        svn_wc_notify_state_t obstr_state;

        SVN_ERR(perform_obstruction_check(&obstr_state, &is_deleted, &excluded,
                                          &fb->tree_conflict_local_node_kind,
                                          &parent_depth,
                                          merge_b, local_abspath,
                                          scratch_pool));

        if (obstr_state != svn_wc_notify_state_inapplicable)
          {
            fb->shadowed = TRUE;
            fb->tree_conflict_reason = CONFLICT_REASON_SKIP;
            fb->skip_reason = obstr_state;
            return SVN_NO_ERROR;
          }

        if (is_deleted)
          fb->tree_conflict_local_node_kind = svn_node_none;
      }

      if (fb->tree_conflict_local_node_kind == svn_node_none)
        {
          fb->shadowed = TRUE;

          /* If this is not the merge target and the parent is too shallow to
             contain this directory, and the directory is not present
             via exclusion or depth filtering, skip it instead of recording
             a tree conflict.

             Non-inheritable mergeinfo will be recorded, allowing
             future merges into non-shallow working copies to merge
             changes we missed this time around. */
          if (pdb && (excluded
                      || (parent_depth != svn_depth_unknown &&
                          parent_depth < svn_depth_files)))
            {
                fb->shadowed = TRUE;

                fb->tree_conflict_reason = CONFLICT_REASON_SKIP;
                fb->skip_reason = svn_wc_notify_state_missing;
                return SVN_NO_ERROR;
            }

          if (is_deleted)
            fb->tree_conflict_reason = svn_wc_conflict_reason_deleted;
          else
            fb->tree_conflict_reason = svn_wc_conflict_reason_missing;

          /* ### Similar to directory */
          *skip = TRUE;
          SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));
          return SVN_NO_ERROR;
          /* ### /Similar */
        }
      else if (fb->tree_conflict_local_node_kind != svn_node_file)
        {
          svn_boolean_t added;
          fb->shadowed = TRUE;

          SVN_ERR(svn_wc__node_is_added(&added, merge_b->ctx->wc_ctx,
                                        local_abspath, scratch_pool));

          fb->tree_conflict_reason = added ? svn_wc_conflict_reason_added
                                           : svn_wc_conflict_reason_obstructed;

          /* ### Similar to directory */
          *skip = TRUE;
          SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));
          return SVN_NO_ERROR;
          /* ### /Similar */
        }

      if (! right_source)
        {
          /* We want to delete the directory */
          fb->tree_conflict_action = svn_wc_conflict_action_delete;
          SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));

          if (fb->shadowed)
            {
              return SVN_NO_ERROR; /* Already set a tree conflict */
            }

          /* Comparison mode to verify for delete tree conflicts? */
          if (pdb && pdb->delete_state
              && pdb->delete_state->found_edit)
            {
              /* Earlier nodes found a conflict. Done. */
              *skip = TRUE;
            }
        }
    }
  else
    {
      const svn_wc_conflict_description2_t *old_tc = NULL;

      /* The node doesn't exist pre-merge: We have an addition */
      fb->added = TRUE;
      fb->tree_conflict_action = svn_wc_conflict_action_add;

      if (pdb && pdb->pending_deletes
          && svn_hash_gets(pdb->pending_deletes, local_abspath))
        {
          fb->add_is_replace = TRUE;
          fb->tree_conflict_action = svn_wc_conflict_action_replace;

          svn_hash_sets(pdb->pending_deletes, local_abspath, NULL);
        }

      if (pdb
          && pdb->new_tree_conflicts
          && (old_tc = svn_hash_gets(pdb->new_tree_conflicts, local_abspath)))
        {
          fb->tree_conflict_action = svn_wc_conflict_action_replace;
          fb->tree_conflict_reason = old_tc->reason;

          /* Update the tree conflict to store that this is a replace */
          SVN_ERR(record_tree_conflict(merge_b, local_abspath, pdb,
                                       old_tc->node_kind,
                                       old_tc->src_left_version->node_kind,
                                       svn_node_file,
                                       fb->tree_conflict_action,
                                       fb->tree_conflict_reason,
                                       old_tc, FALSE,
                                       scratch_pool));

          if (old_tc->reason == svn_wc_conflict_reason_deleted
              || old_tc->reason == svn_wc_conflict_reason_moved_away)
            {
              /* Issue #3806: Incoming replacements on local deletes produce
                 inconsistent result.

                 In this specific case we can continue applying the add part
                 of the replacement. */
            }
          else
            {
              *skip = TRUE;

              return SVN_NO_ERROR;
            }
        }
      else if (! (merge_b->dry_run
                  && ((pdb && pdb->added) || fb->add_is_replace)))
        {
          svn_wc_notify_state_t obstr_state;
          svn_boolean_t is_deleted;

          SVN_ERR(perform_obstruction_check(&obstr_state, &is_deleted, NULL,
                                            &fb->tree_conflict_local_node_kind,
                                            NULL, merge_b, local_abspath,
                                            scratch_pool));

          if (obstr_state != svn_wc_notify_state_inapplicable)
            {
              /* Skip the obstruction */
              fb->shadowed = TRUE;
              fb->tree_conflict_reason = CONFLICT_REASON_SKIP;
              fb->skip_reason = obstr_state;
            }
          else if (fb->tree_conflict_local_node_kind != svn_node_none
                   && !is_deleted)
            {
              /* Set a tree conflict */
              svn_boolean_t added;

              fb->shadowed = TRUE;
              SVN_ERR(svn_wc__node_is_added(&added, merge_b->ctx->wc_ctx,
                                            local_abspath, scratch_pool));

              fb->tree_conflict_reason = added ? svn_wc_conflict_reason_added
                                               : svn_wc_conflict_reason_obstructed;
            }
        }

      /* Handle pending conflicts */
      SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_file_opened() when a node receives only text and/or
 * property changes between LEFT_SOURCE and RIGHT_SOURCE.
 *
 * left_file and right_file can be NULL when the file is not modified.
 * left_props and right_props are always available.
 */
static svn_error_t *
merge_file_changed(const char *relpath,
                  const svn_diff_source_t *left_source,
                  const svn_diff_source_t *right_source,
                  const char *left_file,
                  const char *right_file,
                  /*const*/ apr_hash_t *left_props,
                  /*const*/ apr_hash_t *right_props,
                  svn_boolean_t file_modified,
                  const apr_array_header_t *prop_changes,
                  void *file_baton,
                  const struct svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_file_baton_t *fb = file_baton;
  svn_client_ctx_t *ctx = merge_b->ctx;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);
  const svn_wc_conflict_version_t *left;
  const svn_wc_conflict_version_t *right;
  svn_wc_notify_state_t text_state;
  svn_wc_notify_state_t property_state;

  SVN_ERR_ASSERT(local_abspath && svn_dirent_is_absolute(local_abspath));
  SVN_ERR_ASSERT(!left_file || svn_dirent_is_absolute(left_file));
  SVN_ERR_ASSERT(!right_file || svn_dirent_is_absolute(right_file));

  SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));

  if (fb->shadowed)
    {
      if (fb->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_file,
                              svn_wc_notify_update_shadowed_update,
                              fb->skip_reason, fb->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  /* This callback is essentially no more than a wrapper around
     svn_wc_merge6().  Thank goodness that all the
     diff-editor-mechanisms are doing the hard work of getting the
     fulltexts! */

  property_state = svn_wc_notify_state_unchanged;
  text_state = svn_wc_notify_state_unchanged;

  SVN_ERR(prepare_merge_props_changed(&prop_changes, local_abspath,
                                      prop_changes, merge_b,
                                      scratch_pool, scratch_pool));

  SVN_ERR(make_conflict_versions(&left, &right, local_abspath,
                                 svn_node_file, svn_node_file,
                                 &merge_b->merge_source, merge_b->target,
                                 scratch_pool, scratch_pool));

  /* Do property merge now, if we are not going to perform a text merge */
  if ((merge_b->record_only || !left_file) && prop_changes->nelts)
    {
      SVN_ERR(svn_wc_merge_props3(&property_state, ctx->wc_ctx, local_abspath,
                                  left, right,
                                  left_props, prop_changes,
                                  merge_b->dry_run,
                                  NULL, NULL,
                                  ctx->cancel_func, ctx->cancel_baton,
                                  scratch_pool));
      if (property_state == svn_wc_notify_state_conflicted)
        {
          SVN_ERR(notify_conflicted_path(merge_b, local_abspath, FALSE,
                                         scratch_pool));
        }
    }

  /* Easy out: We are only applying mergeinfo differences. */
  if (merge_b->record_only)
    {
      /* NO-OP */
    }
  else if (left_file)
    {
      svn_boolean_t has_local_mods;
      enum svn_wc_merge_outcome_t content_outcome;
      const char *target_label;
      const char *left_label;
      const char *right_label;
      const char *path_ext = "";

      if (merge_b->ext_patterns && merge_b->ext_patterns->nelts)
        {
          svn_path_splitext(NULL, &path_ext, local_abspath, scratch_pool);
          if (! (*path_ext
                 && svn_cstring_match_glob_list(path_ext,
                                                merge_b->ext_patterns)))
            {
              path_ext = "";
            }
        }

      /* xgettext: the '.working', '.merge-left.r%ld' and
         '.merge-right.r%ld' strings are used to tag onto a file
         name in case of a merge conflict */

      target_label = apr_psprintf(scratch_pool, _(".working%s%s"),
                                  *path_ext ? "." : "", path_ext);
      left_label = apr_psprintf(scratch_pool,
                                _(".merge-left.r%ld%s%s"),
                                left_source->revision,
                                *path_ext ? "." : "", path_ext);
      right_label = apr_psprintf(scratch_pool,
                                 _(".merge-right.r%ld%s%s"),
                                 right_source->revision,
                                 *path_ext ? "." : "", path_ext);

      SVN_ERR(svn_wc_text_modified_p2(&has_local_mods, ctx->wc_ctx,
                                      local_abspath, FALSE, scratch_pool));

      /* Do property merge and text merge in one step so that keyword expansion
         takes into account the new property values. */
      SVN_ERR(svn_wc_merge6(&content_outcome, &property_state, ctx->wc_ctx,
                            left_file, right_file, local_abspath,
                            left_label, right_label, target_label,
                            left, right,
                            merge_b->dry_run, merge_b->diff3_cmd,
                            merge_b->merge_options,
                            left_props, prop_changes,
                            NULL, NULL,
                            ctx->cancel_func,
                            ctx->cancel_baton,
                            scratch_pool));

      if (content_outcome == svn_wc_merge_conflict
          || property_state == svn_wc_notify_state_conflicted)
        {
          SVN_ERR(notify_conflicted_path(merge_b, local_abspath, FALSE,
                                         scratch_pool));
        }

      if (content_outcome == svn_wc_merge_conflict)
        text_state = svn_wc_notify_state_conflicted;
      else if (has_local_mods
               && content_outcome != svn_wc_merge_unchanged)
        text_state = svn_wc_notify_state_merged;
      else if (content_outcome == svn_wc_merge_merged)
        text_state = svn_wc_notify_state_changed;
      else if (content_outcome == svn_wc_merge_no_merge)
        text_state = svn_wc_notify_state_missing;
      else /* merge_outcome == svn_wc_merge_unchanged */
        text_state = svn_wc_notify_state_unchanged;
    }

  if (text_state == svn_wc_notify_state_conflicted
      || text_state == svn_wc_notify_state_merged
      || text_state == svn_wc_notify_state_changed
      || property_state == svn_wc_notify_state_conflicted
      || property_state == svn_wc_notify_state_merged
      || property_state == svn_wc_notify_state_changed)
    {
      SVN_ERR(record_update_update(merge_b, local_abspath, svn_node_file,
                                   text_state, property_state,
                                   fb->parent_baton && fb->parent_baton->added,
                                   scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_file_opened() when a node doesn't exist in LEFT_SOURCE,
 * but does in RIGHT_SOURCE.
 *
 * When a node is replaced instead of just added a separate opened+deleted will
 * be invoked before the current open+added.
 */
static svn_error_t *
merge_file_added(const char *relpath,
                 const svn_diff_source_t *copyfrom_source,
                 const svn_diff_source_t *right_source,
                 const char *copyfrom_file,
                 const char *right_file,
                 /*const*/ apr_hash_t *copyfrom_props,
                 /*const*/ apr_hash_t *right_props,
                 void *file_baton,
                 const struct svn_diff_tree_processor_t *processor,
                 apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_file_baton_t *fb = file_baton;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);
  apr_hash_t *pristine_props;
  apr_hash_t *new_props;

  SVN_ERR_ASSERT(svn_dirent_is_absolute(local_abspath));

  SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));

  if (fb->shadowed)
    {
      if (fb->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_file,
                              svn_wc_notify_update_shadowed_add,
                              fb->skip_reason, fb->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  /* Easy out: We are only applying mergeinfo differences. */
  if (merge_b->record_only)
    {
      return SVN_NO_ERROR;
    }

  if (!merge_b->dry_run)
    {
      const char *copyfrom_url;
      svn_revnum_t copyfrom_rev;
      svn_stream_t *new_contents, *pristine_contents;

      /* If this is a merge from the same repository as our
         working copy, we handle adds as add-with-history.
         Otherwise, we'll use a pure add. */
      if (merge_b->same_repos)
        {
          copyfrom_url = svn_path_url_add_component2(
                                       merge_b->merge_source.loc2->url,
                                       relpath, scratch_pool);
          copyfrom_rev = right_source->revision;
          SVN_ERR(check_repos_match(merge_b->target, local_abspath,
                                    copyfrom_url, scratch_pool));
          SVN_ERR(svn_stream_open_readonly(&pristine_contents,
                                           right_file,
                                           scratch_pool,
                                           scratch_pool));
          new_contents = NULL; /* inherit from new_base_contents */

          pristine_props = right_props; /* Includes last_* information */
          new_props = NULL; /* No local changes */

          if (merge_b->cb_table && merge_b->cb_table->mergeinfo_changed)
            {
              const svn_string_t *new_mergeinfo
                = svn_hash_gets(pristine_props, SVN_PROP_MERGEINFO);

              SVN_ERR(merge_b->cb_table->mergeinfo_changed(merge_b->cb_baton,
                                                           local_abspath,
                                                           NULL,
                                                           new_mergeinfo,
                                                           scratch_pool));
            }
        }
      else
        {
          apr_array_header_t *regular_props;

          copyfrom_url = NULL;
          copyfrom_rev = SVN_INVALID_REVNUM;

          pristine_contents = svn_stream_empty(scratch_pool);
          SVN_ERR(svn_stream_open_readonly(&new_contents, right_file,
                                           scratch_pool, scratch_pool));

          pristine_props = apr_hash_make(scratch_pool); /* Local addition */

          /* We don't want any foreign properties */
          SVN_ERR(svn_categorize_props(svn_prop_hash_to_array(right_props,
                                                              scratch_pool),
                                       NULL, NULL, &regular_props,
                                       scratch_pool));

          new_props = svn_prop_array_to_hash(regular_props, scratch_pool);

          /* Issue #3383: We don't want mergeinfo from a foreign repository. */
          svn_hash_sets(new_props, SVN_PROP_MERGEINFO, NULL);
        }

      /* Do everything like if we had called 'svn cp PATH1 PATH2'. */
      SVN_ERR(svn_wc_add_repos_file4(merge_b->ctx->wc_ctx,
                                      local_abspath,
                                      pristine_contents,
                                      new_contents,
                                      pristine_props, new_props,
                                      copyfrom_url, copyfrom_rev,
                                      merge_b->ctx->cancel_func,
                                      merge_b->ctx->cancel_baton,
                                      scratch_pool));

#if TODO_USE_SLEEP
      /* Caller must call svn_sleep_for_timestamps() */
      *merge_b->use_sleep = TRUE;
#endif
    }

  SVN_ERR(record_update_add(merge_b, local_abspath, svn_node_file,
                            fb->add_is_replace,
                            fb->parent_baton && fb->parent_baton->added,
                            scratch_pool));

  return SVN_NO_ERROR;
}

/* Compare the two sets of properties PROPS1 and PROPS2, ignoring the
 * "svn:mergeinfo" property, and noticing only "normal" props. Set *SAME to
 * true if the rest of the properties are identical or false if they differ.
 */
static svn_error_t *
properties_same_p(svn_boolean_t *same,
                  apr_hash_t *props1,
                  apr_hash_t *props2,
                  apr_pool_t *scratch_pool)
{
  apr_array_header_t *prop_changes;
  int i, diffs;

  /* Examine the properties that differ */
  SVN_ERR(svn_prop_diffs(&prop_changes, props1, props2, scratch_pool));
  diffs = 0;
  for (i = 0; i < prop_changes->nelts; i++)
    {
      const char *pname = APR_ARRAY_IDX(prop_changes, i, svn_prop_t).name;

      /* Count the properties we're interested in; ignore the rest */
      if (svn_wc_is_normal_prop(pname)
          && strcmp(pname, SVN_PROP_MERGEINFO) != 0)
        diffs++;
    }
  *same = (diffs == 0);
  return SVN_NO_ERROR;
}

/* Compare the file OLDER_ABSPATH (together with its normal properties in
 * ORIGINAL_PROPS which may also contain WC props and entry props) with the
 * versioned file MINE_ABSPATH (together with its versioned properties).
 * Set *SAME to true if they are the same or false if they differ, ignoring
 * the "svn:mergeinfo" property, and ignoring differences in keyword
 * expansion and end-of-line style. */
static svn_error_t *
files_same_p(svn_boolean_t *same,
             const char *older_abspath,
             apr_hash_t *original_props,
             const char *mine_abspath,
             svn_wc_context_t *wc_ctx,
             apr_pool_t *scratch_pool)
{
  apr_hash_t *working_props;

  SVN_ERR(svn_wc_prop_list2(&working_props, wc_ctx, mine_abspath,
                            scratch_pool, scratch_pool));

  /* Compare the properties */
  SVN_ERR(properties_same_p(same, original_props, working_props,
                            scratch_pool));
  if (*same)
    {
      svn_stream_t *mine_stream;
      svn_stream_t *older_stream;
      svn_string_t *special = svn_hash_gets(working_props, SVN_PROP_SPECIAL);
      svn_string_t *eol_style = svn_hash_gets(working_props, SVN_PROP_EOL_STYLE);
      svn_string_t *keywords = svn_hash_gets(working_props, SVN_PROP_KEYWORDS);

      /* Compare the file content, translating 'mine' to 'normal' form. */
      if (special != NULL)
        SVN_ERR(svn_subst_read_specialfile(&mine_stream, mine_abspath,
                                           scratch_pool, scratch_pool));
      else
        SVN_ERR(svn_stream_open_readonly(&mine_stream, mine_abspath,
                                         scratch_pool, scratch_pool));

      if (!special && (eol_style || keywords))
        {
          apr_hash_t *kw = NULL;
          const char *eol = NULL;
          svn_subst_eol_style_t style;

          /* We used to use svn_client__get_normalized_stream() here, but
             that doesn't work in 100% of the cases because it doesn't
             convert EOLs to the repository form; just to '\n'.
           */

          if (eol_style)
            {
              svn_subst_eol_style_from_value(&style, &eol, eol_style->data);

              if (style == svn_subst_eol_style_native)
                eol = SVN_SUBST_NATIVE_EOL_STR;
              else if (style != svn_subst_eol_style_fixed
                       && style != svn_subst_eol_style_none)
                return svn_error_create(SVN_ERR_IO_UNKNOWN_EOL, NULL, NULL);
            }

          if (keywords)
            SVN_ERR(svn_subst_build_keywords3(&kw, keywords->data, "", "",
                                              "", 0, "", scratch_pool));

          mine_stream = svn_subst_stream_translated(
            mine_stream, eol, FALSE, kw, FALSE, scratch_pool);
        }

      SVN_ERR(svn_stream_open_readonly(&older_stream, older_abspath,
                                       scratch_pool, scratch_pool));

      SVN_ERR(svn_stream_contents_same2(same, mine_stream, older_stream,
                                        scratch_pool));

    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_file_opened() when a node does exist in LEFT_SOURCE, but
 * no longer exists (or is replaced) in RIGHT_SOURCE.
 *
 * When a node is replaced instead of just added a separate opened+added will
 * be invoked after the current open+deleted.
 */
static svn_error_t *
merge_file_deleted(const char *relpath,
                   const svn_diff_source_t *left_source,
                   const char *left_file,
                   /*const*/ apr_hash_t *left_props,
                   void *file_baton,
                   const struct svn_diff_tree_processor_t *processor,
                   apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_file_baton_t *fb = file_baton;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);
  svn_boolean_t same;

  SVN_ERR(mark_file_edited(merge_b, fb, local_abspath, scratch_pool));

  if (fb->shadowed)
    {
      if (fb->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_file,
                              svn_wc_notify_update_shadowed_delete,
                              fb->skip_reason, fb->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  /* Easy out: We are only applying mergeinfo differences. */
  if (merge_b->record_only)
    {
      return SVN_NO_ERROR;
    }

  /* If the files are identical, attempt deletion */
  if (merge_b->force_delete)
    same = TRUE;
  else
    SVN_ERR(files_same_p(&same, left_file, left_props,
                         local_abspath, merge_b->ctx->wc_ctx,
                         scratch_pool));

  if (fb->parent_baton
      && fb->parent_baton->delete_state)
    {
      if (same)
        {
          /* Note that we checked this file */
          store_path(fb->parent_baton->delete_state->compared_abspaths,
                     local_abspath);
        }
      else
        {
          /* We found some modification. Parent should raise a tree conflict */
          fb->parent_baton->delete_state->found_edit = TRUE;
        }

      return SVN_NO_ERROR;
    }
  else if (same)
    {
      if (!merge_b->dry_run)
        SVN_ERR(svn_wc_delete4(merge_b->ctx->wc_ctx, local_abspath,
                               FALSE /* keep_local */, FALSE /* unversioned */,
                               merge_b->ctx->cancel_func,
                               merge_b->ctx->cancel_baton,
                               NULL, NULL /* no notify */,
                               scratch_pool));

      if (merge_b->cb_table && merge_b->cb_table->mergeinfo_changed)
        {
          /* Does LOCAL_ABSPATH have any pristine mergeinfo? */
          const svn_string_t *old_mergeinfo;

          if (left_props)
            old_mergeinfo = svn_hash_gets(left_props, SVN_PROP_MERGEINFO);
          else
            old_mergeinfo = NULL;

          /* Record that we might have deleted mergeinfo */
          SVN_ERR(merge_b->cb_table->mergeinfo_changed(merge_b->cb_baton, local_abspath,
                                                       old_mergeinfo,
                                                       NULL,
                                                       scratch_pool));
        }

      /* And notify the deletion */
      SVN_ERR(record_update_delete(merge_b, fb->parent_baton, local_abspath,
                                   svn_node_file, scratch_pool));
    }
  else
    {
      /* The files differ, so raise a conflict instead of deleting */

      /* This is use case 5 described in the paper attached to issue
       * #2282.  See also notes/tree-conflicts/detection.txt
       */
      SVN_ERR(record_tree_conflict(merge_b, local_abspath, fb->parent_baton,
                                   svn_node_file,
                                   svn_node_file,
                                   svn_node_none,
                                   svn_wc_conflict_action_delete,
                                   svn_wc_conflict_reason_edited,
                                   NULL, TRUE,
                                   scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.

   Called before either merge_dir_changed(), merge_dir_added(),
   merge_dir_deleted() or merge_dir_closed(), unless it sets *SKIP to TRUE.

   After this call and before the close call, all descendants will receive
   their changes, unless *SKIP_CHILDREN is set to TRUE.

   When *SKIP is TRUE, the diff driver avoids work on getting the details
   for the closing callbacks.

   The SKIP and SKIP_DESCENDANTS work independently.
 */
static svn_error_t *
merge_dir_opened(void **new_dir_baton,
                 svn_boolean_t *skip,
                 svn_boolean_t *skip_children,
                 const char *relpath,
                 const svn_diff_source_t *left_source,
                 const svn_diff_source_t *right_source,
                 const svn_diff_source_t *copyfrom_source,
                 void *parent_dir_baton,
                 const struct svn_diff_tree_processor_t *processor,
                 apr_pool_t *result_pool,
                 apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = create_dir_baton(result_pool);
  struct merge_dir_baton_t *pdb = parent_dir_baton;
  const char *local_abspath;

  *new_dir_baton = db;

  /* Easy out: we are opening a fake directory, that doesn't have a relpath.
   * This may be used as a directory of replace-single-file merge.
   */
  if (relpath == NULL)
    return SVN_NO_ERROR;

  local_abspath = svn_dirent_join(merge_b->target->abspath,
                                  relpath, scratch_pool);

  if (left_source)
    db->tree_conflict_merge_left_node_kind = svn_node_dir;
  else
    db->tree_conflict_merge_left_node_kind = svn_node_none;

  if (right_source)
    db->tree_conflict_merge_right_node_kind = svn_node_dir;
  else
    db->tree_conflict_merge_right_node_kind = svn_node_none;

  if (pdb)
    {
      db->parent_baton = pdb;
      db->shadowed = pdb->shadowed;
      db->skip_reason = pdb->skip_reason;
    }

  if (db->shadowed)
    {
      /* An ancestor is tree conflicted. Nothing to do here. */
      if (! left_source)
        db->added = TRUE;
    }
  else if (left_source != NULL)
    {
      /* Node is expected to be a directory. */
      svn_boolean_t is_deleted;
      svn_boolean_t excluded;
      svn_depth_t parent_depth;

      if (! right_source)
          db->tree_conflict_action = svn_wc_conflict_action_delete;

      /* Check for an obstructed or missing node on disk. */
      {
        svn_wc_notify_state_t obstr_state;
        SVN_ERR(perform_obstruction_check(&obstr_state, &is_deleted, &excluded,
                                          &db->tree_conflict_local_node_kind,
                                          &parent_depth, merge_b,
                                          local_abspath, scratch_pool));

        if (obstr_state != svn_wc_notify_state_inapplicable)
          {
            db->shadowed = TRUE;

            if (obstr_state == svn_wc_notify_state_obstructed)
              {
                svn_boolean_t is_wcroot;

                SVN_ERR(svn_wc_check_root(&is_wcroot, NULL, NULL,
                                        merge_b->ctx->wc_ctx,
                                        local_abspath, scratch_pool));

                if (is_wcroot)
                  {
                    db->tree_conflict_reason = CONFLICT_REASON_SKIP_WC;
                    return SVN_NO_ERROR;
                  }
              }

            db->tree_conflict_reason = CONFLICT_REASON_SKIP;
            db->skip_reason = obstr_state;

            if (! right_source)
              {
                *skip = *skip_children = TRUE;
                SVN_ERR(mark_dir_edited(merge_b, db, local_abspath,
                                        scratch_pool));
              }

            return SVN_NO_ERROR;
          }

        if (is_deleted)
          db->tree_conflict_local_node_kind = svn_node_none;
      }

      if (db->tree_conflict_local_node_kind == svn_node_none)
        {
          db->shadowed = TRUE;

          /* If this is not the merge target and the parent is too shallow to
             contain this directory, and the directory is not presen
             via exclusion or depth filtering, skip it instead of recording
             a tree conflict.

             Non-inheritable mergeinfo will be recorded, allowing
             future merges into non-shallow working copies to merge
             changes we missed this time around. */
          if (pdb && (excluded
                      || (parent_depth != svn_depth_unknown &&
                          parent_depth < svn_depth_immediates)))
            {
              db->shadowed = TRUE;

              db->tree_conflict_reason = CONFLICT_REASON_SKIP;
              db->skip_reason = svn_wc_notify_state_missing;

              return SVN_NO_ERROR;
            }

          if (is_deleted)
            db->tree_conflict_reason = svn_wc_conflict_reason_deleted;
          else
            db->tree_conflict_reason = svn_wc_conflict_reason_missing;

          /* ### To avoid breaking tests */
          *skip = TRUE;
          *skip_children = TRUE;
          SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));
          return SVN_NO_ERROR;
          /* ### /avoid breaking tests */
        }
      else if (db->tree_conflict_local_node_kind != svn_node_dir)
        {
          svn_boolean_t added;

          db->shadowed = TRUE;
          SVN_ERR(svn_wc__node_is_added(&added, merge_b->ctx->wc_ctx,
                                        local_abspath, scratch_pool));

          db->tree_conflict_reason = added ? svn_wc_conflict_reason_added
                                           : svn_wc_conflict_reason_obstructed;

          /* ### To avoid breaking tests */
          *skip = TRUE;
          *skip_children = TRUE;
          SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));
          return SVN_NO_ERROR;
          /* ### /avoid breaking tests */
        }

      if (! right_source)
        {
          /* We want to delete the directory */
          /* Mark PB edited now? */
          db->tree_conflict_action = svn_wc_conflict_action_delete;
          SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));

          if (db->shadowed)
            {
              *skip_children = TRUE;
              return SVN_NO_ERROR; /* Already set a tree conflict */
            }

          db->delete_state = (pdb != NULL) ? pdb->delete_state : NULL;

          if (db->delete_state && db->delete_state->found_edit)
            {
              /* A sibling found a conflict. Done. */
              *skip = TRUE;
              *skip_children = TRUE;
            }
          else if (merge_b->force_delete)
            {
              /* No comparison necessary */
              *skip_children = TRUE;
            }
          else if (! db->delete_state)
            {
              /* Start descendant comparison */
              db->delete_state = apr_pcalloc(db->pool,
                                             sizeof(*db->delete_state));

              db->delete_state->del_root = db;
              db->delete_state->compared_abspaths = apr_hash_make(db->pool);
            }
        }
    }
  else
    {
      const svn_wc_conflict_description2_t *old_tc = NULL;

      /* The node doesn't exist pre-merge: We have an addition */
      db->added = TRUE;
      db->tree_conflict_action = svn_wc_conflict_action_add;

      if (pdb && pdb->pending_deletes
          && svn_hash_gets(pdb->pending_deletes, local_abspath))
        {
          db->add_is_replace = TRUE;
          db->tree_conflict_action = svn_wc_conflict_action_replace;

          svn_hash_sets(pdb->pending_deletes, local_abspath, NULL);
        }

      if (pdb
          && pdb->new_tree_conflicts
          && (old_tc = svn_hash_gets(pdb->new_tree_conflicts, local_abspath)))
        {
          db->tree_conflict_action = svn_wc_conflict_action_replace;
          db->tree_conflict_reason = old_tc->reason;

          if (old_tc->reason == svn_wc_conflict_reason_deleted
             || old_tc->reason == svn_wc_conflict_reason_moved_away)
            {
              /* Issue #3806: Incoming replacements on local deletes produce
                 inconsistent result.

                 In this specific case we can continue applying the add part
                 of the replacement. */
            }
          else
            {
              *skip = TRUE;
              *skip_children = TRUE;

              /* Update the tree conflict to store that this is a replace */
              SVN_ERR(record_tree_conflict(merge_b, local_abspath, pdb,
                                           old_tc->node_kind,
                                           old_tc->src_left_version->node_kind,
                                           svn_node_dir,
                                           db->tree_conflict_action,
                                           db->tree_conflict_reason,
                                           old_tc, FALSE,
                                           scratch_pool));

              return SVN_NO_ERROR;
            }
        }

      if (! (merge_b->dry_run
             && ((pdb && pdb->added) || db->add_is_replace)))
        {
          svn_wc_notify_state_t obstr_state;
          svn_boolean_t is_deleted;

          SVN_ERR(perform_obstruction_check(&obstr_state, &is_deleted, NULL,
                                            &db->tree_conflict_local_node_kind,
                                            NULL, merge_b, local_abspath,
                                            scratch_pool));

          /* In this case of adding a directory, we have an exception to the
           * usual "skip if it's inconsistent" rule. If the directory exists
           * on disk unexpectedly, we simply make it versioned, because we can
           * do so without risk of destroying data. Only skip if it is
           * versioned but unexpectedly missing from disk, or is unversioned
           * but obstructed by a node of the wrong kind. */
          if (obstr_state == svn_wc_notify_state_obstructed
              && (is_deleted ||
                  db->tree_conflict_local_node_kind == svn_node_none))
            {
              svn_node_kind_t disk_kind;

              SVN_ERR(svn_io_check_path(local_abspath, &disk_kind,
                                        scratch_pool));

              if (disk_kind == svn_node_dir)
                {
                  obstr_state = svn_wc_notify_state_inapplicable;
                  db->add_existing = TRUE; /* Take over existing directory */
                }
            }

          if (obstr_state != svn_wc_notify_state_inapplicable)
            {
              /* Skip the obstruction */
              db->shadowed = TRUE;
              db->tree_conflict_reason = CONFLICT_REASON_SKIP;
              db->skip_reason = obstr_state;
            }
          else if (db->tree_conflict_local_node_kind != svn_node_none
                   && !is_deleted)
            {
              /* Set a tree conflict */
              svn_boolean_t added;
              db->shadowed = TRUE;

              SVN_ERR(svn_wc__node_is_added(&added, merge_b->ctx->wc_ctx,
                                            local_abspath, scratch_pool));

              db->tree_conflict_reason = added ? svn_wc_conflict_reason_added
                                               : svn_wc_conflict_reason_obstructed;
            }
        }

      /* Handle pending conflicts */
      SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));

      if (db->shadowed)
        {
          /* Notified and done. Skip children? */
        }
      else if (merge_b->record_only)
        {
          /* Ok, we are done for this node and its descendants */
          *skip = TRUE;
          *skip_children = TRUE;
        }
      else if (! merge_b->dry_run)
        {
          /* Create the directory on disk, to allow descendants to be added */
          if (! db->add_existing)
            SVN_ERR(svn_io_dir_make(local_abspath, APR_OS_DEFAULT,
                                    scratch_pool));

          if (old_tc)
            {
              /* svn_wc_add4 and svn_wc_add_from_disk3 can't add a node
                 over an existing tree conflict */

              /* ### These functions should take some tree conflict argument
                     and allow overwriting the tc when one is passed */

              SVN_ERR(svn_wc__del_tree_conflict(merge_b->ctx->wc_ctx,
                                                local_abspath,
                                                scratch_pool));
            }

          if (merge_b->same_repos)
            {
              const char *original_url;

              original_url = svn_path_url_add_component2(
                                        merge_b->merge_source.loc2->url,
                                        relpath, scratch_pool);

              /* Limitation (aka HACK):
                 We create a newly added directory with an original URL and
                 revision as that in the repository, but without its properties
                 and children.

                 When the merge is cancelled before the final dir_added(), the
                 copy won't really represent the in-repository state of the node.
               */
              SVN_ERR(svn_wc_add4(merge_b->ctx->wc_ctx, local_abspath,
                                  svn_depth_infinity,
                                  original_url,
                                  right_source->revision,
                                  merge_b->ctx->cancel_func,
                                  merge_b->ctx->cancel_baton,
                                  NULL, NULL /* no notify! */,
                                  scratch_pool));
            }
          else
            {
              SVN_ERR(svn_wc_add_from_disk3(merge_b->ctx->wc_ctx, local_abspath,
                                            apr_hash_make(scratch_pool),
                                            FALSE /* skip checks */,
                                            NULL, NULL /* no notify! */,
                                            scratch_pool));
            }

          if (old_tc != NULL)
            {
              /* ### Should be atomic with svn_wc_add(4|_from_disk2)() */
              SVN_ERR(record_tree_conflict(merge_b, local_abspath, pdb,
                                           old_tc->node_kind,
                                           svn_node_none,
                                           svn_node_dir,
                                           db->tree_conflict_action,
                                           db->tree_conflict_reason,
                                           old_tc, FALSE,
                                           scratch_pool));
            }
        }

      if (! db->shadowed && !merge_b->record_only)
        SVN_ERR(record_update_add(merge_b, local_abspath, svn_node_dir,
                                  db->add_is_replace,
                                  pdb && pdb->added,
                                  scratch_pool));
    }
  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_dir_opened() when a node exists in both the left and
 * right source, but has its properties changed inbetween.
 *
 * After the merge_dir_opened() but before the call to this merge_dir_changed()
 * function all descendants will have been updated.
 */
static svn_error_t *
merge_dir_changed(const char *relpath,
                  const svn_diff_source_t *left_source,
                  const svn_diff_source_t *right_source,
                  /*const*/ apr_hash_t *left_props,
                  /*const*/ apr_hash_t *right_props,
                  const apr_array_header_t *prop_changes,
                  void *dir_baton,
                  const struct svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = dir_baton;
  const apr_array_header_t *props;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);

  SVN_ERR(handle_pending_notifications(merge_b, db, scratch_pool));

  SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));

  if (db->shadowed)
    {
      if (db->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_dir,
                              svn_wc_notify_update_shadowed_update,
                              db->skip_reason, db->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  SVN_ERR(prepare_merge_props_changed(&props, local_abspath, prop_changes,
                                      merge_b, scratch_pool, scratch_pool));

  if (props->nelts)
    {
      const svn_wc_conflict_version_t *left;
      const svn_wc_conflict_version_t *right;
      svn_client_ctx_t *ctx = merge_b->ctx;
      svn_wc_notify_state_t prop_state;

      SVN_ERR(make_conflict_versions(&left, &right, local_abspath,
                                     svn_node_dir, svn_node_dir,
                                     &merge_b->merge_source,
                                     merge_b->target,
                                     scratch_pool, scratch_pool));

      SVN_ERR(svn_wc_merge_props3(&prop_state, ctx->wc_ctx, local_abspath,
                                  left, right,
                                  left_props, props,
                                  merge_b->dry_run,
                                  NULL, NULL,
                                  ctx->cancel_func, ctx->cancel_baton,
                                  scratch_pool));

      if (prop_state == svn_wc_notify_state_conflicted)
        {
          SVN_ERR(notify_conflicted_path(merge_b, local_abspath, FALSE,
                                         scratch_pool));

        }

      if (prop_state == svn_wc_notify_state_conflicted
          || prop_state == svn_wc_notify_state_merged
          || prop_state == svn_wc_notify_state_changed)
        {
          SVN_ERR(record_update_update(merge_b, local_abspath, svn_node_file,
                                       svn_wc_notify_state_inapplicable,
                                       prop_state,
                                       db->parent_baton && db->added,
                                       scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}


/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_dir_opened() when a node doesn't exist in LEFT_SOURCE,
 * but does in RIGHT_SOURCE. After the merge_dir_opened() but before the call
 * to this merge_dir_added() function all descendants will have been added.
 *
 * When a node is replaced instead of just added a separate opened+deleted will
 * be invoked before the current open+added.
 */
static svn_error_t *
merge_dir_added(const char *relpath,
                const svn_diff_source_t *copyfrom_source,
                const svn_diff_source_t *right_source,
                /*const*/ apr_hash_t *copyfrom_props,
                /*const*/ apr_hash_t *right_props,
                void *dir_baton,
                const struct svn_diff_tree_processor_t *processor,
                apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = dir_baton;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);

  /* For consistency; usually a no-op from _dir_added() */
  SVN_ERR(handle_pending_notifications(merge_b, db, scratch_pool));
  SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));

  if (db->shadowed)
    {
      if (db->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_dir,
                              svn_wc_notify_update_shadowed_add,
                              db->skip_reason, db->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  SVN_ERR_ASSERT(
                 db->edited                  /* Marked edited from merge_open_dir() */
                 && ! merge_b->record_only /* Skip details from merge_open_dir() */
                 );

  SVN_ERR(notify_updated_path(merge_b, local_abspath,
                              svn_wc_notify_update_add,
                              db->parent_baton && db->parent_baton->added,
                              scratch_pool));

  if (merge_b->same_repos)
    {
      /* When the directory was added in merge_dir_added() we didn't update its
         pristine properties. Instead we receive the property changes later and
         apply them in this function.

         If we would apply them as changes (such as before fixing issue #3405),
         we would see the unmodified properties as local changes, and the
         pristine properties would be out of sync with what the repository
         expects for this directory.

         Instead of doing that we now simply set the properties as the pristine
         properties via a private libsvn_wc api.
      */

      const char *copyfrom_url;
      svn_revnum_t copyfrom_rev;
      const char *parent_abspath;
      const char *child;

      /* Creating a hash containing regular and entry props */
      apr_hash_t *new_pristine_props = right_props;

      parent_abspath = svn_dirent_dirname(local_abspath, scratch_pool);
      child = svn_dirent_is_child(merge_b->target->abspath, local_abspath, NULL);
      SVN_ERR_ASSERT(child != NULL);

      copyfrom_url = svn_path_url_add_component2(merge_b->merge_source.loc2->url,
                                                 child, scratch_pool);
      copyfrom_rev = right_source->revision;

      SVN_ERR(check_repos_match(merge_b->target, parent_abspath, copyfrom_url,
                                scratch_pool));

      if (!merge_b->dry_run)
        {
          SVN_ERR(svn_wc__complete_directory_add(merge_b->ctx->wc_ctx,
                                                local_abspath,
                                                new_pristine_props,
                                                copyfrom_url, copyfrom_rev,
                                                scratch_pool));
        }

      if (merge_b->cb_table && merge_b->cb_table->mergeinfo_changed)
        {
          const svn_string_t *new_mergeinfo = svn_hash_gets(new_pristine_props,
                                                            SVN_PROP_MERGEINFO);

          SVN_ERR(merge_b->cb_table->mergeinfo_changed(merge_b->cb_baton,
                                                       local_abspath,
                                                       NULL,
                                                       new_mergeinfo,
                                                       scratch_pool));
        }
    }
  else
    {
      apr_array_header_t *regular_props;
      apr_hash_t *new_props;
      svn_wc_notify_state_t prop_state;

      SVN_ERR(svn_categorize_props(svn_prop_hash_to_array(right_props,
                                                          scratch_pool),
                                   NULL, NULL, &regular_props, scratch_pool));

      new_props = svn_prop_array_to_hash(regular_props, scratch_pool);

      svn_hash_sets(new_props, SVN_PROP_MERGEINFO, NULL);

      /* ### What is the easiest way to set new_props on LOCAL_ABSPATH?

         ### This doesn't need a merge as we just added the node
         ### (or installed a tree conflict and skipped this node)*/

      SVN_ERR(svn_wc_merge_props3(&prop_state, merge_b->ctx->wc_ctx,
                                  local_abspath,
                                  NULL, NULL,
                                  apr_hash_make(scratch_pool),
                                  svn_prop_hash_to_array(new_props,
                                                         scratch_pool),
                                  merge_b->dry_run,
                                  NULL, NULL,
                                  merge_b->ctx->cancel_func,
                                  merge_b->ctx->cancel_baton,
                                  scratch_pool));
      if (prop_state == svn_wc_notify_state_conflicted)
        {
          SVN_ERR(notify_conflicted_path(merge_b, local_abspath, FALSE,
                                         scratch_pool));
        }
    }

  return SVN_NO_ERROR;
}

/* Helper for merge_dir_deleted. Implement svn_wc_status_func4_t */
static svn_error_t *
verify_touched_by_del_check(void *baton,
                            const char *local_abspath,
                            const svn_wc_status3_t *status,
                            apr_pool_t *scratch_pool)
{
  struct dir_delete_baton_t *delb = baton;

  if (svn_hash_gets(delb->compared_abspaths, local_abspath))
    return SVN_NO_ERROR;

  switch (status->node_status)
    {
      case svn_wc_status_deleted:
      case svn_wc_status_ignored:
      case svn_wc_status_none:
        return SVN_NO_ERROR;

      default:
        delb->found_edit = TRUE;
        return svn_error_create(SVN_ERR_CEASE_INVOCATION, NULL, NULL);
    }
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_dir_opened() when a node existed only in the left source.
 *
 * After the merge_dir_opened() but before the call to this merge_dir_deleted()
 * function all descendants that existed in left_source will have been deleted.
 *
 * If this node is replaced, an _opened() followed by a matching _add() will
 * be invoked after this function.
 */
static svn_error_t *
merge_dir_deleted(const char *relpath,
                  const svn_diff_source_t *left_source,
                  /*const*/ apr_hash_t *left_props,
                  void *dir_baton,
                  const struct svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = dir_baton;
  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);
  svn_boolean_t same;
  apr_hash_t *working_props;

  SVN_ERR(handle_pending_notifications(merge_b, db, scratch_pool));
  SVN_ERR(mark_dir_edited(merge_b, db, local_abspath, scratch_pool));

  if (db->shadowed)
    {
      if (db->tree_conflict_reason == CONFLICT_REASON_NONE)
        {
          /* We haven't notified for this node yet: report a skip */
          SVN_ERR(record_skip(merge_b, local_abspath, svn_node_dir,
                              svn_wc_notify_update_shadowed_delete,
                              db->skip_reason, db->parent_baton,
                              scratch_pool));
        }

      return SVN_NO_ERROR;
    }

  /* Easy out: We are only applying mergeinfo differences. */
  if (merge_b->record_only)
    {
      return SVN_NO_ERROR;
    }

  SVN_ERR(svn_wc_prop_list2(&working_props,
                            merge_b->ctx->wc_ctx, local_abspath,
                            scratch_pool, scratch_pool));

  if (merge_b->force_delete)
    {
      /* In this legacy mode we just assume that a directory delete
         matches any directory. db->delete_state is NULL */
      same = TRUE;
    }
  else
    {
      struct dir_delete_baton_t *delb;

      /* Compare the properties */
      SVN_ERR(properties_same_p(&same, left_props, working_props,
                                scratch_pool));
      delb = db->delete_state;
      assert(delb != NULL);

      if (! same)
        {
          delb->found_edit = TRUE;
        }
      else
        {
          store_path(delb->compared_abspaths, local_abspath);
        }

      if (delb->del_root != db)
        return SVN_NO_ERROR;

      if (delb->found_edit)
        same = FALSE;
      else
        {
          apr_array_header_t *ignores;
          svn_error_t *err;
          same = TRUE;

          SVN_ERR(svn_wc_get_default_ignores(&ignores, merge_b->ctx->config,
                                             scratch_pool));

          /* None of the descendants was modified, but maybe there are
             descendants we haven't walked?

             Note that we aren't interested in changes, as we already verified
             changes in the paths touched by the merge. And the existence of
             other paths is enough to mark the directory edited */
          err = svn_wc_walk_status(merge_b->ctx->wc_ctx, local_abspath,
                                   svn_depth_infinity, TRUE /* get-all */,
                                   FALSE /* no-ignore */,
                                   TRUE /* ignore-text-mods */, ignores,
                                   verify_touched_by_del_check, delb,
                                   merge_b->ctx->cancel_func,
                                   merge_b->ctx->cancel_baton,
                                   scratch_pool);

          if (err)
            {
              if (err->apr_err != SVN_ERR_CEASE_INVOCATION)
                return svn_error_trace(err);

              svn_error_clear(err);
            }

          same = ! delb->found_edit;
        }
    }

  if (same && !merge_b->dry_run)
    {
      svn_error_t *err;

      err = svn_wc_delete4(merge_b->ctx->wc_ctx, local_abspath,
                           FALSE /* keep_local */, FALSE /* unversioned */,
                           merge_b->ctx->cancel_func,
                           merge_b->ctx->cancel_baton,
                           NULL, NULL /* no notify */,
                           scratch_pool);

      if (err)
        {
          if (err->apr_err != SVN_ERR_WC_LEFT_LOCAL_MOD)
            return svn_error_trace(err);

          svn_error_clear(err);
          same = FALSE;
        }
    }

  if (! same)
    {
      /* If the attempt to delete an existing directory failed,
       * the directory has local modifications (e.g. locally added
       * files, or property changes). Flag a tree conflict. */

      /* This handles use case 5 described in the paper attached to issue
       * #2282.  See also notes/tree-conflicts/detection.txt
       */
      SVN_ERR(record_tree_conflict(merge_b, local_abspath, db->parent_baton,
                                   svn_node_dir,
                                   svn_node_dir,
                                   svn_node_none,
                                   svn_wc_conflict_action_delete,
                                   svn_wc_conflict_reason_edited,
                                   NULL, TRUE,
                                   scratch_pool));
    }
  else
    {
      /* Record that we might have deleted mergeinfo */
      if (merge_b->cb_table && merge_b->cb_table->mergeinfo_changed)
        {
          const svn_string_t *old_mergeinfo;
          if (working_props)
            old_mergeinfo = svn_hash_gets(working_props, SVN_PROP_MERGEINFO);
          else
            old_mergeinfo = NULL;

          if (old_mergeinfo)
            {
              SVN_ERR(merge_b->cb_table->mergeinfo_changed(merge_b->cb_baton, local_abspath,
                                                           old_mergeinfo,
                                                           NULL,
                                                           scratch_pool));
            }
      }

      SVN_ERR(record_update_delete(merge_b, db->parent_baton, local_abspath,
                                   svn_node_dir, scratch_pool));
    }

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.
 *
 * Called after merge_dir_opened() when a node itself didn't change between
 * the left and right source.
 *
 * After the merge_dir_opened() but before the call to this merge_dir_closed()
 * function all descendants will have been processed.
 */
static svn_error_t *
merge_dir_closed(const char *relpath,
                 const svn_diff_source_t *left_source,
                 const svn_diff_source_t *right_source,
                 void *dir_baton,
                 const struct svn_diff_tree_processor_t *processor,
                 apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = dir_baton;

  SVN_ERR(handle_pending_notifications(merge_b, db, scratch_pool));

  return SVN_NO_ERROR;
}

/* An svn_diff_tree_processor_t function.

   Called when the diff driver wants to report an absent path.

   In case of merges this happens when the diff encounters a server-excluded
   path.

   We register a skipped path, which will make parent mergeinfo non-
   inheritable. This ensures that a future merge might see these skipped
   changes as eligible for merging.

   For legacy reasons we also notify the path as skipped.
 */
static svn_error_t *
merge_node_absent(const char *relpath,
                  void *dir_baton,
                  const svn_diff_tree_processor_t *processor,
                  apr_pool_t *scratch_pool)
{
  merge_apply_processor_baton_t *merge_b = processor->baton;
  struct merge_dir_baton_t *db = dir_baton;

  const char *local_abspath = svn_dirent_join(merge_b->target->abspath,
                                              relpath, scratch_pool);

  SVN_ERR(record_skip(merge_b, local_abspath, svn_node_unknown,
                      svn_wc_notify_skip, svn_wc_notify_state_missing,
                      db, scratch_pool));

  return SVN_NO_ERROR;
}

svn_diff_tree_processor_t *
svn_client__apply_processor_create(const svn_client__merge_target_t *target,
                                   const svn_client__merge_source_t *source,
                                   svn_wc_notify_func2_t notify_func,
                                   void *notify_baton,
                                   svn_boolean_t same_repos,
                                   svn_boolean_t force_delete,
                                   svn_boolean_t record_only,
                                   svn_boolean_t dry_run,
                                   const char *diff3_cmd,
                                   const apr_array_header_t *merge_options,
                                   const apr_array_header_t *ext_patterns,
                                   const svn_client__apply_processor_callbacks_t *cb_table,
                                   void *cb_baton,
                                   svn_client_ctx_t *ctx,
                                   apr_pool_t *scratch_pool,
                                   apr_pool_t *result_pool)
{
  svn_diff_tree_processor_t *merge_processor;

  merge_apply_processor_baton_t *baton =
    apr_pcalloc(result_pool, sizeof(*baton));

  baton->target = target;
  baton->merge_source = *source;
  baton->diff3_cmd = diff3_cmd;
  baton->merge_options = merge_options;
  baton->ext_patterns = ext_patterns;
  baton->same_repos = same_repos;
  baton->force_delete = force_delete;
  baton->record_only = record_only;
  baton->dry_run = dry_run;
  baton->notify_func = notify_func;
  baton->notify_baton = notify_baton;
  baton->cb_table = cb_table;
  baton->cb_baton = cb_baton;
  baton->ctx = ctx;

  merge_processor = svn_diff__tree_processor_create(baton,
                                                    result_pool);

  merge_processor->dir_opened   = merge_dir_opened;
  merge_processor->dir_changed  = merge_dir_changed;
  merge_processor->dir_added    = merge_dir_added;
  merge_processor->dir_deleted  = merge_dir_deleted;
  merge_processor->dir_closed   = merge_dir_closed;

  merge_processor->file_opened  = merge_file_opened;
  merge_processor->file_changed = merge_file_changed;
  merge_processor->file_added   = merge_file_added;
  merge_processor->file_deleted = merge_file_deleted;
  /* Not interested in file_closed() */

  merge_processor->node_absent = merge_node_absent;

  return merge_processor;
}
