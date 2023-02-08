TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = cpp-template-utils thin_io thin_io_testapp

exists($${PWD}/../cpp-template-utils) {
	SUBREPOS_DIR=$${PWD}/..
} else {
	SUBREPOS_DIR=$${PWD}/../..
}

cpp-template-utils.subdir=$${SUBREPOS_DIR}/cpp-template-utils
thin_io.file=$${PWD}/../thin_io.pro

thin_io_testapp.depends=thin_io
