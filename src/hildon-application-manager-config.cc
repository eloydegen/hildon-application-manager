/*
 * This file is part of the hildon-application-manager.
 *
 * Copyright (C) 2007 Nokia Corporation.  All Rights reserved.
 *
 * Contact: Marius Vollmer <marius.vollmer@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/* HILDON-APPLICATION-MANAGER-CONFIG

   Utility for automatically editing the Application Manager
   configuration.

   Usage: hildon-application-manager-config add FILE...
          hildon-application-manager-config delete FILE...
          hildon-application-manager-config set FILE...
          hildon-application-manager-config update
          hildon-application-manager-config dump

   The "add" sub-command will merge the configuration elements in the
   files given on the command line with the existing configuration and
   the "delete" sub-command will delete them.  The "set" sub-command
   will first clear the existing configuration and the add the bits in
   the files.  Afterwards the new configuration is used to update all
   the dependend bits, like the APT sources.list configuration.

   The "update" command will just do the updating, without any changes
   to the existing configuration.

   The "dump" command will dump the current configuration in a form
   that can be used with "set" to restore it.

   The configuration elements in the files can be either catalogues or
   domains.  XXX - more about them here?
*/

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

#include "confutils.h"

#ifdef DEBUG
static void
DBG (const char *str, xexp *cat)
{
  fprintf (stderr, "%s:\n", str);
  if (cat)
    xexp_write (stderr, cat);
  else
    fprintf (stderr, "NULL\n");
}
#else
static void
DBG (const char *str, xexp *cat)
{
}
#endif

bool verbose;

xexp *catalogues;
xexp *domains;

void
read_conf ()
{
  catalogues = xexp_read_file (CATALOGUE_CONF);
  if (catalogues == NULL)
    exit (1);

  domains = xexp_read_file (DOMAIN_CONF);
  if (domains == NULL)
    exit (1);
}

void
reset_conf ()
{
  catalogues = xexp_list_new ("catalogues");
  domains = xexp_list_new ("domains");
}

void
write_conf ()
{
  if (!xexp_write_file (DOMAIN_CONF, domains))
    exit (1);

  if (!xexp_write_file (CATALOGUE_CONF, catalogues))
    exit (1);
}

void
update_conf ()
{
  if (!write_sources_list (CATALOGUE_APT_SOURCE, catalogues))
    exit (1);
}

xexp *
find_element (xexp *conf, xexp *elt,
	      bool (*equal) (xexp *a, xexp *b))
{
  for (xexp *x = xexp_first (conf); x; x = xexp_rest (x))
    if (equal (elt, x))
      return x;
  return NULL;
}

const char *
element_description (xexp *element)
{
  const char *desc = xexp_aref_text (element, "name");
  if (desc == NULL)
    desc = "<unnamed>";
  return desc;
}

void
handle_generic_element (xexp *conf, xexp *element,
			bool add,
			bool (*equal) (xexp *a, xexp *b))
{
  xexp *old_element = find_element (conf, element, equal);

  if (verbose)
    {
      if (old_element && add)
	printf ("Replacing %s\n", element_description (element));
      else if(old_element == NULL && add)
	printf ("Adding %s\n", element_description (element));
      else if (old_element && !add)
	printf ("Removing %s\n", element_description (element));
      else
	printf ("Not found: %s\n", element_description (element));
    }

  if (old_element)
    xexp_del (conf, old_element);
  if (add)
    xexp_append_1 (conf, element);
  else
    xexp_free (element);
}

void
handle_element (xexp *element, bool add)
{
  xexp *conf = NULL;
  bool (*equal) (xexp *a, xexp *b) = NULL;

  if (xexp_is (element, "catalogue"))
    {
      conf = catalogues;
      equal = catalogue_equal;
    }
  else if (xexp_is (element, "domain"))
    {
      conf = domains;
      equal = domain_equal;
    }
  else
    {
      fprintf (stderr, "hildon-application-manager-config: "
	       "unsupported configuration element: %s\n", 
	       xexp_tag (element));
      return;
    }

  handle_generic_element (conf, element, add, equal);
}

xexp *
xexp_read_stdin ()
{
  GError *error = NULL;

  xexp *x = xexp_read (stdin, &error);
  if (x == NULL)
    fprintf (stderr, "stdin: %s\n", error->message);
  return x;
}

void
handle_file (const char *file, bool add)
{
  xexp *elements;

  if (strcmp (file, "-") == 0)
    elements = xexp_read_stdin ();
  else
    elements = xexp_read_file (file);

  if (elements == NULL)
    return;

  while (xexp *element = xexp_pop (elements))
    handle_element (element, add);
}

void
handle_files (char **files, bool add)
{
  while (*files)
    {
      handle_file (*files, add);
      files++;
    }
}

void
usage ()
{
  fprintf (stderr,
	   "Usage: hildon-application-manager-config add FILE...\n"
	   "       hildon-application-manager-config delete FILE...\n"
	   "       hildon-application-manager-config set FILE...\n"
	   "       hildon-application-manager-config update\n"
	   "       hildon-application-manager-config dump\n");
  exit (1);
}

int
main (int argc, char **argv)
{
  GError *error = NULL;

  if (argc > 1 && strcmp (argv[1], "-v") == 0)
    {
      verbose = true;
      argc--;
      argv++;
    }

  if (argc < 2)
    usage ();

  const char *cmd = argv[1];
  char **files = argv+2;

  if (strcmp (cmd, "update") == 0)
    {
      read_conf ();
      update_conf ();
    }
  else if (strcmp (cmd, "add") == 0)
    {
      read_conf ();
      handle_files (files, true);
      write_conf ();
      update_conf ();
    }
  else if (strcmp (cmd, "delete") == 0)
    {
      read_conf ();
      handle_files (files, false);
      write_conf ();
      update_conf ();
    }
  else if (strcmp (cmd, "set") == 0)
    {
      reset_conf ();
      handle_files (files, true);
      write_conf ();
      update_conf ();
    }
  else if (strcmp (cmd, "dump") == 0)
    {
      read_conf ();
      xexp *conf = xexp_list_new ("config");
      xexp_append (conf, catalogues);
      xexp_append (conf, domains);
      xexp_write (stdout, conf);
    }
  else
    usage ();

  return 0;
}