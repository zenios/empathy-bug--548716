#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib/gstdio.h>

#include <check.h>
#include "check-helpers.h"
#include "check-libempathy.h"

#include <libempathy/empathy-chatroom-manager.h>

#define CHATROOM_SAMPLE "chatrooms-sample.xml"
#define CHATROOM_FILE "chatrooms.xml"

START_TEST (test_empathy_chatroom_manager_new)
{
  EmpathyChatroomManager *mgr;
  gchar *cmd;
  gchar *file;
  McProfile *profile;
  McAccount *account;

  profile = mc_profile_lookup ("test");
  account = mc_account_create (profile);

  copy_xml_file (CHATROOM_SAMPLE, CHATROOM_FILE);

  file = get_user_xml_file (CHATROOM_FILE);
  /* change the chatrooms XML file to use the account we just created */
  cmd = g_strdup_printf ("sed -i 's/CHANGE_ME/%s/' %s",
      mc_account_get_unique_name (account), file);
  system (cmd);
  g_free (cmd);

  mgr = empathy_chatroom_manager_new (file);

  fail_if (empathy_chatroom_manager_get_count (mgr, account) != 2);

  g_free (file);
  g_object_unref (mgr);
  g_object_unref (profile);
  remove_account_from_gconf (account);
  mc_account_delete (account);
  g_object_unref (account);
}
END_TEST

TCase *
make_empathy_chatroom_manager_tcase (void)
{
    TCase *tc = tcase_create ("empathy-chatroom-manager");
    tcase_add_test (tc, test_empathy_chatroom_manager_new);
    return tc;
}