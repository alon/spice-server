NULL =
SUBDIRS = . tests

AM_CPPFLAGS =					\
	-DSPICE_SERVER_INTERNAL			\
	-DRED_STATISTICS			\
	$(CELT051_CFLAGS)			\
	$(COMMON_CFLAGS)			\
	$(GLIB2_CFLAGS)				\
	$(PIXMAN_CFLAGS)			\
	$(SASL_CFLAGS)				\
	$(SLIRP_CFLAGS)				\
	$(SMARTCARD_CFLAGS)			\
	$(SSL_CFLAGS)				\
	$(VISIBILITY_HIDDEN_CFLAGS)		\
	$(WARN_CFLAGS)				\
	$(NULL)

lib_LTLIBRARIES = libspice-server.la

libspice_server_la_LDFLAGS =			\
	-version-info $(SPICE_LT_VERSION)	\
	-no-undefined				\
	$(RELRO_LDFLAGS)			\
	$(NO_INDIRECT_LDFLAGS)			\
	$(NULL)

if HAVE_LD_VERSION_SCRIPT
libspice_server_la_LDFLAGS += \
	-Wl,--version-script=$(top_srcdir)/server/spice-server.syms
endif

libspice_server_la_LIBADD =						\
	$(top_builddir)/spice-common/common/libspice-common.la		\
	$(top_builddir)/spice-common/common/libspice-common-server.la	\
	$(GL_LIBS)							\
	$(GLIB2_LIBS)							\
	$(JPEG_LIBS)							\
	$(LIBRT)							\
	$(PIXMAN_LIBS)							\
	$(SASL_LIBS)							\
	$(SLIRP_LIBS)							\
	$(SSL_LIBS)							\
	$(Z_LIBS)							\
	$(SPICE_NONPKGCONFIG_LIBS)					\
	$(NULL)

libspice_server_la_SOURCES =			\
	agent-msg-filter.c			\
	agent-msg-filter.h			\
	cache-item.h				\
	char-device.c				\
	char-device.h				\
	demarshallers.h				\
	glz-encoder.c				\
	glz-encoder.h				\
	glz-encoder-dict.c		\
	glz-encoder-dict.h		\
	glz-encoder-priv.h	\
	inputs-channel.c			\
	inputs-channel.h			\
	jpeg-encoder.c				\
	jpeg-encoder.h				\
	main-channel.c				\
	main-channel.h				\
	mjpeg-encoder.c				\
	mjpeg-encoder.h				\
	red_channel.c				\
	red_channel.h				\
	common.h				\
	dispatcher.c				\
	dispatcher.h				\
	red_dispatcher.c			\
	red_dispatcher.h			\
	main-dispatcher.c			\
	main-dispatcher.h			\
	migration-protocol.h		\
	memslot.c				\
	memslot.h				\
	red_parse_qxl.c				\
	red_record_qxl.c			\
	red_replay_qxl.c			\
	red_parse_qxl.h				\
	red_time.h				\
	red_worker.c				\
	red_worker.h				\
	display-channel.c			\
	display-channel.h			\
	cursor-channel.c			\
	cursor-channel.h			\
	reds.c					\
	reds.h					\
	reds-private.h				\
	reds_stream.c				\
	reds_stream.h				\
	sound.c				\
	sound.h				\
	spice-experimental.h			\
	spice.h					\
	stat.h					\
	spicevmc.c				\
	zlib-encoder.c				\
	zlib-encoder.h				\
	spice-bitmap-utils.h		\
	spice-bitmap-utils.c		\
	spice_server_utils.h		\
	image-cache.h			\
	image-cache.c			\
	pixmap-cache.h				\
	pixmap-cache.c				\
	tree.h				\
	tree.c				\
	utils.c					\
	utils.h					\
	stream.c					\
	stream.h					\
	dcc.c					\
	dcc-send.c					\
	dcc.h					\
	dcc-encoders.c					\
	dcc-encoders.h					\
	canvas.c					\
	canvas.h					\
	$(NULL)

if SUPPORT_SMARTCARD
libspice_server_la_SOURCES +=	\
	smartcard.c		\
	smartcard.h		\
	$(NULL)
endif

libspice_serverincludedir = $(includedir)/spice-server
libspice_serverinclude_HEADERS =		\
	spice.h					\
	spice-experimental.h			\
	$(NULL)

EXTRA_DIST =					\
	red_bitmap_utils.c.template			\
	cache-item.c.template			\
	glz-encode-match.c.template			\
	glz-encode.c.template			\
	spice-server.syms			\
	$(NULL)

BUILT_SOURCES = $(spice_built_sources)

dist-hook:
	if test ./tests/test_spice_version.sh && ! test ./tests/test_spice_version.sh; then exit 1; fi
