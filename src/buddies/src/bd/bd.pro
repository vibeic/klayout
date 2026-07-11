
DESTDIR = $$OUT_PWD/../../..
TARGET = klayout_bd

include($$PWD/../../../lib.pri)

DEFINES += MAKE_BD_LIBRARY

SOURCES = \
  bdInit.cc \
  bdReaderOptions.cc \
  bdWriterOptions.cc \
  bdConverterMain.cc \
  strm2cif.cc \
  strm2gds.cc \
  strm2oas.cc \
  strm2lstr.cc \
  strmclip.cc \
  strm2dxf.cc \
  strm2gdstxt.cc \
  strm2txt.cc \
  strmcmp.cc \
  strmxor.cc \
  strmrun.cc \
  strm2mag.cc \
  svrfdrc.cc \
  $$PWD/../../../plugins/tools/svrf_drc/db_plugin/dbSVRFDeck.cc \
  $$PWD/../../../plugins/tools/svrf_drc/db_plugin/dbSVRFEngine.cc

HEADERS = \
  bdCommon.h \
  bdInit.h \
  bdReaderOptions.h \
  bdWriterOptions.h \
  bdConverterMain.h \

RESOURCES = \

INCLUDEPATH += $$TL_INC $$GSI_INC $$VERSION_INC $$DB_INC $$LIB_INC $$RDB_INC $$PEX_INC $$LYM_INC
DEPENDPATH += $$TL_INC $$GSI_INC $$VERSION_INC $$DB_INC $$LIB_INC $$RDB_INC $$PEX_INC $$LYM_INC

#  native SVRF/Calibre DRC engine sources (compiled into klayout_bd for the svrfdrc buddy)
INCLUDEPATH += $$PWD/../../../plugins/tools/svrf_drc/db_plugin
DEPENDPATH += $$PWD/../../../plugins/tools/svrf_drc/db_plugin
LIBS += -L$$DESTDIR -lklayout_tl -lklayout_db -lklayout_gsi -lklayout_lib -lklayout_rdb -lklayout_pex -lklayout_lym

INCLUDEPATH += $$RBA_INC
DEPENDPATH += $$RBA_INC

equals(HAVE_RUBY, "1") {
  INCLUDEPATH += $$DRC_INC $$LVS_INC
  DEPENDPATH += $$DRC_INC $$LVS_INC
  LIBS += -lklayout_rba -lklayout_drc -lklayout_lvs
} else {
  LIBS += -lklayout_rbastub
}

INCLUDEPATH += $$PYA_INC
DEPENDPATH += $$PYA_INC

equals(HAVE_PYTHON, "1") {
  LIBS += -lklayout_pya
} else {
  LIBS += -lklayout_pyastub
}


