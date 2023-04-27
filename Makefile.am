
MODULE_NAME = TemplateRepo
MODULE_DIR = @LOOLWSD_MODULE_DATA_DIR@/${MODULE_NAME}

include_paths = -I$(abs_top_builddir)/src/include \
		-I$(abs_top_builddir)/common \
		-I$(abs_top_builddir)/net \
		-I$(abs_top_builddir)/wsd

EXTRA_DIST = admin/*

install-data-hook:
	mkdir -p $(DESTDIR)$(MODULE_DIR)
	cp -aR admin $(DESTDIR)$(MODULE_DIR)/

uninstall-hook:
	rm -fr $(DESTDIR)$(MODULE_DIR)

configdir = @LOOLWSD_MODULE_CONFIG_DIR@
dist_config_DATA = TemplateRepo.xml

# Module install path
moduledir = @LOOLWSD_MODULE_DIR@
module_LTLIBRARIES = TemplateRepo.la

TemplateRepo_la_CPPFLAGS = -pthread ${include_paths}

TemplateRepo_la_LDFLAGS = -avoid-version -module -lPocoDataSQLite

TemplateRepo_la_SOURCES = src/TemplateRepo.cpp
