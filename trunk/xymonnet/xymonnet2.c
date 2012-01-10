/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* Copyright (C) 2011-2012 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: dns2.c 6743 2011-09-03 15:44:52Z storner $";

#include <string.h>
#include <stdio.h>

#include "libxymon.h"
#include "setuptests.h"
#include "tcptalk.h"
#include "dnstalk.h"

int main(int argc, char **argv)
{

	debug = 1;
	conn_register_infohandler(NULL, 7);

	init_tcp_testmodule();
	dns_lookup_init();

	setup_tests();
	run_net_tests();

	dump_net_tests(NULL);

	conn_deinit();
	dns_lookup_shutdown();

	return 0;
}

