#
# This qmake file generates a header with the GCS version info string.
#
# This is a bit hacky but QMake seems to have some kind of bug that prevents this working
# with a target instead (the deps always end up with a relative path and the rule an absolute path or vice-versa, they don't match up)
#

ROOT_DIR              = $$GCS_SOURCE_TREE/../..
VERSION_INFO_PATH     = $$system_path($$GCS_BUILD_TREE)
VERSION_INFO_HEADER   = $$system_path($$VERSION_INFO_PATH/gcsversioninfo.h)
VERSION_INFO_SCRIPT   = $$ROOT_DIR/make/scripts/version-info.py
VERSION_INFO_TEMPLATE = $$ROOT_DIR/make/templates/gcsversioninfotemplate.h
VERSION_INFO_COMMAND  = python3 \"$$VERSION_INFO_SCRIPT\"
UAVO_DEF_PATH         = $$ROOT_DIR/shared/uavobjectdefinition
AUTHORS_TEMPLATE      = $$ROOT_DIR/make/templates/gcsauthorstemplate.html
REL_AUTHORS_TEMPLATE  = $$ROOT_DIR/make/templates/gcsrelauthorstemplate.html
AUTHORS_PATH          = $$system_path($$targetPath($$GCS_DATA_PATH))
AUTHORS_DEST          = $$system_path($$AUTHORS_PATH/gcsauthors.html)
REL_AUTHORS_DEST      = $$system_path($$AUTHORS_PATH/gcsrelauthors.html)
VERSION_METADATA_DEST = $$system_path($$AUTHORS_PATH/gcs-version-metadata.json)
VERSION_METADATA_SOURCE = $$ROOT_DIR/make/gcs-version-metadata.json
# A packaged source tree has these inputs instead of Git history. A checkout
# always uses its own current revision and contributors.
!exists($$ROOT_DIR/.git):exists($$VERSION_METADATA_SOURCE) {
    VERSION_METADATA_ARGS = --metadata=\"$$system_path($$VERSION_METADATA_SOURCE)\"
}

# these run once while Qmake is reading pro files rather than later during the actual build
!build_pass {
    system($$QMAKE_MKDIR \"$$AUTHORS_PATH\")
    !system($$VERSION_INFO_COMMAND \
                            --path=\"$$GCS_SOURCE_TREE\" \
                            --uavodir=\"$$UAVO_DEF_PATH\" \
                            $$VERSION_METADATA_ARGS \
                            --write-metadata=\"$$VERSION_METADATA_DEST\" \
                            --template=\"$$VERSION_INFO_TEMPLATE\" \
                            --outfile=\"$$VERSION_INFO_HEADER\" \
                            --template=\"$$REL_AUTHORS_TEMPLATE\" \
                            --outfile=\"$$REL_AUTHORS_DEST\" \
                            --template=\"$$AUTHORS_TEMPLATE\" \
                            --outfile=\"$$AUTHORS_DEST\"):error("Could not generate GCS version metadata")
}

DEPENDPATH *= $$VERSION_INFO_PATH
HEADERS *= $$VERSION_INFO_HEADER
DEFINES += 'GCS_VERSION_INFO_FILE=\\\"$$VERSION_INFO_HEADER\\\"'
