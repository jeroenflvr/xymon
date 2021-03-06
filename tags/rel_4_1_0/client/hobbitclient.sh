#!/bin/sh
#----------------------------------------------------------------------------#
# Hobbit client main script.                                                 #
#                                                                            #
# This invokes the OS-specific script to build a client message, and sends   #
# if off to the Hobbit server.                                               #
#                                                                            #
# Copyright (C) 2005 Henrik Storner <henrik@hswn.dk>                         #
#                                                                            #
# This program is released under the GNU General Public License (GPL),       #
# version 2. See the file "COPYING" for details.                             #
#                                                                            #
#----------------------------------------------------------------------------#
#
# $Id: hobbitclient.sh,v 1.2 2005-07-24 11:32:51 henrik Exp $

echo "client $MACHINE.$BBOSTYPE"      >  $BBTMP/msg.txt
$BBHOME/bin/hobbitclient-$BBOSTYPE.sh >> $BBTMP/msg.txt
$BB $BBDISP "@" < $BBTMP/msg.txt

exit 0

