/**
 * @file
 * Send/reply with an attachment
 *
 * @authors
 * Copyright (C) 1999-2004 Thomas Roessler <roessler@does-not-exist.org>
 * Copyright (C) 2019 Pietro Cerutti <gahr@gahr.ch>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page neo_recvcmd Send/reply with an attachment
 *
 * Send/reply with an attachment
 */

#include "config.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "mutt/lib.h"
#include "address/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "alias/lib.h"
#include "gui/lib.h"
#include "mutt.h"
#include "recvcmd.h"
#include "attach/lib.h"
#include "enter/lib.h"
#include "question/lib.h"
#include "send/lib.h"
#include "copy.h"
#include "format_flags.h"
#include "handler.h"
#include "hdrline.h"
#include "mutt_body.h"
#include "mutt_logging.h"
#include "muttlib.h"
#include "options.h"
#include "protos.h"
#ifdef ENABLE_NLS
#include <libintl.h>
#endif

/**
 * check_msg - Are we working with an RFC822 message
 * @param b   Body of email
 * @param err If true, display a message if this isn't an RFC822 message
 * @retval true This is an RFC822 message
 *
 * some helper functions to verify that we are exclusively operating on
 * message/rfc822 attachments
 */
static bool check_msg(struct Body *b, bool err)
{
  if (!mutt_is_message_type(b->type, b->subtype))
  {
    if (err)
      mutt_error(_("You may only bounce message/rfc822 parts"));
    return false;
  }
  return true;
}

/**
 * check_all_msg - Are all the Attachments RFC822 messages?
 * @param actx Attachment context
 * @param cur  Current message
 * @param err  If true, report errors
 * @retval true All parts are RFC822 messages
 */
static bool check_all_msg(struct AttachCtx *actx, struct Body *cur, bool err)
{
  if (cur && !check_msg(cur, err))
    return false;
  if (!cur)
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        if (!check_msg(actx->idx[i]->body, err))
          return false;
      }
    }
  }
  return true;
}

/**
 * check_can_decode - Can we decode all tagged attachments?
 * @param actx Attachment context
 * @param cur  Body of email
 * @retval true All tagged attachments are decodable
 */
static bool check_can_decode(struct AttachCtx *actx, struct Body *cur)
{
  if (cur)
    return mutt_can_decode(cur);

  for (short i = 0; i < actx->idxlen; i++)
    if (actx->idx[i]->body->tagged && !mutt_can_decode(actx->idx[i]->body))
      return false;

  return true;
}

/**
 * count_tagged - Count the number of tagged attachments
 * @param actx Attachment context
 * @retval num Number of tagged attachments
 */
static short count_tagged(struct AttachCtx *actx)
{
  short count = 0;
  for (short i = 0; i < actx->idxlen; i++)
    if (actx->idx[i]->body->tagged)
      count++;

  return count;
}

/**
 * count_tagged_children - Tagged children below a multipart/message attachment
 * @param actx Attachment context
 * @param i    Index of first attachment
 * @retval num Number of tagged attachments
 */
static short count_tagged_children(struct AttachCtx *actx, short i)
{
  short level = actx->idx[i]->level;
  short count = 0;

  while ((++i < actx->idxlen) && (level < actx->idx[i]->level))
    if (actx->idx[i]->body->tagged)
      count++;

  return count;
}

/**
 * mutt_attach_bounce - Bounce function, from the attachment menu
 * @param m    Mailbox
 * @param fp   Handle of message
 * @param actx Attachment context
 * @param cur  Body of email
 */
void mutt_attach_bounce(struct Mailbox *m, FILE *fp, struct AttachCtx *actx, struct Body *cur)
{
  if (!m || !fp || !actx)
    return;

  if (!check_all_msg(actx, cur, true))
    return;

  struct AddressList al = TAILQ_HEAD_INITIALIZER(al);
  struct Buffer *prompt = mutt_buffer_pool_get();
  struct Buffer *buf = mutt_buffer_pool_get();

  /* RFC5322 mandates a From: header, so warn before bouncing
   * messages without one */
  if (cur)
  {
    if (TAILQ_EMPTY(&cur->email->env->from))
    {
      mutt_error(_("Warning: message contains no From: header"));
      mutt_clear_error();
    }
  }
  else
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        if (TAILQ_EMPTY(&actx->idx[i]->body->email->env->from))
        {
          mutt_error(_("Warning: message contains no From: header"));
          mutt_clear_error();
          break;
        }
      }
    }
  }

  /* one or more messages? */
  int num_msg = cur ? 1 : count_tagged(actx);
  if (num_msg == 1)
    mutt_buffer_strcpy(prompt, _("Bounce message to: "));
  else
    mutt_buffer_strcpy(prompt, _("Bounce tagged messages to: "));

  if ((mutt_buffer_get_field(mutt_buffer_string(prompt), buf, MUTT_COMP_ALIAS,
                             false, NULL, NULL, NULL) != 0) ||
      mutt_buffer_is_empty(buf))
  {
    goto done;
  }

  mutt_addrlist_parse(&al, mutt_buffer_string(buf));
  if (TAILQ_EMPTY(&al))
  {
    mutt_error(_("Error parsing address"));
    goto done;
  }

  mutt_expand_aliases(&al);

  char *err = NULL;
  if (mutt_addrlist_to_intl(&al, &err) < 0)
  {
    mutt_error(_("Bad IDN: '%s'"), err);
    FREE(&err);
    goto done;
  }

  mutt_buffer_reset(buf);
  mutt_buffer_alloc(buf, 8192);
  mutt_addrlist_write(&al, buf->data, buf->dsize, true);

#define EXTRA_SPACE (15 + 7 + 2)
  /* See commands.c.  */
  mutt_buffer_printf(prompt, ngettext("Bounce message to %s?", "Bounce messages to %s?", num_msg),
                     mutt_buffer_string(buf));

  const size_t width = msgwin_get_width();
  if (mutt_strwidth(mutt_buffer_string(prompt)) > (width - EXTRA_SPACE))
  {
    struct Buffer *scratch = mutt_buffer_pool_get();
    mutt_simple_format(scratch->data, scratch->dsize - 4, 0, width - EXTRA_SPACE,
                       JUSTIFY_LEFT, 0, prompt->data, prompt->dsize, false);
    mutt_buffer_addstr(scratch, "...?");
    mutt_buffer_copy(prompt, scratch);
    mutt_buffer_pool_release(&scratch);
  }
  else
  {
    mutt_buffer_addstr(prompt, "?");
  }

  const enum QuadOption c_bounce = cs_subset_quad(NeoMutt->sub, "bounce");
  if (query_quadoption(c_bounce, mutt_buffer_string(prompt)) != MUTT_YES)
  {
    msgwin_clear_text();
    mutt_message(ngettext("Message not bounced", "Messages not bounced", num_msg));
    goto done;
  }

  msgwin_clear_text();

  int rc = 0;
  if (cur)
  {
    rc = mutt_bounce_message(fp, m, cur->email, &al, NeoMutt->sub);
  }
  else
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        if (mutt_bounce_message(actx->idx[i]->fp, m, actx->idx[i]->body->email,
                                &al, NeoMutt->sub))
        {
          rc = 1;
        }
      }
    }
  }

  if (rc == 0)
    mutt_message(ngettext("Message bounced", "Messages bounced", num_msg));
  else
    mutt_error(ngettext("Error bouncing message", "Error bouncing messages", num_msg));

done:
  mutt_addrlist_clear(&al);
  mutt_buffer_pool_release(&buf);
  mutt_buffer_pool_release(&prompt);
}

/**
 * mutt_attach_resend - Resend-message, from the attachment menu
 * @param fp   File containing email
 * @param m    Current mailbox
 * @param actx Attachment context
 * @param cur  Attachment
 */
void mutt_attach_resend(FILE *fp, struct Mailbox *m, struct AttachCtx *actx, struct Body *cur)
{
  if (!check_all_msg(actx, cur, true))
    return;

  if (cur)
    mutt_resend_message(fp, m, cur->email, NeoMutt->sub);
  else
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        mutt_resend_message(actx->idx[i]->fp, m, actx->idx[i]->body->email,
                            NeoMutt->sub);
      }
    }
  }
}

/**
 * find_common_parent - Find a common parent message for the tagged attachments
 * @param actx    Attachment context
 * @param nattach Number of tagged attachments
 * @retval ptr Parent attachment
 * @retval NULL Failure, no common parent
 */
static struct AttachPtr *find_common_parent(struct AttachCtx *actx, short nattach)
{
  short i;
  short nchildren;

  for (i = 0; i < actx->idxlen; i++)
    if (actx->idx[i]->body->tagged)
      break;

  while (--i >= 0)
  {
    if (mutt_is_message_type(actx->idx[i]->body->type, actx->idx[i]->body->subtype))
    {
      nchildren = count_tagged_children(actx, i);
      if (nchildren == nattach)
        return actx->idx[i];
    }
  }

  return NULL;
}

/**
 * is_parent - Check whether one attachment is the parent of another
 * @param i    Index of parent Attachment
 * @param actx Attachment context
 * @param cur  Potential child Attachemnt
 * @retval true Attachment
 *
 * check whether attachment i is a parent of the attachment pointed to by cur
 *
 * @note This and the calling procedure could be optimized quite a bit.
 *       For now, it's not worth the effort.
 */
static int is_parent(short i, struct AttachCtx *actx, const struct Body *cur)
{
  short level = actx->idx[i]->level;

  while ((++i < actx->idxlen) && (actx->idx[i]->level > level))
  {
    if (actx->idx[i]->body == cur)
      return true;
  }

  return false;
}

/**
 * find_parent - Find the parent of an Attachment
 * @param actx    Attachment context
 * @param cur     Attachment (OPTIONAL)
 * @param nattach Use the nth attachment
 * @retval ptr  Parent attachment
 * @retval NULL No parent exists
 */
static struct AttachPtr *find_parent(struct AttachCtx *actx, struct Body *cur, short nattach)
{
  struct AttachPtr *parent = NULL;

  if (cur)
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (mutt_is_message_type(actx->idx[i]->body->type, actx->idx[i]->body->subtype) &&
          is_parent(i, actx, cur))
      {
        parent = actx->idx[i];
      }
      if (actx->idx[i]->body == cur)
        break;
    }
  }
  else if (nattach)
    parent = find_common_parent(actx, nattach);

  return parent;
}

/**
 * include_header - Write an email header to a file, optionally quoting it
 * @param quote  If true, prefix the lines
 * @param fp_in  File to read from
 * @param e      Email
 * @param fp_out File to write to
 * @param prefix Prefix for each line (OPTIONAL)
 */
static void include_header(bool quote, FILE *fp_in, struct Email *e, FILE *fp_out, char *prefix)
{
  CopyHeaderFlags chflags = CH_DECODE;
  char prefix2[128];

  const bool c_weed = cs_subset_bool(NeoMutt->sub, "weed");
  if (c_weed)
    chflags |= CH_WEED | CH_REORDER;

  if (quote)
  {
    const bool c_text_flowed = cs_subset_bool(NeoMutt->sub, "text_flowed");
    if (prefix)
      mutt_str_copy(prefix2, prefix, sizeof(prefix2));
    else if (!c_text_flowed)
    {
      const char *const c_indent_string = cs_subset_string(NeoMutt->sub, "indent_string");
      mutt_make_string(prefix2, sizeof(prefix2), 0, NONULL(c_indent_string),
                       NULL, -1, e, MUTT_FORMAT_NO_FLAGS, NULL);
    }
    else
      mutt_str_copy(prefix2, ">", sizeof(prefix2));

    chflags |= CH_PREFIX;
  }

  mutt_copy_header(fp_in, e, fp_out, chflags, quote ? prefix2 : NULL, 0);
}

/**
 * copy_problematic_attachments - Attach the body parts which can't be decoded
 * @param[out] last  Body pointer to update
 * @param[in]  actx  Attachment context
 * @param[in]  force If true, attach parts that can't be decoded
 * @retval ptr Pointer to last Body part
 *
 * This code is shared by forwarding and replying.
 */
static struct Body **copy_problematic_attachments(struct Body **last,
                                                  struct AttachCtx *actx, bool force)
{
  for (short i = 0; i < actx->idxlen; i++)
  {
    if (actx->idx[i]->body->tagged && (force || !mutt_can_decode(actx->idx[i]->body)))
    {
      if (mutt_body_copy(actx->idx[i]->fp, last, actx->idx[i]->body) == -1)
        return NULL; /* XXXXX - may lead to crashes */
      last = &((*last)->next);
    }
  }
  return last;
}

/**
 * attach_forward_bodies - Forward one or several MIME bodies
 * @param fp      File to read from
 * @param e       Email
 * @param actx    Attachment Context
 * @param cur     Body of email
 * @param nattach Number of tagged attachments
 *
 * (non-message types)
 */
static void attach_forward_bodies(FILE *fp, struct Email *e, struct AttachCtx *actx,
                                  struct Body *cur, short nattach)
{
  bool mime_fwd_all = false;
  bool mime_fwd_any = true;
  struct Email *e_parent = NULL;
  FILE *fp_parent = NULL;
  char prefix[256] = { 0 };
  enum QuadOption ans = MUTT_NO;
  struct Buffer *tmpbody = NULL;

  /* First, find the parent message.
   * Note: This could be made an option by just
   * putting the following lines into an if block.  */
  struct AttachPtr *parent = find_parent(actx, cur, nattach);
  if (parent)
  {
    e_parent = parent->body->email;
    fp_parent = parent->fp;
  }
  else
  {
    e_parent = e;
    fp_parent = actx->fp_root;
  }

  struct Email *e_tmp = email_new();
  e_tmp->env = mutt_env_new();
  mutt_make_forward_subject(e_tmp->env, e_parent, NeoMutt->sub);

  tmpbody = mutt_buffer_pool_get();
  mutt_buffer_mktemp(tmpbody);
  FILE *fp_tmp = mutt_file_fopen(mutt_buffer_string(tmpbody), "w");
  if (!fp_tmp)
  {
    mutt_error(_("Can't open temporary file %s"), mutt_buffer_string(tmpbody));
    email_free(&e_tmp);
    goto bail;
  }

  mutt_forward_intro(e_parent, fp_tmp, NeoMutt->sub);

  /* prepare the prefix here since we'll need it later. */

  const bool c_forward_quote = cs_subset_bool(NeoMutt->sub, "forward_quote");
  if (c_forward_quote)
  {
    const bool c_text_flowed = cs_subset_bool(NeoMutt->sub, "text_flowed");
    if (c_text_flowed)
      mutt_str_copy(prefix, ">", sizeof(prefix));
    else
    {
      const char *const c_indent_string = cs_subset_string(NeoMutt->sub, "indent_string");
      mutt_make_string(prefix, sizeof(prefix), 0, NONULL(c_indent_string), NULL,
                       -1, e_parent, MUTT_FORMAT_NO_FLAGS, NULL);
    }
  }

  include_header(c_forward_quote, fp_parent, e_parent, fp_tmp, prefix);

  /* Now, we have prepared the first part of the message body: The
   * original message's header.
   *
   * The next part is more interesting: either include the message bodies,
   * or attach them.  */
  const enum QuadOption c_mime_forward = cs_subset_quad(NeoMutt->sub, "mime_forward");
  if ((!cur || mutt_can_decode(cur)) &&
      ((ans = query_quadoption(c_mime_forward, _("Forward as attachments?"))) == MUTT_YES))
  {
    mime_fwd_all = true;
  }
  else if (ans == MUTT_ABORT)
  {
    goto bail;
  }

  /* shortcut MIMEFWDREST when there is only one attachment.
   * Is this intuitive?  */
  if (!mime_fwd_all && !cur && (nattach > 1) && !check_can_decode(actx, cur))
  {
    const enum QuadOption c_mime_forward_rest = cs_subset_quad(NeoMutt->sub, "mime_forward_rest");
    ans = query_quadoption(c_mime_forward_rest,
                           _("Can't decode all tagged attachments.  MIME-forward the others?"));
    if (ans == MUTT_ABORT)
      goto bail;
    else if (ans == MUTT_NO)
      mime_fwd_any = false;
  }

  /* initialize a state structure */

  struct State st = { 0 };
  if (c_forward_quote)
    st.prefix = prefix;
  st.flags = MUTT_CHARCONV;
  const bool c_weed = cs_subset_bool(NeoMutt->sub, "weed");
  if (c_weed)
    st.flags |= MUTT_WEED;
  st.fp_out = fp_tmp;

  /* where do we append new MIME parts? */
  struct Body **last = &e_tmp->body;

  if (cur)
  {
    /* single body case */

    if (!mime_fwd_all && mutt_can_decode(cur))
    {
      st.fp_in = fp;
      mutt_body_handler(cur, &st);
      state_putc(&st, '\n');
    }
    else
    {
      if (mutt_body_copy(fp, last, cur) == -1)
        goto bail;
    }
  }
  else
  {
    /* multiple body case */

    if (!mime_fwd_all)
    {
      for (int i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged && mutt_can_decode(actx->idx[i]->body))
        {
          st.fp_in = actx->idx[i]->fp;
          mutt_body_handler(actx->idx[i]->body, &st);
          state_putc(&st, '\n');
        }
      }
    }

    if (mime_fwd_any && !copy_problematic_attachments(last, actx, mime_fwd_all))
      goto bail;
  }

  mutt_forward_trailer(e_parent, fp_tmp, NeoMutt->sub);

  mutt_file_fclose(&fp_tmp);
  fp_tmp = NULL;

  /* now that we have the template, send it. */
  struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
  emaillist_add_email(&el, e_parent);
  mutt_send_message(SEND_NO_FLAGS, e_tmp, mutt_buffer_string(tmpbody), NULL,
                    &el, NeoMutt->sub);
  emaillist_clear(&el);
  mutt_buffer_pool_release(&tmpbody);
  return;

bail:
  if (fp_tmp)
  {
    mutt_file_fclose(&fp_tmp);
    mutt_file_unlink(mutt_buffer_string(tmpbody));
  }
  mutt_buffer_pool_release(&tmpbody);

  email_free(&e_tmp);
}

/**
 * attach_forward_msgs - Forward one or several message-type attachments
 * @param fp    File handle to attachment
 * @param actx  Attachment Context
 * @param cur   Attachment to forward (OPTIONAL)
 * @param flags Send mode, see #SendFlags
 *
 * This is different from the previous function since we want to mimic the
 * index menu's behavior.
 *
 * Code reuse from mutt_send_message() is not possible here. It relies on a
 * context structure to find messages, while, on the attachment menu, messages
 * are referenced through the attachment index.
 */
static void attach_forward_msgs(FILE *fp, struct AttachCtx *actx,
                                struct Body *cur, SendFlags flags)
{
  struct Email *e_cur = NULL;
  struct Email *e_tmp = NULL;
  enum QuadOption ans;
  struct Body **last = NULL;
  struct Buffer *tmpbody = NULL;
  FILE *fp_tmp = NULL;

  CopyHeaderFlags chflags = CH_DECODE;

  if (cur)
    e_cur = cur->email;
  else
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        e_cur = actx->idx[i]->body->email;
        break;
      }
    }
  }

  e_tmp = email_new();
  e_tmp->env = mutt_env_new();
  mutt_make_forward_subject(e_tmp->env, e_cur, NeoMutt->sub);

  tmpbody = mutt_buffer_pool_get();

  const enum QuadOption c_mime_forward = cs_subset_quad(NeoMutt->sub, "mime_forward");
  ans = query_quadoption(c_mime_forward, _("Forward MIME encapsulated?"));
  if (ans == MUTT_NO)
  {
    /* no MIME encapsulation */

    mutt_buffer_mktemp(tmpbody);
    fp_tmp = mutt_file_fopen(mutt_buffer_string(tmpbody), "w");
    if (!fp_tmp)
    {
      mutt_error(_("Can't create %s"), mutt_buffer_string(tmpbody));
      goto cleanup;
    }

    CopyMessageFlags cmflags = MUTT_CM_NO_FLAGS;
    const bool c_forward_quote = cs_subset_bool(NeoMutt->sub, "forward_quote");
    if (c_forward_quote)
    {
      chflags |= CH_PREFIX;
      cmflags |= MUTT_CM_PREFIX;
    }

    const bool c_forward_decode = cs_subset_bool(NeoMutt->sub, "forward_decode");
    if (c_forward_decode)
    {
      cmflags |= MUTT_CM_DECODE | MUTT_CM_CHARCONV;
      const bool c_weed = cs_subset_bool(NeoMutt->sub, "weed");
      if (c_weed)
      {
        chflags |= CH_WEED | CH_REORDER;
        cmflags |= MUTT_CM_WEED;
      }
    }

    if (cur)
    {
      mutt_forward_intro(cur->email, fp_tmp, NeoMutt->sub);
      mutt_copy_message_fp(fp_tmp, fp, cur->email, cmflags, chflags, 0);
      mutt_forward_trailer(cur->email, fp_tmp, NeoMutt->sub);
    }
    else
    {
      for (short i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged)
        {
          mutt_forward_intro(actx->idx[i]->body->email, fp_tmp, NeoMutt->sub);
          mutt_copy_message_fp(fp_tmp, actx->idx[i]->fp,
                               actx->idx[i]->body->email, cmflags, chflags, 0);
          mutt_forward_trailer(actx->idx[i]->body->email, fp_tmp, NeoMutt->sub);
        }
      }
    }
    mutt_file_fclose(&fp_tmp);
  }
  else if (ans == MUTT_YES) /* do MIME encapsulation - we don't need to do much here */
  {
    last = &e_tmp->body;
    if (cur)
      mutt_body_copy(fp, last, cur);
    else
    {
      for (short i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged)
        {
          mutt_body_copy(actx->idx[i]->fp, last, actx->idx[i]->body);
          last = &((*last)->next);
        }
      }
    }
  }
  else
    email_free(&e_tmp);

  struct EmailList el = STAILQ_HEAD_INITIALIZER(el);
  emaillist_add_email(&el, e_cur);
  mutt_send_message(flags, e_tmp,
                    mutt_buffer_is_empty(tmpbody) ? NULL : mutt_buffer_string(tmpbody),
                    NULL, &el, NeoMutt->sub);
  emaillist_clear(&el);
  e_tmp = NULL; /* mutt_send_message frees this */

cleanup:
  email_free(&e_tmp);
  mutt_buffer_pool_release(&tmpbody);
}

/**
 * mutt_attach_forward - Forward an Attachment
 * @param fp    Handle to the attachment
 * @param e     Email
 * @param actx  Attachment Context
 * @param cur   Current message
 * @param flags Send mode, see #SendFlags
 */
void mutt_attach_forward(FILE *fp, struct Email *e, struct AttachCtx *actx,
                         struct Body *cur, SendFlags flags)
{
  if (check_all_msg(actx, cur, false))
    attach_forward_msgs(fp, actx, cur, flags);
  else
  {
    const short nattach = count_tagged(actx);
    attach_forward_bodies(fp, e, actx, cur, nattach);
  }
}

/**
 * attach_reply_envelope_defaults - Create the envelope defaults for a reply
 * @param env    Envelope to fill in
 * @param actx   Attachment Context
 * @param parent Parent Email
 * @param flags  Flags, see #SendFlags
 * @retval  0 Success
 * @retval -1 Error
 *
 * This function can be invoked in two ways.
 *
 * Either, parent is NULL.  In this case, all tagged bodies are of a message type,
 * and the header information is fetched from them.
 *
 * Or, parent is non-NULL.  In this case, cur is the common parent of all the
 * tagged attachments.
 *
 * Note that this code is horribly similar to envelope_defaults() from send.c.
 */
static int attach_reply_envelope_defaults(struct Envelope *env, struct AttachCtx *actx,
                                          struct Email *parent, SendFlags flags)
{
  struct Envelope *curenv = NULL;
  struct Email *e = NULL;

  if (!parent)
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        e = actx->idx[i]->body->email;
        curenv = e->env;
        break;
      }
    }
  }
  else
  {
    curenv = parent->env;
    e = parent;
  }

  if (!curenv || !e)
  {
    mutt_error(_("Can't find any tagged messages"));
    return -1;
  }

#ifdef USE_NNTP
  if ((flags & SEND_NEWS))
  {
    /* in case followup set Newsgroups: with Followup-To: if it present */
    if (!env->newsgroups && curenv && !mutt_istr_equal(curenv->followup_to, "poster"))
    {
      env->newsgroups = mutt_str_dup(curenv->followup_to);
    }
  }
  else
#endif
  {
    if (parent)
    {
      if (mutt_fetch_recips(env, curenv, flags, NeoMutt->sub) == -1)
        return -1;
    }
    else
    {
      for (short i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged &&
            (mutt_fetch_recips(env, actx->idx[i]->body->email->env, flags,
                               NeoMutt->sub) == -1))
        {
          return -1;
        }
      }
    }

    if ((flags & SEND_LIST_REPLY) && TAILQ_EMPTY(&env->to))
    {
      mutt_error(_("No mailing lists found"));
      return -1;
    }

    mutt_fix_reply_recipients(env, NeoMutt->sub);
  }
  mutt_make_misc_reply_headers(env, curenv, NeoMutt->sub);

  if (parent)
    mutt_add_to_reference_headers(env, curenv, NeoMutt->sub);
  else
  {
    for (short i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged)
      {
        mutt_add_to_reference_headers(env, actx->idx[i]->body->email->env,
                                      NeoMutt->sub);
      }
    }
  }

  return 0;
}

/**
 * attach_include_reply - This is _very_ similar to send.c's include_reply()
 * @param fp     File handle to attachment
 * @param fp_tmp File handle to temporary file
 * @param e      Email
 */
static void attach_include_reply(FILE *fp, FILE *fp_tmp, struct Email *e)
{
  CopyMessageFlags cmflags = MUTT_CM_PREFIX | MUTT_CM_DECODE | MUTT_CM_CHARCONV;
  CopyHeaderFlags chflags = CH_DECODE;

  mutt_make_attribution(e, fp_tmp, NeoMutt->sub);

  const bool c_header = cs_subset_bool(NeoMutt->sub, "header");
  if (!c_header)
    cmflags |= MUTT_CM_NOHEADER;
  const bool c_weed = cs_subset_bool(NeoMutt->sub, "weed");
  if (c_weed)
  {
    chflags |= CH_WEED;
    cmflags |= MUTT_CM_WEED;
  }

  mutt_copy_message_fp(fp_tmp, fp, e, cmflags, chflags, 0);
  mutt_make_post_indent(e, fp_tmp, NeoMutt->sub);
}

/**
 * mutt_attach_reply - Attach a reply
 * @param fp    File handle to reply
 * @param m     Mailbox
 * @param e     Email
 * @param actx  Attachment Context
 * @param e_cur   Current message
 * @param flags Send mode, see #SendFlags
 */
void mutt_attach_reply(FILE *fp, struct Mailbox *m, struct Email *e,
                       struct AttachCtx *actx, struct Body *e_cur, SendFlags flags)
{
  bool mime_reply_any = false;

  short nattach = 0;
  struct AttachPtr *parent = NULL;
  struct Email *e_parent = NULL;
  FILE *fp_parent = NULL;
  struct Email *e_tmp = NULL;
  FILE *fp_tmp = NULL;
  struct Buffer *tmpbody = NULL;
  struct EmailList el = STAILQ_HEAD_INITIALIZER(el);

  char prefix[128] = { 0 };

#ifdef USE_NNTP
  if (flags & SEND_NEWS)
    OptNewsSend = true;
  else
    OptNewsSend = false;
#endif

  if (!check_all_msg(actx, e_cur, false))
  {
    nattach = count_tagged(actx);
    parent = find_parent(actx, e_cur, nattach);
    if (parent)
    {
      e_parent = parent->body->email;
      fp_parent = parent->fp;
    }
    else
    {
      e_parent = e;
      fp_parent = actx->fp_root;
    }
  }

  if ((nattach > 1) && !check_can_decode(actx, e_cur))
  {
    const enum QuadOption c_mime_forward_rest = cs_subset_quad(NeoMutt->sub, "mime_forward_rest");
    const enum QuadOption ans = query_quadoption(
        c_mime_forward_rest,
        _("Can't decode all tagged attachments.  MIME-encapsulate the others?"));
    if (ans == MUTT_ABORT)
      return;
    if (ans == MUTT_YES)
      mime_reply_any = true;
  }
  else if (nattach == 1)
    mime_reply_any = true;

  e_tmp = email_new();
  e_tmp->env = mutt_env_new();

  if (attach_reply_envelope_defaults(e_tmp->env, actx,
                                     e_parent ? e_parent : (e_cur ? e_cur->email : NULL),
                                     flags) == -1)
  {
    goto cleanup;
  }

  tmpbody = mutt_buffer_pool_get();
  mutt_buffer_mktemp(tmpbody);
  fp_tmp = mutt_file_fopen(mutt_buffer_string(tmpbody), "w");
  if (!fp_tmp)
  {
    mutt_error(_("Can't create %s"), mutt_buffer_string(tmpbody));
    goto cleanup;
  }

  if (!e_parent)
  {
    if (e_cur)
      attach_include_reply(fp, fp_tmp, e_cur->email);
    else
    {
      for (short i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged)
          attach_include_reply(actx->idx[i]->fp, fp_tmp, actx->idx[i]->body->email);
      }
    }
  }
  else
  {
    mutt_make_attribution(e_parent, fp_tmp, NeoMutt->sub);

    struct State st;
    memset(&st, 0, sizeof(struct State));
    st.fp_out = fp_tmp;

    const bool c_text_flowed = cs_subset_bool(NeoMutt->sub, "text_flowed");
    if (c_text_flowed)
    {
      mutt_str_copy(prefix, ">", sizeof(prefix));
    }
    else
    {
      const char *const c_indent_string = cs_subset_string(NeoMutt->sub, "indent_string");
      mutt_make_string(prefix, sizeof(prefix), 0, NONULL(c_indent_string), m,
                       -1, e_parent, MUTT_FORMAT_NO_FLAGS, NULL);
    }

    st.prefix = prefix;
    st.flags = MUTT_CHARCONV;

    const bool c_weed = cs_subset_bool(NeoMutt->sub, "weed");
    if (c_weed)
      st.flags |= MUTT_WEED;

    const bool c_header = cs_subset_bool(NeoMutt->sub, "header");
    if (c_header)
      include_header(true, fp_parent, e_parent, fp_tmp, prefix);

    if (e_cur)
    {
      if (mutt_can_decode(e_cur))
      {
        st.fp_in = fp;
        mutt_body_handler(e_cur, &st);
        state_putc(&st, '\n');
      }
      else
        mutt_body_copy(fp, &e_tmp->body, e_cur);
    }
    else
    {
      for (short i = 0; i < actx->idxlen; i++)
      {
        if (actx->idx[i]->body->tagged && mutt_can_decode(actx->idx[i]->body))
        {
          st.fp_in = actx->idx[i]->fp;
          mutt_body_handler(actx->idx[i]->body, &st);
          state_putc(&st, '\n');
        }
      }
    }

    mutt_make_post_indent(e_parent, fp_tmp, NeoMutt->sub);

    if (mime_reply_any && !e_cur && !copy_problematic_attachments(&e_tmp->body, actx, false))
    {
      goto cleanup;
    }
  }

  mutt_file_fclose(&fp_tmp);

  emaillist_add_email(&el, e_parent ? e_parent : (e_cur ? e_cur->email : NULL));
  if (mutt_send_message(flags, e_tmp, mutt_buffer_string(tmpbody), NULL, &el,
                        NeoMutt->sub) == 0)
  {
    mutt_set_flag(m, e, MUTT_REPLIED, true);
  }
  e_tmp = NULL; /* mutt_send_message frees this */

cleanup:
  if (fp_tmp)
  {
    mutt_file_fclose(&fp_tmp);
    mutt_file_unlink(mutt_buffer_string(tmpbody));
  }
  mutt_buffer_pool_release(&tmpbody);
  email_free(&e_tmp);
  emaillist_clear(&el);
}

/**
 * mutt_attach_mail_sender - Compose an email to the sender in the email attachment
 * @param actx Attachment Context
 * @param cur  Current attachment
 */
void mutt_attach_mail_sender(struct AttachCtx *actx, struct Body *cur)
{
  if (!check_all_msg(actx, cur, 0))
  {
    /* L10N: You will see this error message if you invoke <compose-to-sender>
       when you are on a normal attachment.  */
    mutt_error(_("You may only compose to sender with message/rfc822 parts"));
    return;
  }

  struct Email *e_tmp = email_new();
  e_tmp->env = mutt_env_new();

  if (cur)
  {
    if (mutt_fetch_recips(e_tmp->env, cur->email->env, SEND_TO_SENDER, NeoMutt->sub) == -1)
    {
      email_free(&e_tmp);
      return;
    }
  }
  else
  {
    for (int i = 0; i < actx->idxlen; i++)
    {
      if (actx->idx[i]->body->tagged &&
          (mutt_fetch_recips(e_tmp->env, actx->idx[i]->body->email->env,
                             SEND_TO_SENDER, NeoMutt->sub) == -1))
      {
        email_free(&e_tmp);
        return;
      }
    }
  }

  // This call will free e_tmp for us
  mutt_send_message(SEND_NO_FLAGS, e_tmp, NULL, NULL, NULL, NeoMutt->sub);
}
