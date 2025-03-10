/**
 * @file
 * Test code for mutt_date_localtime()
 *
 * @authors
 * Copyright (C) 2019 Richard Russon <rich@flatcap.org>
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

#define TEST_NO_MAIN
#include "config.h"
#include "acutest.h"
#include <stdlib.h>
#include <time.h>
#include "mutt/lib.h"

void test_mutt_date_localtime(void)
{
  // struct tm mutt_date_localtime(time_t t);

  setenv("TZ", "UTC", 1);

  {
    TEST_CASE("December, 2000");
    struct tm tm = mutt_date_localtime(977745600);
    TEST_CHECK(tm.tm_sec == 0);
    TEST_CHECK(tm.tm_hour == 12);
    TEST_CHECK(tm.tm_mday == 25);
    TEST_CHECK(tm.tm_mon == 11);
    TEST_CHECK(tm.tm_year == 100);
    TEST_CHECK(tm.tm_wday == 1);
    TEST_CHECK(tm.tm_yday == 359);
  }

  {
    TEST_CASE("June, 2000");
    struct tm tm = mutt_date_localtime(961930800);
    TEST_CHECK(tm.tm_sec == 0);
    TEST_CHECK((tm.tm_hour == 12) || // Expected result...
               (tm.tm_hour == 11));  // but Travis seems to have locale problems
    TEST_MSG("hour = %d", tm.tm_hour);
    TEST_CHECK(tm.tm_mday == 25);
    TEST_CHECK(tm.tm_mon == 5);
    TEST_CHECK(tm.tm_year == 100);
    TEST_CHECK(tm.tm_wday == 0);
    TEST_CHECK(tm.tm_yday == 176);
  }

  {
    TEST_CASE("Today");
    struct tm tm = mutt_date_localtime(MUTT_DATE_NOW);
    TEST_CHECK(tm.tm_year >= 119);
  }
}
