/*
 * cmdline.c:  command-line processing
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


/*** Includes. ***/
#include "svn_client.h"
#include "svn_error.h"
#include "svn_dirent_uri.h"
#include "svn_path.h"
#include "svn_opt.h"
#include "svn_utf.h"

#include "client.h"

#include "private/svn_opt_private.h"

#include "svn_private_config.h"


/*** Code. ***/


/* Attempt to find the repository root url for TARGET, possibly using CTX for
 * authentication.  If one is found and *ROOT_URL is not NULL, then just check
 * that the root url for TARGET matches the value given in *ROOT_URL and
 * return an error if it does not.  If one is found and *ROOT_URL is NULL then
 * set *ROOT_URL to the root url for TARGET, allocated from POOL.
 * If a root url is not found for TARGET because it does not exist in the
 * repository, then return with no error.
 *
 * TARGET is a UTF-8 encoded string that is fully canonicalized and escaped.
 */
static svn_error_t *
check_root_url_of_target(const char **root_url,
                         const char *target,
                         svn_client_ctx_t *ctx,
                         apr_pool_t *pool)
{
  svn_error_t *err;
  const char *tmp_root_url;
  const char *truepath;
  svn_opt_revision_t opt_rev;

  SVN_ERR(svn_opt_parse_path(&opt_rev, &truepath, target, pool));
  if (!svn_path_is_url(truepath))
    SVN_ERR(svn_dirent_get_absolute(&truepath, truepath, pool));

  err = svn_client_get_repos_root(&tmp_root_url, NULL, truepath,
                                  ctx, pool, pool);

  if (err)
    {
      /* It is OK if the given target does not exist, it just means
       * we will not be able to determine the root url from this particular
       * argument.
       *
       * If the target itself is a URL to a repository that does not exist,
       * that's fine, too. The callers will deal with this argument in an
       * appropriate manner if it does not make any sense.
       *
       * Also tolerate locally added targets ("bad revision" error).
       */
      if ((err->apr_err == SVN_ERR_ENTRY_NOT_FOUND)
          || (err->apr_err == SVN_ERR_WC_PATH_NOT_FOUND)
          || (err->apr_err == SVN_ERR_WC_NOT_WORKING_COPY)
          || (err->apr_err == SVN_ERR_RA_CANNOT_CREATE_SESSION)
          || (err->apr_err == SVN_ERR_CLIENT_BAD_REVISION))
        {
          svn_error_clear(err);
          return SVN_NO_ERROR;
        }
      else
        return svn_error_trace(err);
     }

   if (*root_url && tmp_root_url)
     {
       if (strcmp(*root_url, tmp_root_url) != 0)
         return svn_error_createf(SVN_ERR_ILLEGAL_TARGET, NULL,
                                  _("All non-relative targets must have "
                                    "the same root URL"));
     }
   else
     *root_url = tmp_root_url;

   return SVN_NO_ERROR;
}

static svn_error_t *
find_root_url(const char **root_url_p,
              const apr_array_header_t *raw_targets,
              svn_client_ctx_t *ctx,
              apr_pool_t *pool)
{
  int i;

  for (i = 0; i < raw_targets->nelts; i++)
    {
      const char *target = APR_ARRAY_IDX(raw_targets, i, const char *);

      /* Later targets have priority over earlier target, I
         don't know why, see basic_relative_url_multi_repo. */
      SVN_ERR(check_root_url_of_target(root_url_p, target, ctx, pool));
    }

  /*
   * Use the current directory's root url if one wasn't found using the
   * arguments.
   */
  if (*root_url_p == NULL)
    {
      const char *current_abspath;
      svn_error_t *err;

      SVN_ERR(svn_dirent_get_absolute(&current_abspath, "", pool));
      err = svn_client_get_repos_root(root_url_p, NULL /* uuid */,
                                      current_abspath, ctx, pool, pool);
      if (err || *root_url_p == NULL)
        return svn_error_create(SVN_ERR_WC_NOT_WORKING_COPY, err,
                                _("Resolving '^/': no repository root "
                                  "found in the target arguments or "
                                  "in the current directory"));
    }

  return SVN_NO_ERROR;
}


/* Note: This is substantially copied from svn_opt__process_target_array() in
 * order to move to libsvn_client while maintaining backward compatibility. */
svn_error_t *
svn_client__process_target_array(apr_array_header_t **targets_p,
                                 apr_array_header_t *utf8_targets,
                                 const apr_array_header_t *known_targets,
                                 svn_client_ctx_t *ctx,
                                 svn_boolean_t keep_last_origpath_on_truepath_collision,
                                 apr_pool_t *pool)
{
  int i;
  svn_boolean_t rel_url_found = FALSE;
  const char *root_url = NULL;
  apr_array_header_t *input_targets = NULL;
  apr_array_header_t *parsed_targets = NULL;
  apr_array_header_t *reserved_names = NULL;

  /* Step 1:  create a master array of targets that are in UTF-8
     encoding, and come from concatenating the targets left by apr_getopt,
     plus any extra targets (e.g., from the --targets switch.)
     If any of the targets are relative urls, then set the rel_url_found
     flag.*/

  SVN_ERR(svn_opt__collect_targets(&input_targets, &rel_url_found,
                                   utf8_targets, known_targets, pool));

  SVN_ERR(svn_opt__target_array_parse(&parsed_targets, &rel_url_found,
                                      input_targets, pool));

  /* Step 2:  process each target.  */

  for (i = 0; i < parsed_targets->nelts; i++)
    {
      svn_opt__target_t *target = APR_ARRAY_IDX(parsed_targets, i,
                                                svn_opt__target_t *);
      const char *raw_target = APR_ARRAY_IDX(input_targets, i, const char *);

      /* Reject the form "@abc", a peg specifier with no path. */
      if (target->true_target[0] == '\0' && target->peg_revision[0] != '\0')
        {
          return svn_error_createf(SVN_ERR_BAD_FILENAME, NULL,
                                    _("'%s' is just a peg revision. "
                                      "Maybe try '%s@' instead?"),
                                    raw_target, raw_target);
        }

      /* Relative urls will be canonicalized when they are resolved later in
       * the function
       */
      if (target->type == svn_opt__target_type_absolute_url)
        {
          /*
           * This is needed so that the target can be properly canonicalized,
           * otherwise the canonicalization does not treat a ".@BASE" as a "."
           * with a BASE peg revision, and it is not canonicalized to "@BASE".
           * If any peg revision exists, it is appended to the final
           * canonicalized path or URL.  Do not use svn_opt_parse_path()
           * because the resulting peg revision is a structure that would have
           * to be converted back into a string.  Converting from a string date
           * to the apr_time_t field in the svn_opt_revision_value_t and back to
           * a string would not necessarily preserve the exact bytes of the
           * input date, so its easier just to keep it in string form.
           */

          if (target->type == svn_opt__target_type_local_abspath)
            {
              const char *base_name;
              const char *original_target;

              original_target = svn_dirent_internal_style(target->true_target,
                                                          pool);

              /* There are two situations in which a 'truepath-conversion'
                 (case-canonicalization to on-disk path on case-insensitive
                 filesystem) needs to be undone:

                 1. If KEEP_LAST_ORIGPATH_ON_TRUEPATH_COLLISION is TRUE, and
                    this is the last target of a 2-element target list, and
                    both targets have the same truepath. */
              if (keep_last_origpath_on_truepath_collision
                  && input_targets->nelts == 2 && i == 1
                  && strcmp(original_target, target->true_target) != 0)
                {
                  const char *src_truepath = APR_ARRAY_IDX(input_targets,
                                                           0,
                                                           const char *);
                  if (strcmp(src_truepath, target->true_target) == 0)
                    target->true_target = original_target;
                }

              /* 2. If there is an exact match in the wc-db without a
                    corresponding on-disk path (e.g. a scheduled-for-delete
                    file only differing in case from an on-disk file). */
              if (strcmp(original_target, target->true_target) != 0)
                {
                  const char *target_abspath;
                  svn_node_kind_t kind;
                  svn_error_t *err2;

                  SVN_ERR(svn_dirent_get_absolute(&target_abspath,
                                                  original_target, pool));
                  err2 = svn_wc_read_kind2(&kind, ctx->wc_ctx, target_abspath,
                                           TRUE, FALSE, pool);
                  if (err2
                      && (err2->apr_err == SVN_ERR_WC_NOT_WORKING_COPY
                          || err2->apr_err == SVN_ERR_WC_UPGRADE_REQUIRED))
                    {
                      svn_error_clear(err2);
                    }
                  else
                    {
                      SVN_ERR(err2);
                      /* We successfully did a lookup in the wc-db. Now see
                         if it's something interesting. */
                      if (kind == svn_node_file || kind == svn_node_dir)
                        target->true_target = original_target;
                    }
                }

              /* If the target has the same name as a Subversion
                 working copy administrative dir, skip it. */
              base_name = svn_dirent_basename(target->true_target, pool);

              if (svn_wc_is_adm_dir(base_name, pool))
                {
                  if (!reserved_names)
                    reserved_names = apr_array_make(pool, 1,
                                                    sizeof(const char *));

                  APR_ARRAY_PUSH(reserved_names, const char *) = raw_target;

                  continue;
                }
            }

          if (rel_url_found
              && target->type != svn_opt__target_type_relative_url)
            {
              /* Later targets have priority over earlier target, I
                 don't know why, see basic_relative_url_multi_repo. */
              SVN_ERR(check_root_url_of_target(&root_url, target->true_target,
                                               ctx, pool));
            }
        }
    }

  /* Only resolve relative urls if there were some actually found earlier. */
  if (rel_url_found)
    {
      SVN_ERR(find_root_url(&root_url, input_targets, ctx, pool));

      for (i = 0; i < parsed_targets->nelts; i++)
        {
          svn_opt__target_t *target = APR_ARRAY_IDX(parsed_targets, i,
                                                    svn_opt__target_t *);

          if (target->type == svn_opt__target_type_relative_url)
            SVN_ERR(svn_opt__target_resolve(target, root_url, pool));
        }
    }

  *targets_p = apr_array_make(pool, parsed_targets->nelts,
                              sizeof(const char *));

  SVN_ERR(svn_opt__target_array_to_string(targets_p, parsed_targets, pool));

  if (reserved_names)
    {
      svn_error_t *err = SVN_NO_ERROR;

      for (i = 0; i < reserved_names->nelts; ++i)
        err = svn_error_createf(SVN_ERR_RESERVED_FILENAME_SPECIFIED, err,
                                _("'%s' ends in a reserved name"),
                                APR_ARRAY_IDX(reserved_names, i,
                                              const char *));
      return svn_error_trace(err);
    }

  return SVN_NO_ERROR;
}

svn_error_t *
svn_client_args_to_target_array3(apr_array_header_t **targets_p,
                                 apr_getopt_t *os,
                                 const apr_array_header_t *known_targets,
                                 svn_client_ctx_t *ctx,
                                 svn_boolean_t keep_last_origpath_on_truepath_collision,
                                 apr_pool_t *pool)
{
  apr_array_header_t *utf8_input_targets;

  SVN_ERR(svn_opt_parse_all_args(&utf8_input_targets, os, pool));

  return svn_error_trace(svn_client__process_target_array(
      targets_p, utf8_input_targets, known_targets, ctx,
      keep_last_origpath_on_truepath_collision, pool));
}
