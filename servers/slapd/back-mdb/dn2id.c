/* dn2id.c - routines to deal with the dn2id index */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2011 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "back-mdb.h"
#include "idl.h"
#include "lutil.h"

/* Management routines for a hierarchically structured database.
 *
 * Instead of a ldbm-style dn2id database, we use a hierarchical one. Each
 * entry in this database is a struct diskNode, keyed by entryID and with
 * the data containing the RDN and entryID of the node's children. We use
 * a B-Tree with sorted duplicates to store all the children of a node under
 * the same key. Also, the first item under the key contains the entry's own
 * rdn and the ID of the node's parent, to allow bottom-up tree traversal as
 * well as top-down. To keep this info first in the list, the high bit of all
 * subsequent nrdnlen's is always set. This means we can only accomodate
 * RDNs up to length 32767, but that's fine since full DNs are already
 * restricted to 8192.
 *
 * The diskNode is a variable length structure. This definition is not
 * directly usable for in-memory manipulation.
 */
typedef struct diskNode {
	unsigned char nrdnlen[2];
	char nrdn[1];
	char rdn[1];                        /* variable placement */
	unsigned char entryID[sizeof(ID)];  /* variable placement */
} diskNode;

/* Sort function for the sorted duplicate data items of a dn2id key.
 * Sorts based on normalized RDN, in length order.
 */
int
mdb_dup_compare(
	DB *db, 
	const DBT *usrkey,
	const DBT *curkey
)
{
	diskNode *un, *cn;
	int rc;

	un = (diskNode *)usrkey->data;
	cn = (diskNode *)curkey->data;

	/* data is not aligned, cannot compare directly */
	rc = un->nrdnlen[0] - cn->nrdnlen[0];
	if ( rc ) return rc;
	rc = un->nrdnlen[1] - cn->nrdnlen[1];
	if ( rc ) return rc;

	return strcmp( un->nrdn, cn->nrdn );
}

/* This function constructs a full DN for a given entry.
 */
int mdb_fix_dn(
	Entry *e,
	int checkit )
{
	EntryInfo *ei;
	int rlen = 0, nrlen = 0;
	char *ptr, *nptr;
	int max = 0;

	if ( !e->e_id )
		return 0;

	/* count length of all DN components */
	for ( ei = BEI(e); ei && ei->bei_id; ei=ei->bei_parent ) {
		rlen += ei->bei_rdn.bv_len + 1;
		nrlen += ei->bei_nrdn.bv_len + 1;
		if (ei->bei_modrdns > max) max = ei->bei_modrdns;
	}

	/* See if the entry DN was invalidated by a subtree rename */
	if ( checkit ) {
		if ( BEI(e)->bei_modrdns >= max ) {
			return 0;
		}
		/* We found a mismatch, tell the caller to lock it */
		if ( checkit == 1 ) {
			return 1;
		}
		/* checkit == 2. do the fix. */
		free( e->e_name.bv_val );
		free( e->e_nname.bv_val );
	}

	e->e_name.bv_len = rlen - 1;
	e->e_nname.bv_len = nrlen - 1;
	e->e_name.bv_val = ch_malloc(rlen);
	e->e_nname.bv_val = ch_malloc(nrlen);
	ptr = e->e_name.bv_val;
	nptr = e->e_nname.bv_val;
	for ( ei = BEI(e); ei && ei->bei_id; ei=ei->bei_parent ) {
		ptr = lutil_strcopy(ptr, ei->bei_rdn.bv_val);
		nptr = lutil_strcopy(nptr, ei->bei_nrdn.bv_val);
		if ( ei->bei_parent ) {
			*ptr++ = ',';
			*nptr++ = ',';
		}
	}
	BEI(e)->bei_modrdns = max;
	if ( ptr > e->e_name.bv_val ) ptr[-1] = '\0';
	if ( nptr > e->e_nname.bv_val ) nptr[-1] = '\0';

	return 0;
}

/* We add two elements to the DN2ID database - a data item under the parent's
 * entryID containing the child's RDN and entryID, and an item under the
 * child's entryID containing the parent's entryID.
 */
int
mdb_dn2id_add(
	Operation	*op,
	DB_TXN *txn,
	EntryInfo	*eip,
	Entry		*e )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	DB *db = mdb->bi_dn2id->bdi_db;
	DBT		key, data;
	ID		nid;
	int		rc, rlen, nrlen;
	diskNode *d;
	char *ptr;

	Debug( LDAP_DEBUG_TRACE, "=> mdb_dn2id_add 0x%lx: \"%s\"\n",
		e->e_id, e->e_ndn, 0 );

	nrlen = dn_rdnlen( op->o_bd, &e->e_nname );
	if (nrlen) {
		rlen = dn_rdnlen( op->o_bd, &e->e_name );
	} else {
		nrlen = e->e_nname.bv_len;
		rlen = e->e_name.bv_len;
	}

	d = op->o_tmpalloc(sizeof(diskNode) + rlen + nrlen, op->o_tmpmemctx);
	d->nrdnlen[1] = nrlen & 0xff;
	d->nrdnlen[0] = (nrlen >> 8) | 0x80;
	ptr = lutil_strncopy( d->nrdn, e->e_nname.bv_val, nrlen );
	*ptr++ = '\0';
	ptr = lutil_strncopy( ptr, e->e_name.bv_val, rlen );
	*ptr++ = '\0';
	MDB_ID2DISK( e->e_id, ptr );

	DBTzero(&key);
	DBTzero(&data);
	key.size = sizeof(ID);
	key.flags = DB_DBT_USERMEM;
	MDB_ID2DISK( eip->bei_id, &nid );

	key.data = &nid;

	/* Need to make dummy root node once. Subsequent attempts
	 * will fail harmlessly.
	 */
	if ( eip->bei_id == 0 ) {
		diskNode dummy = {{0, 0}, "", "", ""};
		data.data = &dummy;
		data.size = sizeof(diskNode);
		data.flags = DB_DBT_USERMEM;

		db->put( db, txn, &key, &data, DB_NODUPDATA );
	}

	data.data = d;
	data.size = sizeof(diskNode) + rlen + nrlen;
	data.flags = DB_DBT_USERMEM;

	rc = db->put( db, txn, &key, &data, DB_NODUPDATA );

	if (rc == 0) {
		MDB_ID2DISK( e->e_id, &nid );
		MDB_ID2DISK( eip->bei_id, ptr );
		d->nrdnlen[0] ^= 0x80;

		rc = db->put( db, txn, &key, &data, DB_NODUPDATA );
	}

	/* Update all parents' IDL cache entries */
	if ( rc == 0 && mdb->bi_idl_cache_size ) {
		ID tmp[2];
		char *ptr = ((char *)&tmp[1])-1;
		key.data = ptr;
		key.size = sizeof(ID)+1;
		tmp[1] = eip->bei_id;
		*ptr = DN_ONE_PREFIX;
		mdb_idl_cache_add_id( mdb, db, &key, e->e_id );
		if ( eip->bei_parent ) {
			*ptr = DN_SUBTREE_PREFIX;
			for (; eip && eip->bei_parent->bei_id; eip = eip->bei_parent) {
				tmp[1] = eip->bei_id;
				mdb_idl_cache_add_id( mdb, db, &key, e->e_id );
			}
			/* Handle DB with empty suffix */
			if ( !op->o_bd->be_suffix[0].bv_len && eip ) {
				tmp[1] = eip->bei_id;
				mdb_idl_cache_add_id( mdb, db, &key, e->e_id );
			}
		}
	}

	op->o_tmpfree( d, op->o_tmpmemctx );
	Debug( LDAP_DEBUG_TRACE, "<= mdb_dn2id_add 0x%lx: %d\n", e->e_id, rc, 0 );

	return rc;
}

int
mdb_dn2id_delete(
	Operation	*op,
	DB_TXN *txn,
	EntryInfo	*eip,
	Entry	*e )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	DB *db = mdb->bi_dn2id->bdi_db;
	DBT		key, data;
	DBC	*cursor;
	diskNode *d;
	int rc;
	ID	nid;
	unsigned char dlen[2];

	Debug( LDAP_DEBUG_TRACE, "=> mdb_dn2id_delete 0x%lx: \"%s\"\n",
		e->e_id, e->e_ndn, 0 );

	DBTzero(&key);
	key.size = sizeof(ID);
	key.ulen = key.size;
	key.flags = DB_DBT_USERMEM;
	MDB_ID2DISK( eip->bei_id, &nid );

	DBTzero(&data);
	data.size = sizeof(diskNode) + BEI(e)->bei_nrdn.bv_len - sizeof(ID) - 1;
	data.ulen = data.size;
	data.dlen = data.size;
	data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;

	key.data = &nid;

	d = op->o_tmpalloc( data.size, op->o_tmpmemctx );
	d->nrdnlen[1] = BEI(e)->bei_nrdn.bv_len & 0xff;
	d->nrdnlen[0] = (BEI(e)->bei_nrdn.bv_len >> 8) | 0x80;
	dlen[0] = d->nrdnlen[0];
	dlen[1] = d->nrdnlen[1];
	memcpy( d->nrdn, BEI(e)->bei_nrdn.bv_val, BEI(e)->bei_nrdn.bv_len+1 );
	data.data = d;

	rc = db->cursor( db, txn, &cursor, mdb->bi_db_opflags );
	if ( rc ) goto func_leave;

	/* Delete our ID from the parent's list */
	rc = cursor->c_get( cursor, &key, &data, DB_GET_BOTH_RANGE );
	if ( rc == 0 ) {
		if ( dlen[1] == d->nrdnlen[1] && dlen[0] == d->nrdnlen[0] &&
			!strcmp( d->nrdn, BEI(e)->bei_nrdn.bv_val ))
			rc = cursor->c_del( cursor, 0 );
		else
			rc = DB_NOTFOUND;
	}

	/* Delete our ID from the tree. With sorted duplicates, this
	 * will leave any child nodes still hanging around. This is OK
	 * for modrdn, which will add our info back in later.
	 */
	if ( rc == 0 ) {
		MDB_ID2DISK( e->e_id, &nid );
		rc = cursor->c_get( cursor, &key, &data, DB_SET );
		if ( rc == 0 )
			rc = cursor->c_del( cursor, 0 );
	}

	cursor->c_close( cursor );
func_leave:
	op->o_tmpfree( d, op->o_tmpmemctx );

	/* Delete IDL cache entries */
	if ( rc == 0 && mdb->bi_idl_cache_size ) {
		ID tmp[2];
		char *ptr = ((char *)&tmp[1])-1;
		key.data = ptr;
		key.size = sizeof(ID)+1;
		tmp[1] = eip->bei_id;
		*ptr = DN_ONE_PREFIX;
		mdb_idl_cache_del_id( mdb, db, &key, e->e_id );
		if ( eip ->bei_parent ) {
			*ptr = DN_SUBTREE_PREFIX;
			for (; eip && eip->bei_parent->bei_id; eip = eip->bei_parent) {
				tmp[1] = eip->bei_id;
				mdb_idl_cache_del_id( mdb, db, &key, e->e_id );
			}
			/* Handle DB with empty suffix */
			if ( !op->o_bd->be_suffix[0].bv_len && eip ) {
				tmp[1] = eip->bei_id;
				mdb_idl_cache_del_id( mdb, db, &key, e->e_id );
			}
		}
	}
	Debug( LDAP_DEBUG_TRACE, "<= mdb_dn2id_delete 0x%lx: %d\n", e->e_id, rc, 0 );
	return rc;
}


int
mdb_dn2id(
	Operation	*op,
	struct berval	*in,
	EntryInfo	*ei,
	DB_TXN *txn,
	DBC **cursor )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	DB *db = mdb->bi_dn2id->bdi_db;
	DBT		key, data;
	int		rc = 0, nrlen;
	diskNode *d;
	char	*ptr;
	unsigned char dlen[2];
	ID idp, parentID;

	Debug( LDAP_DEBUG_TRACE, "=> mdb_dn2id(\"%s\")\n", in->bv_val, 0, 0 );

	nrlen = dn_rdnlen( op->o_bd, in );
	if (!nrlen) nrlen = in->bv_len;

	DBTzero(&key);
	key.size = sizeof(ID);
	key.data = &idp;
	key.ulen = sizeof(ID);
	key.flags = DB_DBT_USERMEM;
	parentID = ( ei->bei_parent != NULL ) ? ei->bei_parent->bei_id : 0;
	MDB_ID2DISK( parentID, &idp );

	DBTzero(&data);
	data.size = sizeof(diskNode) + nrlen - sizeof(ID) - 1;
	data.ulen = data.size * 3;
	data.dlen = data.ulen;
	data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;

	rc = db->cursor( db, txn, cursor, mdb->bi_db_opflags );
	if ( rc ) return rc;

	d = op->o_tmpalloc( data.size * 3, op->o_tmpmemctx );
	d->nrdnlen[1] = nrlen & 0xff;
	d->nrdnlen[0] = (nrlen >> 8) | 0x80;
	dlen[0] = d->nrdnlen[0];
	dlen[1] = d->nrdnlen[1];
	ptr = lutil_strncopy( d->nrdn, in->bv_val, nrlen );
	*ptr = '\0';
	data.data = d;

	rc = (*cursor)->c_get( *cursor, &key, &data, DB_GET_BOTH_RANGE );
	if ( rc == 0 && (dlen[1] != d->nrdnlen[1] || dlen[0] != d->nrdnlen[0] ||
		strncmp( d->nrdn, in->bv_val, nrlen ))) {
		rc = DB_NOTFOUND;
	}
	if ( rc == 0 ) {
		ptr = (char *) data.data + data.size - sizeof(ID);
		MDB_DISK2ID( ptr, &ei->bei_id );
		ei->bei_rdn.bv_len = data.size - sizeof(diskNode) - nrlen;
		ptr = d->nrdn + nrlen + 1;
		ber_str2bv( ptr, ei->bei_rdn.bv_len, 1, &ei->bei_rdn );
		if ( ei->bei_parent != NULL && !ei->bei_parent->bei_dkids ) {
			db_recno_t dkids;
			/* How many children does the parent have? */
			/* FIXME: do we need to lock the parent
			 * entryinfo? Seems safe...
			 */
			(*cursor)->c_count( *cursor, &dkids, 0 );
			ei->bei_parent->bei_dkids = dkids;
		}
	}

	op->o_tmpfree( d, op->o_tmpmemctx );
	if( rc != 0 ) {
		Debug( LDAP_DEBUG_TRACE, "<= mdb_dn2id: get failed: %s (%d)\n",
			db_strerror( rc ), rc, 0 );
	} else {
		Debug( LDAP_DEBUG_TRACE, "<= mdb_dn2id: got id=0x%lx\n",
			ei->bei_id, 0, 0 );
	}

	return rc;
}

int
mdb_dn2id_parent(
	Operation *op,
	DB_TXN *txn,
	EntryInfo *ei,
	ID *idp )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	DB *db = mdb->bi_dn2id->bdi_db;
	DBT		key, data;
	DBC	*cursor;
	int		rc = 0;
	diskNode *d;
	char	*ptr;
	ID	nid;

	DBTzero(&key);
	key.size = sizeof(ID);
	key.data = &nid;
	key.ulen = sizeof(ID);
	key.flags = DB_DBT_USERMEM;
	MDB_ID2DISK( ei->bei_id, &nid );

	DBTzero(&data);
	data.flags = DB_DBT_USERMEM;

	rc = db->cursor( db, txn, &cursor, mdb->bi_db_opflags );
	if ( rc ) return rc;

	data.ulen = sizeof(diskNode) + (SLAP_LDAPDN_MAXLEN * 2);
	d = op->o_tmpalloc( data.ulen, op->o_tmpmemctx );
	data.data = d;

	rc = cursor->c_get( cursor, &key, &data, DB_SET );
	if ( rc == 0 ) {
		if (d->nrdnlen[0] & 0x80) {
			rc = LDAP_OTHER;
		} else {
			db_recno_t dkids;
			ptr = (char *) data.data + data.size - sizeof(ID);
			MDB_DISK2ID( ptr, idp );
			ei->bei_nrdn.bv_len = (d->nrdnlen[0] << 8) | d->nrdnlen[1];
			ber_str2bv( d->nrdn, ei->bei_nrdn.bv_len, 1, &ei->bei_nrdn );
			ei->bei_rdn.bv_len = data.size - sizeof(diskNode) -
				ei->bei_nrdn.bv_len;
			ptr = d->nrdn + ei->bei_nrdn.bv_len + 1;
			ber_str2bv( ptr, ei->bei_rdn.bv_len, 1, &ei->bei_rdn );
			/* How many children does this node have? */
			cursor->c_count( cursor, &dkids, 0 );
			ei->bei_dkids = dkids;
		}
	}
	cursor->c_close( cursor );
	op->o_tmpfree( d, op->o_tmpmemctx );
	return rc;
}

int
mdb_dn2id_children(
	Operation *op,
	DB_TXN *txn,
	Entry *e )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	DB *db = mdb->bi_dn2id->bdi_db;
	DBT		key, data;
	DBC		*cursor;
	int		rc;
	ID		id;
	diskNode d;

	DBTzero(&key);
	key.size = sizeof(ID);
	key.data = &e->e_id;
	key.flags = DB_DBT_USERMEM;
	MDB_ID2DISK( e->e_id, &id );

	/* IDL cache is in host byte order */
	if ( mdb->bi_idl_cache_size ) {
		rc = mdb_idl_cache_get( mdb, db, &key, NULL );
		if ( rc != LDAP_NO_SUCH_OBJECT ) {
			return rc;
		}
	}

	key.data = &id;
	DBTzero(&data);
	data.data = &d;
	data.ulen = sizeof(d);
	data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;
	data.dlen = sizeof(d);

	rc = db->cursor( db, txn, &cursor, mdb->bi_db_opflags );
	if ( rc ) return rc;

	rc = cursor->c_get( cursor, &key, &data, DB_SET );
	if ( rc == 0 ) {
		db_recno_t dkids;
		rc = cursor->c_count( cursor, &dkids, 0 );
		if ( rc == 0 ) {
			BEI(e)->bei_dkids = dkids;
			if ( dkids < 2 ) rc = DB_NOTFOUND;
		}
	}
	cursor->c_close( cursor );
	return rc;
}

/* mdb_dn2idl:
 * We can't just use mdb_idl_fetch_key because
 * 1 - our data items are longer than just an entry ID
 * 2 - our data items are sorted alphabetically by nrdn, not by ID.
 *
 * We descend the tree recursively, so we define this cookie
 * to hold our necessary state information. The mdb_dn2idl_internal
 * function uses this cookie when calling itself.
 */

struct dn2id_cookie {
	struct mdb_info *mdb;
	Operation *op;
	DB_TXN *txn;
	EntryInfo *ei;
	ID *ids;
	ID *tmp;
	ID *buf;
	DB *db;
	DBC *dbc;
	DBT key;
	DBT data;
	ID dbuf;
	ID id;
	ID nid;
	int rc;
	int depth;
	char need_sort;
	char prefix;
};

static int
apply_func(
	void *data,
	void *arg )
{
	EntryInfo *ei = data;
	ID *idl = arg;

	mdb_idl_append_one( idl, ei->bei_id );
	return 0;
}

static int
mdb_dn2idl_internal(
	struct dn2id_cookie *cx
)
{
	MDB_IDL_ZERO( cx->tmp );

	if ( cx->mdb->bi_idl_cache_size ) {
		char *ptr = ((char *)&cx->id)-1;

		cx->key.data = ptr;
		cx->key.size = sizeof(ID)+1;
		if ( cx->prefix == DN_SUBTREE_PREFIX ) {
			ID *ids = cx->depth ? cx->tmp : cx->ids;
			*ptr = cx->prefix;
			cx->rc = mdb_idl_cache_get(cx->mdb, cx->db, &cx->key, ids);
			if ( cx->rc == LDAP_SUCCESS ) {
				if ( cx->depth ) {
					mdb_idl_append( cx->ids, cx->tmp );
					cx->need_sort = 1;
				}
				return cx->rc;
			}
		}
		*ptr = DN_ONE_PREFIX;
		cx->rc = mdb_idl_cache_get(cx->mdb, cx->db, &cx->key, cx->tmp);
		if ( cx->rc == LDAP_SUCCESS ) {
			goto gotit;
		}
		if ( cx->rc == DB_NOTFOUND ) {
			return cx->rc;
		}
	}

	mdb_cache_entryinfo_lock( cx->ei );

	/* If number of kids in the cache differs from on-disk, load
	 * up all the kids from the database
	 */
	if ( cx->ei->bei_ckids+1 != cx->ei->bei_dkids ) {
		EntryInfo ei;
		db_recno_t dkids = cx->ei->bei_dkids;
		ei.bei_parent = cx->ei;

		/* Only one thread should load the cache */
		while ( cx->ei->bei_state & CACHE_ENTRY_ONELEVEL ) {
			mdb_cache_entryinfo_unlock( cx->ei );
			ldap_pvt_thread_yield();
			mdb_cache_entryinfo_lock( cx->ei );
			if ( cx->ei->bei_ckids+1 == cx->ei->bei_dkids ) {
				goto synced;
			}
		}

		cx->ei->bei_state |= CACHE_ENTRY_ONELEVEL;

		mdb_cache_entryinfo_unlock( cx->ei );

		cx->rc = cx->db->cursor( cx->db, NULL, &cx->dbc,
			cx->mdb->bi_db_opflags );
		if ( cx->rc )
			goto done_one;

		cx->data.data = &cx->dbuf;
		cx->data.ulen = sizeof(ID);
		cx->data.dlen = sizeof(ID);
		cx->data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;

		/* The first item holds the parent ID. Ignore it. */
		cx->key.data = &cx->nid;
		cx->key.size = sizeof(ID);
		cx->rc = cx->dbc->c_get( cx->dbc, &cx->key, &cx->data, DB_SET );
		if ( cx->rc ) {
			cx->dbc->c_close( cx->dbc );
			goto done_one;
		}

		/* If the on-disk count is zero we've never checked it.
		 * Count it now.
		 */
		if ( !dkids ) {
			cx->dbc->c_count( cx->dbc, &dkids, 0 );
			cx->ei->bei_dkids = dkids;
		}

		cx->data.data = cx->buf;
		cx->data.ulen = MDB_IDL_UM_SIZE * sizeof(ID);
		cx->data.flags = DB_DBT_USERMEM;

		if ( dkids > 1 ) {
			/* Fetch the rest of the IDs in a loop... */
			while ( (cx->rc = cx->dbc->c_get( cx->dbc, &cx->key, &cx->data,
				DB_MULTIPLE | DB_NEXT_DUP )) == 0 ) {
				u_int8_t *j;
				size_t len;
				void *ptr;
				DB_MULTIPLE_INIT( ptr, &cx->data );
				while (ptr) {
					DB_MULTIPLE_NEXT( ptr, &cx->data, j, len );
					if (j) {
						EntryInfo *ei2;
						diskNode *d = (diskNode *)j;
						short nrlen;

						MDB_DISK2ID( j + len - sizeof(ID), &ei.bei_id );
						nrlen = ((d->nrdnlen[0] ^ 0x80) << 8) | d->nrdnlen[1];
						ei.bei_nrdn.bv_len = nrlen;
						/* nrdn/rdn are set in-place.
						 * mdb_cache_load will copy them as needed
						 */
						ei.bei_nrdn.bv_val = d->nrdn;
						ei.bei_rdn.bv_len = len - sizeof(diskNode)
							- ei.bei_nrdn.bv_len;
						ei.bei_rdn.bv_val = d->nrdn + ei.bei_nrdn.bv_len + 1;
						mdb_idl_append_one( cx->tmp, ei.bei_id );
						mdb_cache_load( cx->mdb, &ei, &ei2 );
					}
				}
			}
		}

		cx->rc = cx->dbc->c_close( cx->dbc );
done_one:
		mdb_cache_entryinfo_lock( cx->ei );
		cx->ei->bei_state &= ~CACHE_ENTRY_ONELEVEL;
		mdb_cache_entryinfo_unlock( cx->ei );
		if ( cx->rc )
			return cx->rc;

	} else {
		/* The in-memory cache is in sync with the on-disk data.
		 * do we have any kids?
		 */
synced:
		cx->rc = 0;
		if ( cx->ei->bei_ckids > 0 ) {
			/* Walk the kids tree; order is irrelevant since mdb_idl_sort
			 * will sort it later.
			 */
			avl_apply( cx->ei->bei_kids, apply_func,
				cx->tmp, -1, AVL_POSTORDER );
		}
		mdb_cache_entryinfo_unlock( cx->ei );
	}

	if ( !MDB_IDL_IS_RANGE( cx->tmp ) && cx->tmp[0] > 3 )
		mdb_idl_sort( cx->tmp, cx->buf );
	if ( cx->mdb->bi_idl_cache_max_size && !MDB_IDL_IS_ZERO( cx->tmp )) {
		char *ptr = ((char *)&cx->id)-1;
		cx->key.data = ptr;
		cx->key.size = sizeof(ID)+1;
		*ptr = DN_ONE_PREFIX;
		mdb_idl_cache_put( cx->mdb, cx->db, &cx->key, cx->tmp, cx->rc );
	}

gotit:
	if ( !MDB_IDL_IS_ZERO( cx->tmp )) {
		if ( cx->prefix == DN_SUBTREE_PREFIX ) {
			mdb_idl_append( cx->ids, cx->tmp );
			cx->need_sort = 1;
			if ( !(cx->ei->bei_state & CACHE_ENTRY_NO_GRANDKIDS)) {
				ID *save, idcurs;
				EntryInfo *ei = cx->ei;
				int nokids = 1;
				save = cx->op->o_tmpalloc( MDB_IDL_SIZEOF( cx->tmp ),
					cx->op->o_tmpmemctx );
				MDB_IDL_CPY( save, cx->tmp );

				idcurs = 0;
				cx->depth++;
				for ( cx->id = mdb_idl_first( save, &idcurs );
					cx->id != NOID;
					cx->id = mdb_idl_next( save, &idcurs )) {
					EntryInfo *ei2;
					cx->ei = NULL;
					if ( mdb_cache_find_id( cx->op, cx->txn, cx->id, &cx->ei,
						ID_NOENTRY, NULL ))
						continue;
					if ( cx->ei ) {
						ei2 = cx->ei;
						if ( !( ei2->bei_state & CACHE_ENTRY_NO_KIDS )) {
							MDB_ID2DISK( cx->id, &cx->nid );
							mdb_dn2idl_internal( cx );
							if ( !MDB_IDL_IS_ZERO( cx->tmp ))
								nokids = 0;
						}
						mdb_cache_entryinfo_lock( ei2 );
						ei2->bei_finders--;
						mdb_cache_entryinfo_unlock( ei2 );
					}
				}
				cx->depth--;
				cx->op->o_tmpfree( save, cx->op->o_tmpmemctx );
				if ( nokids ) {
					mdb_cache_entryinfo_lock( ei );
					ei->bei_state |= CACHE_ENTRY_NO_GRANDKIDS;
					mdb_cache_entryinfo_unlock( ei );
				}
			}
			/* Make sure caller knows it had kids! */
			cx->tmp[0]=1;

			cx->rc = 0;
		} else {
			MDB_IDL_CPY( cx->ids, cx->tmp );
		}
	}
	return cx->rc;
}

int
mdb_dn2idl(
	Operation	*op,
	DB_TXN *txn,
	struct berval *ndn,
	EntryInfo	*ei,
	ID *ids,
	ID *stack )
{
	struct mdb_info *mdb = (struct mdb_info *)op->o_bd->be_private;
	struct dn2id_cookie cx;

	Debug( LDAP_DEBUG_TRACE, "=> mdb_dn2idl(\"%s\")\n",
		ndn->bv_val, 0, 0 );

#ifndef MDB_MULTIPLE_SUFFIXES
	if ( op->ors_scope != LDAP_SCOPE_ONELEVEL && 
		( ei->bei_id == 0 ||
		( ei->bei_parent->bei_id == 0 && op->o_bd->be_suffix[0].bv_len )))
	{
		MDB_IDL_ALL( mdb, ids );
		return 0;
	}
#endif

	cx.id = ei->bei_id;
	MDB_ID2DISK( cx.id, &cx.nid );
	cx.ei = ei;
	cx.mdb = mdb;
	cx.db = cx.mdb->bi_dn2id->bdi_db;
	cx.prefix = (op->ors_scope == LDAP_SCOPE_ONELEVEL) ?
		DN_ONE_PREFIX : DN_SUBTREE_PREFIX;
	cx.ids = ids;
	cx.tmp = stack;
	cx.buf = stack + MDB_IDL_UM_SIZE;
	cx.op = op;
	cx.txn = txn;
	cx.need_sort = 0;
	cx.depth = 0;

	if ( cx.prefix == DN_SUBTREE_PREFIX ) {
		ids[0] = 1;
		ids[1] = cx.id;
	} else {
		MDB_IDL_ZERO( ids );
	}
	if ( cx.ei->bei_state & CACHE_ENTRY_NO_KIDS )
		return LDAP_SUCCESS;

	DBTzero(&cx.key);
	cx.key.ulen = sizeof(ID);
	cx.key.size = sizeof(ID);
	cx.key.flags = DB_DBT_USERMEM;

	DBTzero(&cx.data);

	mdb_dn2idl_internal(&cx);
	if ( cx.need_sort ) {
		char *ptr = ((char *)&cx.id)-1;
		if ( !MDB_IDL_IS_RANGE( cx.ids ) && cx.ids[0] > 3 ) 
			mdb_idl_sort( cx.ids, cx.tmp );
		cx.key.data = ptr;
		cx.key.size = sizeof(ID)+1;
		*ptr = cx.prefix;
		cx.id = ei->bei_id;
		if ( cx.mdb->bi_idl_cache_max_size )
			mdb_idl_cache_put( cx.mdb, cx.db, &cx.key, cx.ids, cx.rc );
	}

	if ( cx.rc == DB_NOTFOUND )
		cx.rc = LDAP_SUCCESS;

	return cx.rc;
}