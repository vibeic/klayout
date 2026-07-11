
# The native SVRF DRC engine sources (db_plugin/dbSVRFDeck.cc, dbSVRFEngine.cc)
# are compiled into klayout_bd for the `svrfdrc` buddy (see buddies/src/bd/bd.pro).
# This directory therefore builds no module of its own -- this empty subdirs
# project only exists so the tools.pro auto-glob ($files) accepts the folder.
TEMPLATE = subdirs
SUBDIRS =
