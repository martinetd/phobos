#!/bin/bash

export PGPASSWORD="phobos"
PSQL="psql -U phobos -h localhost phobos"

setup_db() {
	su postgres -c "createuser phobos"
	su postgres -c "createdb phobos"
	su postgres -c "psql phobos" << EOF
GRANT ALL ON DATABASE phobos TO phobos;
ALTER SCHEMA public OWNER TO phobos;
ALTER USER phobos WITH PASSWORD 'phobos';
CREATE EXTENSION IF NOT EXISTS btree_gin SCHEMA public;
EOF
	# You WILL need to configure your pg_hba.conf with either 'trust'
	# or 'md5' for the IP you connect from.
	# PSQL command is configured for password over TCP, if you would
	# like to use trust just remove -h localhost and use the socket.
}

drop_db() {
	su postgres -c "dropdb phobos"
	su postgres -c "dropuser phobos"
}

setup_tables() {
	$PSQL << EOF
CREATE TYPE dev_family AS ENUM ('disk', 'tape', 'dir');
CREATE TYPE dev_model AS ENUM ('ULTRIUM-TD6', 'ULTRIUM-TD5');
CREATE TYPE tape_model AS ENUM ('LTO6', 'LTO5', 'T10KB');
CREATE TYPE adm_status AS ENUM ('locked', 'unlocked', 'failed');
CREATE TYPE fs_type AS ENUM ('POSIX', 'LTFS');
CREATE TYPE address_type AS ENUM ('PATH', 'HASH1', 'OPAQUE');
CREATE TYPE fs_status AS ENUM ('blank', 'empty', 'used');

-- enums extensibles: ALTER TYPE type ADD VALUE 'value'

CREATE TABLE device(family dev_family, model dev_model,
                    id varchar(32) UNIQUE, host varchar(128),
	            adm_status adm_status, path varchar(256),
		    changer_idx int, PRIMARY KEY (family, id));
CREATE INDEX ON device USING gin(host);

CREATE TABLE media(family dev_family, model tape_model, id varchar(32) UNIQUE,
                   adm_status adm_status, fs_type fs_type,
                   address_type address_type, fs_status fs_status,
		   stats jsonb, PRIMARY KEY (family, id));
CREATE INDEX ON media((stats->>'phys_spc_free'));

CREATE TABLE object(oid varchar(1024), user_md jsonb, st_md json,
                    PRIMARY KEY (oid));

CREATE TABLE extent(oid varchar(1024), layout_idx int, media varchar(32),
                    address varchar(256), size bigint, PRIMARY KEY (oid));
CREATE INDEX ON extent(media);

-- that or put all layouts for a single object in a json array?
-- But not sure we can index/query on any elemetns of an array...


EOF

}

drop_tables() {
# Don't drop the schema, it drops the extension
        $PSQL << EOF
DROP TABLE IF EXISTS device, media, object, extent CASCADE;
DROP TYPE IF EXISTS dev_family, dev_model, tape_model, fs_status,
		    adm_status, fs_type, address_type CASCADE;
EOF
}

insert_examples() {
#  initially mounted tapes don't have enough room to store a big file
	$PSQL << EOF
insert into device (family, model, id, host, adm_status, path, changer_idx)
    values ('tape', 'ULTRIUM-TD6', '1013005381', 'phobos1',
	    'locked', '/dev/IBMtape1', 3),
           ('tape', 'ULTRIUM-TD6', '1014005381', 'phobos1',
	    'unlocked', '/dev/IBMtape0', 4),
           ('dir', NULL, 'phobos1:/tmp/pho_testdir1', 'phobos1',
	    'unlocked', '/tmp/pho_testdir1', NULL),
           ('dir', NULL, 'phobos1:/tmp/pho_testdir2', 'phobos1',
	    'unlocked', '/tmp/pho_testdir2', NULL);
insert into media (family, model, id, adm_status, fs_type, address_type,
		   fs_status, stats)
    values ('tape', 'LTO6', '073220L6', 'unlocked', 'LTFS', 'HASH1', 'blank',
            '{"nb_obj":"2","logc_spc_used":"6291456000",\
	      "phys_spc_used":"42469425152","phys_spc_free":"2048"}'),
           ('tape', 'LTO6', '073221L6', 'unlocked', 'LTFS', 'HASH1', 'blank',
            '{"nb_obj":"2","logc_spc_used":"15033434112",\
	      "phys_spc_used":"15033434112","phys_spc_free":"1024"}'),
           ('tape', 'LTO6', '073222L6', 'unlocked', 'LTFS', 'HASH1', 'blank',
            '{"nb_obj":"1","logc_spc_used":"10480512",\
	      "phys_spc_used":"10480512","phys_spc_free":"2393054904320"}'),
           ('dir', NULL, 'phobos1:/tmp/pho_testdir1', 'unlocked', 'POSIX',
	    'HASH1', 'blank', '{"nb_obj":"5","logc_spc_used":"3668841456",\
	      "phys_spc_used":"3668841456","phys_spc_free":"12857675776"}'),
           ('dir', NULL, 'phobos1:/tmp/pho_testdir2', 'unlocked', 'POSIX',
	    'HASH1', 'blank', '{"nb_obj":"6","logc_spc_used":"4868841472",\
	      "phys_spc_used":"4868841472","phys_spc_free":"12857675776"}');
EOF
}

select_example() {
	$PSQL << EOF
select host->>0 as firsthost from device; -- needs 9.3
select * from device where host ? 'foo1'; -- needs 9.4
EOF

}

# if we're being sourced, don't parse arguments
[[ $(caller | cut -d' ' -f1) != "0" ]] && return 0

usage() {
	echo "Usage: . $0"
	echo "  OR   $0 ACTION [ACTION [ACTION...]]"
	echo "where  ACTION := { setup_db | drop_db | setup_tables |"
	echo "                   drop_tables | insert_examples }"
	exit -1
}

if [[ $# -eq 0 ]]; then
	usage
fi

while [[ $# -gt 0 ]]; do
	case "$1" in
	setup_db)
		setup_db
		;;
	drop_db)
		drop_db
		;;
	setup_tables)
		setup_tables
		;;
	drop_tables)
		drop_tables
		;;
	insert_examples)
		insert_examples
		;;
	*)
		usage
		;;
	esac
	shift
done
