/*
 * Copyright (C) 2011 matt mooney <mfm@muteddisk.com>
 *               2005-2007 Takahiro Hirofuchi
 *               2022-2023 Vadym Hrynchyshyn
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "strings.h"

#include <libusbip\getopt.h>
#include <spdlog\spdlog.h>

int list_exported_devices(const char *host);

static const char usbip_list_usage_string[] =
	"usbip list [-p|--parsable] <args>\n"
	"    -p, --parsable         Parsable list format\n"
	"    -r, --remote=<host>    List the exported USB devices on <host>\n"
	;

void usbip_list_usage()
{
	printf("usage: %s", usbip_list_usage_string);
}

int usbip_list(int argc, char *argv[])
{
	const struct option opts[] = {
		{ "parsable", no_argument, nullptr, 'p' },
		{ "remote", required_argument, nullptr, 'r' },
		{}
	};

	BOOL parsable = FALSE;
	int opt;
	int ret = 1;

	for (;;) {
		opt = getopt_long(argc, argv, "pr:", opts, nullptr);

		if (opt == -1)
			break;

		switch (opt) {
		case 'p':
			parsable = TRUE;
			break;
		case 'r':
			ret = list_exported_devices(optarg);
			goto out;
			break;
		}
	}

	spdlog::error("-r option required");
	usbip_list_usage();
out:
	return ret;
}
