#  
#  $Id: Makefile.am,v 1.12 2004/07/21 13:30:48 holtmann Exp $
#

includedir = @includedir@/bluetooth

include_HEADERS = \
	bluetooth.h hci.h hci_lib.h hci_uart.h hci_vhci.h \
	l2cap.h sco.h rfcomm.h sdp.h sdp_lib.h bnep.h cmtp.h hidp.h

noinst_HEADERS = sdp_internal.h

MAINTAINERCLEANFILES = Makefile.in

all-local:
	@if [ ! -e bluetooth ] ; then $(LN_S) $(top_srcdir)/include bluetooth ; fi

clean-local:
	@rm -f bluetooth
