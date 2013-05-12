#! /usr/bin/env atf-sh

atf_test_case search
search_head() {
	atf_set "descr" "testing pkg search"
        atf_set "require.files" \
           "$(atf_get_srcdir)/png-1.5.14.yaml $(atf_get_srcdir)/repo-2005/repo.txz"
}

search_body() {
        export PKG_DBDIR=$HOME/pkg
	export INSTALL_AS_USER=yes

        mkdir -p $PKG_DBDIR || atf_fail "can't create $PKG_DBDIR"

        atf_check \
            -o match:"^Installing png-1.5.14\.\.\." \
            -e empty \
            -s exit:0 \
            pkg register -t -M $(atf_get_srcdir)/png-1.5.14.yaml

        [ -f "$PKG_DBDIR/local.sqlite" ] || \
            atf_fail "Can't populate $PKG_DBDIR/local.sqlite"

	export PKG_CONF=$HOME/pkg.conf

	# '2005' is the value of REPO_SCHEMA_VERSION at the time of
	# writing.  As REPO_SCHEMA_VERSION gets updated, add new repo
	# directories labelled with the new version so we can test 
	# for binary compatibility...  See libpkg/pkgdb_repo.c

	echo "PACKAGESITE : file:$(atf_get_srcdir)/repo-2005" > $PKG_CONF

	atf_check \
	    -e empty \
	    -s exit:0 \
	    pkg -C $PKG_CONF update

	REPOS_DIR=/nonexistent
	atf_check \
	    -o inline:"pkg                            New generation package manager\n" \
	    -e empty \
	    -s exit:0 \
	    pkg -C $PKG_CONF search -e -Q comment -S name pkg
}

atf_init_test_cases() {
        . $(atf_get_srcdir)/test_environment

	atf_add_test_case search
}
