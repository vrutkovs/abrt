bin_PROGRAMS = abrt-cli

CLI_C =
CLI_C += abrt-cli.c
CLI_C += abrt-cli-core.c

BUILTIN_C =
BUILTIN_C += list.c
BUILTIN_C += rm.c
BUILTIN_C += report.c
BUILTIN_C += status.c

abrt_cli_SOURCES = $(CLI_C) $(BUILTIN_C) builtin-cmd.h abrt-cli-core.h

abrt_cli_CFLAGS = \
	-I$(srcdir)/../include \
	-I$(srcdir)/../lib \
	$(LIBREPORT_CFLAGS)

abrt_cli_LDADD = \
    $(LIBREPORT_LIBS) \
    ../lib/libabrt.la

profileconfigdir = $(sysconfdir)/profile.d
dist_profileconfig_DATA = \
    abrt-console-notification.sh

DEFS = -DLOCALEDIR=\"$(localedir)\" @DEFS@
