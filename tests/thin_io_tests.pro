TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = thin_io_testapp cpp-template-utils thin_io

exists($${PWD}/../cpp-template-utils) {
	SUBREPOS_DIR=$${PWD}/..
} else {
	SUBREPOS_DIR=$${PWD}/../..
}

cpp-template-utils.subdir=$${SUBREPOS_DIR}/cpp-template-utils
thin_io.file=$${PWD}/../thin_io.pro

thin_io_testapp.depends=thin_io
