/*
 * Copyright (C) 2000, 2001, 2002 Loic Dachary <loic@senga.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Run unac_string on an input large enough to trigger re-allocation.
 */ 

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "unac.h"

static char* longstr_expected =
"\n"
"Senga - Catalog software\n"
"\n"
"\n"
"\n"
"\n"
"\n"
"\n"
"   \n"
"    \n"
" \n"
"\n"
"    \n"
"  \n"
"   \n"
"     \n"
"       \n"
"\n"
"  \n"
"  \n"
"   senga.org\n"
"   \n"
"\n"
"  \n"
"\n"
"\n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"\n"
"\n"
"\n"
"\n"
"\n"
"December 28, 2000 \n"
"     \n"
"      January 27, 2000\n"
"      Catalog-1.02 \n"
"        is available. \n"
"      \n"
"       The dmoz loading process has been dramatically simplified. It is\n"
"          now only available as a command. No more fancy web interface that\n"
"      confuses everyone. In addition the convert_dmoz script now generates\n"
"      text files that can be directly loaded into Catalog instead of the\n"
"      intermediate XML file. The whole loading process now takes from \n"
"      one to two hours depending on your machine. It took around 10 hours\n"
"      with the previous version. \n"
"       The -exclude option was added to convert_dmoz to get rid of \n"
"          a whole branch of the catalog at load time. Typical usage would\n"
"      be convert_dmoz -exclude '^/Adult' -what content content.rdf.gz.\n"
"       A lot more sanity checks and repair have been added to deal with\n"
"          duplicates, category id conflicts and the like.\n"
"       Hopefully this new method will also be more understandable and \n"
"          generate less traffic on the mailing list. There is room for \n"
"      improvements and contributors are welcome. \n"
"      \n"
"      A new set of software is available in the \n"
"      download directory under the RedHat-6.1 section. These\n"
"      are the most up to date versions on which Catalog depends. Although the\n"
"      binaries depend on RedHat-6.1 the perl modules are source and can be\n"
"      used on any platform.\n"
"      \n"
"      September 7, 1999\n"
"      Catalog-1.01 \n"
"        is available. \n"
"      This is a maintenance release.\n"
"      \n"
"        Various bug fixes. All easy\n"
"      to fix bugs have been fixed. Take a look at Bug Track to see what hasn't been fixed.\n"
"        The _PATHTEXT_ and _PATHFILE_ \n"
"      tags syntax has been extended to specify a range of path component.\n"
"          \n"
"        Graham Barr added a recursive\n"
"      template feature for a catalog root page. This allows to show sub-categories\n"
"      of the root categories in the root page of a catalog.\n"
"          \n"
"      \n"
"      Don't hesitate to submit bugs\n"
"        or ideas to bug track. Hopefully the next version of Catalog will have\n"
"    a fast full text indexing mechanism and I'll be able to implement new\n"
"    functionalities.\n"
"        \n"
"      Have fun !\n"
"      July 3, 1999\n"
"      Catalog-1.00 \n"
"        is available. \n"
"      This release includes PHP3 \n"
"        code to display a catalog. The author is Weston Bustraan (weston@infinityteldata.net). \n"
"        The main motivation to jump directly to version 1.00 is to avoid version \n"
"        number problems on CPAN. \n"
"      July 2, 1999\n"
"      Catalog-0.19 \n"
"        is available. \n"
"      This is a minor release. The \n"
"        most noticeable addition is the new search mechanism.\n"
"      \n"
"        Searching : two search modes \n"
"          are now available. AltaVista simple syntax and AltaVista advanced syntax. \n"
"          Both use the Text-Query and Text-Query-SQL perl modules. \n"
"        Dmoz loading is much more \n"
"          fault tolerant. In addition it can handle compressed versions of content.rdf \n"
"          and structure.rdf. The comments are now stored in text fields instead \n"
"          of char(255).\n"
"        The template system was \n"
"          extended with the pre_fill and post_fill parameters.\n"
"        Searching associated to \n"
"          a catalog dumped to static pages is now possible using the 'static' \n"
"          mode.\n"
"        Fixed two security weakness \n"
"          in confedit and recursive cgi handling.\n"
"        Many sql queries have been \n"
"          optimized.\n"
"        The configuration was changed \n"
"          a bit to fix bugs and to isolate database dependencies.\n"
"        The tests were updated to \n"
"          isolate database dependencies. \n"
"        Fixed numerous minor bugs, \n"
"          check ChangeLog if you're interested in details.\n"
"      \n"
"      Many thanks to Tim Bunce for \n"
"        his numerous contributions and ideas. He is the architect of the Text-Query \n"
"        and Text-Query-SQL modules, Eric Bohlman and Loic Dachary did the programming. \n"
"        \n"
"      Thanks to Eric Bohlman for \n"
"        his help on the Text-Query module. He was very busy but managed to spend \n"
"        the time needed to release it. \n"
"      There is not yet anything usable \n"
"        for full text indexing but we keep working on it. The storage management \n"
"        is now handled by the reiserfs file system thanks to Hans Reiser who is \n"
"        working full time on this. Loic Dachary does his best to get something \n"
"        working, if you're interested go to http://www.senga.org/mifluz/. \n"
"      For some mysterious reason \n"
"        CPAN lost track of Catalog name. In order to install catalog you should \n"
"        use perl -MCPAN -e 'install Catalog::db'. Weird but temporary.\n"
"      Have fun !\n"
"       The Senga Team\n"
"        Ecila\n"
"        100 Av. du General Leclerc\n"
"        93 500 Pantin\n"
"        Tel: 33 1 56 96 09 80\n"
"        Fax: 33 1 56 96 09 81\n"
"        WEB: http://www.senga.org/\n"
"        Mail: senga@senga.org\n"
"      \n"
"    \n"
"  \n"
"  \n"
"     \n"
"    \n"
"      \n"
"\n"
"[\n"
"Catalog |\n"
"webbase |\n"
"mifluz |\n"
"unac |\n"
"Search-Mifluz |\n"
"Text-Query |\n"
"uri |\n"
"Statistics |\n"
"News\n"
"]\n"
"\n"
"\n"
"    \n"
"  \n"
"\n"
"\n"
"\n"
;

static char* longstr =
"\n"
"Senga - Catalog software\n"
"\n"
"\n"
"\n"
"\n"
"\n"
"\n"
"   \n"
"    \n"
" \n"
"\n"
"    \n"
"  \n"
"   \n"
"     \n"
"      �\n"
"\n"
"  \n"
"  \n"
"   senga.org\n"
"   \n"
"\n"
"� \n"
"\n"
"\n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"   \n"
"    \n"
"  \n"
"\n"
"\n"
"\n"
"\n"
"\n"
"December 28, 2000 \n"
"     \n"
"      January 27, 2000\n"
"      Catalog-1.02 \n"
"        is available. \n"
"      \n"
"       The dmoz loading process has been dramatically simplified. It is\n"
"          now only available as a command. No more fancy web interface that\n"
"      confuses everyone. In addition the convert_dmoz script now generates\n"
"      text files that can be directly loaded into Catalog instead of the\n"
"      intermediate XML file. The whole loading process now takes from \n"
"      one to two hours depending on your machine. It took around 10 hours\n"
"      with the previous version. \n"
"       The -exclude option was added to convert_dmoz to get rid of \n"
"          a whole branch of the catalog at load time. Typical usage would\n"
"      be convert_dmoz -exclude '^/Adult' -what content content.rdf.gz.\n"
"       A lot more sanity checks and repair have been added to deal with\n"
"          duplicates, category id conflicts and the like.\n"
"       Hopefully this new method will also be more understandable and \n"
"          generate less traffic on the mailing list. There is room for \n"
"      improvements and contributors are welcome. \n"
"      \n"
"      A new set of software is available in the \n"
"      download directory under the RedHat-6.1 section. These\n"
"      are the most up to date versions on which Catalog depends. Although the\n"
"      binaries depend on RedHat-6.1 the perl modules are source and can be\n"
"      used on any platform.\n"
"      \n"
"      September 7, 1999\n"
"      Catalog-1.01 \n"
"        is available. \n"
"      This is a maintenance release.\n"
"      \n"
"        Various bug fixes. All easy\n"
"      to fix bugs have been fixed. Take a look at Bug Track to see what hasn't been fixed.\n"
"        The _PATHTEXT_ and _PATHFILE_ \n"
"      tags syntax has been extended to specify a range of path component.\n"
"          \n"
"        Graham Barr added a recursive\n"
"      template feature for a catalog root page. This allows to show sub-categories\n"
"      of the root categories in the root page of a catalog.\n"
"          \n"
"      \n"
"      Don't hesitate to submit bugs\n"
"        or ideas to bug track. Hopefully the next version of Catalog will have\n"
"    a fast full text indexing mechanism and I'll be able to implement new\n"
"    functionalities.\n"
"        \n"
"      Have fun !\n"
"      July 3, 1999\n"
"      Catalog-1.00 \n"
"        is available. \n"
"      This release includes PHP3 \n"
"        code to display a catalog. The author is Weston Bustraan (weston@infinityteldata.net). \n"
"        The main motivation to jump directly to version 1.00 is to avoid version \n"
"        number problems on CPAN. \n"
"      July 2, 1999\n"
"      Catalog-0.19 \n"
"        is available. \n"
"      This is a minor release. The \n"
"        most noticeable addition is the new search mechanism.\n"
"      \n"
"        Searching : two search modes \n"
"          are now available. AltaVista simple syntax and AltaVista advanced syntax. \n"
"          Both use the Text-Query and Text-Query-SQL perl modules. \n"
"        Dmoz loading is much more \n"
"          fault tolerant. In addition it can handle compressed versions of content.rdf \n"
"          and structure.rdf. The comments are now stored in text fields instead \n"
"          of char(255).\n"
"        The template system was \n"
"          extended with the pre_fill and post_fill parameters.\n"
"        Searching associated to \n"
"          a catalog dumped to static pages is now possible using the 'static' \n"
"          mode.\n"
"        Fixed two security weakness \n"
"          in confedit and recursive cgi handling.\n"
"        Many sql queries have been \n"
"          optimized.\n"
"        The configuration was changed \n"
"          a bit to fix bugs and to isolate database dependencies.\n"
"        The tests were updated to \n"
"          isolate database dependencies. \n"
"        Fixed numerous minor bugs, \n"
"          check ChangeLog if you're interested in details.\n"
"      \n"
"      Many thanks to Tim Bunce for \n"
"        his numerous contributions and ideas. He is the architect of the Text-Query \n"
"        and Text-Query-SQL modules, Eric Bohlman and Loic Dachary did the programming. \n"
"        \n"
"      Thanks to Eric Bohlman for \n"
"        his help on the Text-Query module. He was very busy but managed to spend \n"
"        the time needed to release it. \n"
"      There is not yet anything usable \n"
"        for full text indexing but we keep working on it. The storage management \n"
"        is now handled by the reiserfs file system thanks to Hans Reiser who is \n"
"        working full time on this. Loic Dachary does his best to get something \n"
"        working, if you're interested go to http://www.senga.org/mifluz/. \n"
"      For some mysterious reason \n"
"        CPAN lost track of Catalog name. In order to install catalog you should \n"
"        use perl -MCPAN -e 'install Catalog::db'. Weird but temporary.\n"
"      Have fun !\n"
"       The Senga Team\n"
"        Ecila\n"
"        100 Av. du G�n�ral Leclerc\n"
"        93 500 Pantin\n"
"        Tel: 33 1 56 96 09 80\n"
"        Fax: 33 1 56 96 09 81\n"
"        WEB: http://www.senga.org/\n"
"        Mail: senga@senga.org\n"
"      \n"
"    \n"
"  \n"
"  \n"
"    �\n"
"    \n"
"      \n"
"\n"
"[\n"
"Catalog |\n"
"webbase |\n"
"mifluz |\n"
"unac |\n"
"Search-Mifluz |\n"
"Text-Query |\n"
"uri |\n"
"Statistics |\n"
"News\n"
"]\n"
"\n"
"\n"
"    \n"
"  \n"
"\n"
"\n"
"\n"
;

int main() {
  int i;
  char* out = 0;
  size_t out_length = 0;
  {
    if(unac_string("ISO-8859-1", "�t�", 3, &out, &out_length) < 0) {
      perror("unac �t�");
      exit(1);
    }
    if(out_length != 3) {
      fprintf(stderr, "out_length == %d instead of 3\n", (int)out_length);
      exit(1);
    }
    if(memcmp("ete", out, out_length)) {
      fprintf(stderr, "out == %.*s instead of ete\n", (int)out_length, out);
      exit(1);
    }

  }

  {
    char tmp[10];
    sprintf(tmp, "%c", 0xBC);
    if(unac_string("ISO-8859-1", tmp, 1, &out, &out_length) < 0) {
      perror("unac 0xBC (1/4)");
      exit(1);
    }
    if(out_length != 3) {
      fprintf(stderr, "out_length == %d instead of 3\n", (int)out_length);
      exit(1);
    }
    if(memcmp("1 4", out, out_length)) {
      fprintf(stderr, "out == %.*s instead of '1 4'\n", (int)out_length, out);
      exit(1);
    }

  }

  for(i = 0; i < 3; i++) {
    int longstr_length = strlen(longstr);
    if(unac_string("ISO-8859-1", longstr, longstr_length, &out, &out_length) == -1) {
      perror("unac_string longstr failed");
      exit(1);
    }
    if(out_length != longstr_length) {
      fprintf(stderr, "out_length == %d instead of %d\n", (int)out_length, longstr_length);
      exit(1);
    }
    if(memcmp(longstr_expected, out, out_length)) {
      fprintf(stderr, "out == %.*s instead of ete\n", (int)out_length, out);
      exit(1);
    }

  }

  free(out);

  return 0;
}
