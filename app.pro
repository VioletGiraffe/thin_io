TEMPLATE = subdirs

SUBDIRS += app cpputils cpp-template-utils

app.depends = cpputils cpp-template-utils
