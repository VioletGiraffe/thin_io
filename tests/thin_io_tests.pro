TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = thin_io testapp

thin_io.file = $${PWD}/../thin_io.pro
testapp.subdir = $${PWD}/thin_io_testapp

testapp.depends = thin_io
