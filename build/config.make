# config.make.  Generated from config.make.in by configure.

package-version := 0.9
# What version of the Hurd is this?  For compatibility (libraries' SONAMEs),
# hard-code this to 0.3 instead of coupling with PACKAGE_VERSION.
hurd-version := 0.3

# Machine architecture.
machine = i686
asm_syntax = i386

# Build options.
build-profiled = 
build-static = ext2fs iso9660fs rumpdisk pci-arbiter acpi
boot-store-types = device remap part gunzip

# Prefix prepended to names of machine-independent installed files.
prefix = 
# Prefix prepended to names of machine-dependent installed files.
exec_prefix = ${prefix}

# Directories where things get installed.
hurddir = ${exec_prefix}/hurd
libdir = ${exec_prefix}/lib
bindir = ${exec_prefix}/bin
sbindir = ${exec_prefix}/sbin
includedir = ${prefix}/include
libexecdir = ${exec_prefix}/libexec
bootdir = ${exec_prefix}/boot
infodir = ${datarootdir}/info
sysconfdir = ${prefix}/etc
localstatedir = ${prefix}/var
sharedstatedir = ${prefix}/com
datadir = ${datarootdir}
datarootdir = ${prefix}/share

# All of those directories together:
installationdirlist = $(hurddir) $(libdir) $(bindir) $(sbindir) \
	$(includedir) $(libexecdir) $(bootdir) $(infodir) $(sysconfdir) \
	$(localstatedir) $(sharedstatedir)


# How to run compilation tools.
CC = gcc
CPP = $(CC) -E -x c # We need this option when input file names are not *.c.
LD = ld
OBJCOPY = objcopy
AR = ar
RANLIB = ranlib
MIG = mig
MIGCOM = $(MIG) -cc cat - /dev/null
AWK = mawk
SED = /usr/bin/sed

# Compilation flags.  Append these to the definitions already made by
# the specific Makefile.
CPPFLAGS +=  -DPACKAGE_NAME=\"GNU\ Hurd\" -DPACKAGE_TARNAME=\"hurd\" -DPACKAGE_VERSION=\"0.9\" -DPACKAGE_STRING=\"GNU\ Hurd\ 0.9\" -DPACKAGE_BUGREPORT=\"bug-hurd@gnu.org\" -DPACKAGE_URL=\"https://www.gnu.org/software/hurd/\" -DHAVE_FILE_EXEC_PATHS=1 -DHAVE_EXEC_EXEC_PATHS=1 -DHAVE__HURD_EXEC_PATHS=1 -DHAVE__HURD_LIBC_PROC_INIT=1 -DHAVE_FILE_UTIMENS=1 -DUTIME_NOW=-1 -DUTIME_OMIT=-2 -DHAVE_LIBCRYPT=1 -DHAVE_STDIO_H=1 -DHAVE_STDLIB_H=1 -DHAVE_STRING_H=1 -DHAVE_INTTYPES_H=1 -DHAVE_STDINT_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_UNISTD_H=1 -DSTDC_HEADERS=1 -DHAVE_PARTED_PARTED_H=1 -DHAVE_LIBPARTED=1 -DHAVE_LIBUUID=1 -DHAVE_LIBDL=1 -DHAVE_STRUCT_THREAD_SCHED_INFO_LAST_PROCESSOR=1 -DHAVE_STRUCT_MAPPED_TIME_VALUE_TIME_VALUE_SECONDS=1
CFLAGS += -g -O2
LDFLAGS += 

gnu89-inline-CFLAGS = -fgnu89-inline

# How to link against Parted libraries, if at all.
PARTED_LIBS = -lparted -luuid -ldl

# How to compile and link against ncursesw.
LIBNCURSESW = 
NCURSESW_INCLUDE = 

# How to compile and link against xkbcommon.
HAVE_XKBCOMMON = no
XKBCOMMON_CFLAGS = 
XKBCOMMON_LIBS = 

# How to compile and link against libdaemon.
libdaemon_CFLAGS = 
libdaemon_LIBS = 

# How to compile and link against libbz2.
HAVE_LIBBZ2 = 

# How to compile and link against libz.
HAVE_LIBZ = 1

# How to compile and link against libblkid.
libblkid_CFLAGS = 
libblkid_LIBS = 

# Whether Sun RPC support is available.
HAVE_SUN_RPC = no

# How to compile and link against libtirpc.
libtirpc_CFLAGS = 
libtirpc_LIBS = 

# Whether we found libcrypt.
HAVE_LIBCRYPT = 1

# Whether we found libgcrypt.
HAVE_LIBGCRYPT = no

# Whether we found liblwip.
HAVE_LIBLWIP = no

# Whether we found librump.
HAVE_LIBRUMP = no
HAVE_LIBRUMP_VFSNOFIFO = no

# How to compile and link against liblwip.
liblwip_CFLAGS = 
liblwip_LIBS = 

# Whether we found libpciaccess.
HAVE_LIBPCIACCESS = no

# How to compile and link against libpciaccess.
libpciaccess_CFLAGS = 
libpciaccess_LIBS = 

# Whether we found libacpica.
HAVE_LIBACPICA = no

# How to compile and link against libacpica.
libacpica_CFLAGS = 
libacpica_LIBS = 

# Installation tools.
INSTALL = /bin/bash /home/loshmi/dev/hurd/upstream/hurd/install-sh -c -C
INSTALL_PROGRAM = ${INSTALL}
INSTALL_DATA = ${INSTALL} -m 644
