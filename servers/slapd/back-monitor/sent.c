/* sent.c - deal with data sent subsystem */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2001-2004 The OpenLDAP Foundation.
 * Portions Copyright 2001-2003 Pierangelo Masarati.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was initially developed by Pierangelo Masarati for inclusion
 * in OpenLDAP Software.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>

#include "slap.h"
#include "back-monitor.h"

enum {
	MONITOR_SENT_BYTES = 0,
	MONITOR_SENT_PDU,
	MONITOR_SENT_ENTRIES,
	MONITOR_SENT_REFERRALS,

	MONITOR_SENT_LAST
};

struct monitor_sent_t {
	struct berval	rdn;
	struct berval	nrdn;
} monitor_sent[] = {
	{ BER_BVC("cn=Bytes"),		BER_BVNULL },
	{ BER_BVC("cn=PDU"),		BER_BVNULL },
	{ BER_BVC("cn=Entries"),	BER_BVNULL },
	{ BER_BVC("cn=Referrals"),	BER_BVNULL },
	{ BER_BVNULL,			BER_BVNULL }
};

int
monitor_subsys_sent_init(
	BackendDB		*be
)
{
	struct monitorinfo	*mi;
	
	Entry			**ep, *e_sent;
	struct monitorentrypriv	*mp;
	int			i;

	assert( be != NULL );

	mi = ( struct monitorinfo * )be->be_private;

	if ( monitor_cache_get( mi,
			&monitor_subsys[SLAPD_MONITOR_SENT].mss_ndn, &e_sent ) ) {
		Debug( LDAP_DEBUG_ANY,
			"monitor_subsys_sent_init: "
			"unable to get entry \"%s\"\n",
			monitor_subsys[SLAPD_MONITOR_SENT].mss_ndn.bv_val, 0, 0 );
		return( -1 );
	}

	mp = ( struct monitorentrypriv * )e_sent->e_private;
	mp->mp_children = NULL;
	ep = &mp->mp_children;

	for ( i = 0; i < MONITOR_SENT_LAST; i++ ) {
		char			buf[ BACKMONITOR_BUFSIZE ];
		struct berval		rdn, bv;
		Entry			*e;

		snprintf( buf, sizeof( buf ),
				"dn: %s,%s\n"
				"objectClass: %s\n"
				"structuralObjectClass: %s\n"
				"cn: %s\n"
				"creatorsName: %s\n"
				"modifiersName: %s\n"
				"createTimestamp: %s\n"
				"modifyTimestamp: %s\n",
				monitor_sent[i].rdn.bv_val,
				monitor_subsys[SLAPD_MONITOR_SENT].mss_dn.bv_val,
				mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
				mi->mi_oc_monitorCounterObject->soc_cname.bv_val,
				&monitor_sent[i].rdn.bv_val[STRLENOF( "cn=" )],
				mi->mi_creatorsName.bv_val,
				mi->mi_creatorsName.bv_val,
				mi->mi_startTime.bv_val,
				mi->mi_startTime.bv_val );

		e = str2entry( buf );
		if ( e == NULL ) {
			Debug( LDAP_DEBUG_ANY,
				"monitor_subsys_sent_init: "
				"unable to create entry \"%s,%s\"\n",
				monitor_sent[i].rdn.bv_val,
				monitor_subsys[SLAPD_MONITOR_SENT].mss_ndn.bv_val, 0 );
			return( -1 );
		}

		/* steal normalized RDN */
		dnRdn( &e->e_nname, &rdn );
		ber_dupbv( &monitor_sent[i].nrdn, &rdn );
	
		BER_BVSTR( &bv, "0" );
		attr_merge_one( e, mi->mi_ad_monitorCounter, &bv, NULL );
	
		mp = ( struct monitorentrypriv * )ch_calloc( sizeof( struct monitorentrypriv ), 1 );
		e->e_private = ( void * )mp;
		mp->mp_next = NULL;
		mp->mp_children = NULL;
		mp->mp_info = &monitor_subsys[SLAPD_MONITOR_SENT];
		mp->mp_flags = monitor_subsys[SLAPD_MONITOR_SENT].mss_flags \
			| MONITOR_F_SUB | MONITOR_F_PERSISTENT;

		if ( monitor_cache_add( mi, e ) ) {
			Debug( LDAP_DEBUG_ANY,
				"monitor_subsys_sent_init: "
				"unable to add entry \"%s,%s\"\n",
				monitor_sent[i].rdn.bv_val,
				monitor_subsys[SLAPD_MONITOR_SENT].mss_ndn.bv_val, 0 );
			return( -1 );
		}
	
		*ep = e;
		ep = &mp->mp_next;
	}

	monitor_cache_release( mi, e_sent );

	return( 0 );
}

int
monitor_subsys_sent_update(
	Operation		*op,
	Entry                   *e
)
{
	struct monitorinfo	*mi = 
		(struct monitorinfo *)op->o_bd->be_private;
	
	struct berval		rdn;
#ifdef HAVE_GMP
	mpz_t			n;
#else /* ! HAVE_GMP */
	unsigned long 		n;
#endif /* ! HAVE_GMP */
	Attribute		*a;
	int			i;

	assert( mi );
	assert( e );

	dnRdn( &e->e_nname, &rdn );

	for ( i = 0; i < MONITOR_SENT_LAST; i++ ) {
		if ( dn_match( &rdn, &monitor_sent[i].nrdn ) ) {
			break;
		}
	}

	if ( i == MONITOR_SENT_LAST ) {
		return 0;
	}

	ldap_pvt_thread_mutex_lock(&slap_counters.sc_sent_mutex);
	switch ( i ) {
	case MONITOR_SENT_ENTRIES:
#ifdef HAVE_GMP
		mpz_init_set( n, slap_counters.sc_entries );
#else /* ! HAVE_GMP */
		n = slap_counters.sc_entries;
#endif /* ! HAVE_GMP */
		break;

	case MONITOR_SENT_REFERRALS:
#ifdef HAVE_GMP
		mpz_init_set( n, slap_counters.sc_refs );
#else /* ! HAVE_GMP */
		n = slap_counters.sc_refs;
#endif /* ! HAVE_GMP */
		break;

	case MONITOR_SENT_PDU:
#ifdef HAVE_GMP
		mpz_init_set( n, slap_counters.sc_pdu );
#else /* ! HAVE_GMP */
		n = slap_counters.sc_pdu;
#endif /* ! HAVE_GMP */
		break;

	case MONITOR_SENT_BYTES:
#ifdef HAVE_GMP
		mpz_init_set( n, slap_counters.sc_bytes );
#else /* ! HAVE_GMP */
		n = slap_counters.sc_bytes;
#endif /* ! HAVE_GMP */
		break;

	default:
		assert(0);
	}
	ldap_pvt_thread_mutex_unlock(&slap_counters.sc_sent_mutex);
	
	a = attr_find( e->e_attrs, mi->mi_ad_monitorCounter );
	assert( a );

	/* NOTE: no minus sign is allowed in the counters... */
	UI2BV( &a->a_vals[ 0 ], n );
#ifdef HAVE_GMP
	mpz_clear( n );
#endif /* HAVE_GMP */

	return 0;
}

