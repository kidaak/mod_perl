/* Copyright 2000-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mod_perl.h"

int modperl_require_module(pTHX_ const char *pv, int logfailure)
{
    SV *sv;

    dSP;
    PUSHSTACKi(PERLSI_REQUIRE);
    PUTBACK;
    sv = sv_newmortal();
    sv_setpv(sv, "require ");
    sv_catpv(sv, pv);
    eval_sv(sv, G_DISCARD);
    SPAGAIN;
    POPSTACK;

    if (SvTRUE(ERRSV)) {
        if (logfailure) {
            (void)modperl_errsv(aTHX_ HTTP_INTERNAL_SERVER_ERROR,
                                NULL, NULL);
        }
        return FALSE;
    }
        
    return TRUE;
}

int modperl_require_file(pTHX_ const char *pv, int logfailure)
{
    require_pv(pv);

    if (SvTRUE(ERRSV)) {
        if (logfailure) {
            (void)modperl_errsv(aTHX_ HTTP_INTERNAL_SERVER_ERROR,
                                NULL, NULL);
        }
        return FALSE;
    }

    return TRUE;
}

static SV *modperl_hv_request_find(pTHX_ SV *in, char *classname, CV *cv)
{
    static char *r_keys[] = { "r", "_r", NULL };
    HV *hv = (HV *)SvRV(in);
    SV *sv = Nullsv;
    int i;

    for (i=0; r_keys[i]; i++) {
        int klen = i + 1; /* assumes r_keys[] will never change */
        SV **svp;

        if ((svp = hv_fetch(hv, r_keys[i], klen, FALSE)) && (sv = *svp)) {
            if (SvROK(sv) && (SvTYPE(SvRV(sv)) == SVt_PVHV)) {
                /* dig deeper */
                return modperl_hv_request_find(aTHX_ sv, classname, cv);
            }
            break;
        }
    }

    if (!sv) {
        Perl_croak(aTHX_
                   "method `%s' invoked by a `%s' object with no `r' key!",
                   cv ? GvNAME(CvGV(cv)) : "unknown",
                   HvNAME(SvSTASH(SvRV(in))));
    }

    return SvROK(sv) ? SvRV(sv) : sv;
}

MP_INLINE server_rec *modperl_sv2server_rec(pTHX_ SV *sv)
{
    if (SvOBJECT(sv) || (SvROK(sv) && (SvTYPE(SvRV(sv)) == SVt_PVMG))) {
        return (server_rec *)SvObjIV(sv);
    }
    else {
        return modperl_global_get_server_rec();
    }
}

MP_INLINE request_rec *modperl_sv2request_rec(pTHX_ SV *sv)
{
    return modperl_xs_sv2request_rec(aTHX_ sv, NULL, Nullcv);
}

request_rec *modperl_xs_sv2request_rec(pTHX_ SV *in, char *classname, CV *cv)
{
    SV *sv = Nullsv;
    MAGIC *mg;

    if (SvROK(in)) {
        SV *rv = (SV*)SvRV(in);

        switch (SvTYPE(rv)) {
          case SVt_PVMG:
            sv = rv;
            break;
          case SVt_PVHV:
            sv = modperl_hv_request_find(aTHX_ in, classname, cv);
            break;
          default:
            Perl_croak(aTHX_ "panic: unsupported request_rec type %d",
                       SvTYPE(rv));
        }
    }

    if (!sv) {
        request_rec *r = NULL;
        (void)modperl_tls_get_request_rec(&r);

        if (!r) {
            if (classname && SvPOK(in) && !strEQ(classname, SvPVX(in))) {
                /* might be Apache::{ServerRec,RequestRec}-> dual method */
                return NULL;
            }
            Perl_croak(aTHX_
                       "Apache->%s called without setting Apache->request!",
                       cv ? GvNAME(CvGV(cv)) : "unknown");
        }

        return r;
    }

    if ((mg = mg_find(sv, PERL_MAGIC_ext))) {
        return (request_rec *)mg->mg_ptr;
    }
    else {
        if (classname && !sv_derived_from(in, classname)) {
            /* XXX: find something faster than sv_derived_from */
            return NULL;
        }
        return (request_rec *)SvIV(sv);
    }

    return NULL;
}

MP_INLINE SV *modperl_newSVsv_obj(pTHX_ SV *stashsv, SV *obj)
{
    SV *newobj;

    if (!obj) {
        obj = stashsv;
        stashsv = Nullsv;
    }

    newobj = newSVsv(obj);

    if (stashsv) {
        HV *stash = gv_stashsv(stashsv, TRUE);
        return sv_bless(newobj, stash);
    }

    return newobj;
}

MP_INLINE SV *modperl_ptr2obj(pTHX_ char *classname, void *ptr)
{
    SV *sv = newSV(0);

    MP_TRACE_h(MP_FUNC, "sv_setref_pv(%s, 0x%lx)\n",
               classname, (unsigned long)ptr);
    sv_setref_pv(sv, classname, ptr);

    return sv;
}

int modperl_errsv(pTHX_ int status, request_rec *r, server_rec *s)
{
    SV *sv = ERRSV;
    STRLEN n_a;

    if (SvTRUE(sv)) {
        if (sv_derived_from(sv, "APR::Error") &&
            SvIVx(sv) == MODPERL_RC_EXIT) {
            /* ModPerl::Util::exit was called */
            return OK;
        }
#if 0
        if (modperl_sv_is_http_code(ERRSV, &status)) {
            return status;
        }
#endif
        if (r) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r, "%s", SvPV(sv, n_a));
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, s, "%s", SvPV(sv, n_a));
        }

        return status;
    }

    return status;
}

/* prepends the passed sprintf-like arguments to ERRSV, which also
 * gets stringified on the way */
void modperl_errsv_prepend(pTHX_ const char *pat, ...)
{
    SV *sv;
    va_list args;

    va_start(args, pat);
    sv = vnewSVpvf(pat, &args);
    va_end(args);

    sv_catsv(sv, ERRSV);
    sv_copypv(ERRSV, sv);
    sv_free(sv);
}

#define dl_librefs "DynaLoader::dl_librefs"
#define dl_modules "DynaLoader::dl_modules"

void modperl_xs_dl_handles_clear(pTHX)
{
    AV *librefs = get_av(dl_librefs, FALSE);
    if (librefs) {
        av_clear(librefs);
    }
}

void **modperl_xs_dl_handles_get(pTHX)
{
    I32 i;
    AV *librefs = get_av(dl_librefs, FALSE);
    AV *modules = get_av(dl_modules, FALSE);
    void **handles;

    if (!librefs) {
	MP_TRACE_r(MP_FUNC,
                   "Could not get @%s for unloading.\n",
                   dl_librefs);
	return NULL;
    }

    if (!(AvFILL(librefs) >= 0)) {
        /* dl_librefs and dl_modules are empty */
        return NULL;
    }

    handles = (void **)malloc(sizeof(void *) * (AvFILL(librefs)+2));

    for (i=0; i<=AvFILL(librefs); i++) {
	void *handle;
	SV *handle_sv = *av_fetch(librefs, i, FALSE);
	SV *module_sv = *av_fetch(modules, i, FALSE);

	if(!handle_sv) {
	    MP_TRACE_r(MP_FUNC,
                       "Could not fetch $%s[%d]!\n",
                       dl_librefs, (int)i);
	    continue;
	}
	handle = (void *)SvIV(handle_sv);

	MP_TRACE_r(MP_FUNC, "%s dl handle == 0x%lx\n",
                   SvPVX(module_sv), (unsigned long)handle);
	if (handle) {
	    handles[i] = handle;
	}
    }

    av_clear(modules);
    av_clear(librefs);

    handles[i] = (void *)0;

    return handles;
}

void modperl_xs_dl_handles_close(void **handles)
{
    int i;

    if (!handles) {
	return;
    }

    for (i=0; handles[i]; i++) {
        MP_TRACE_r(MP_FUNC, "close 0x%lx\n", (unsigned long)handles[i]);
        modperl_sys_dlclose(handles[i]);
    }

    free(handles);
}

/* XXX: There is no XS accessible splice() */
static void modperl_av_remove_entry(pTHX_ AV *av, I32 index)
{
    I32 i;
    AV *tmpav = newAV();

    /* stash the entries _before_ the item to delete */
    for (i=0; i<=index; i++) {
        av_store(tmpav, i, SvREFCNT_inc(av_shift(av)));
    }
    
    /* make size at the beginning of the array */
    av_unshift(av, index-1);
    
    /* add stashed entries back */
    for (i=0; i<index; i++) {
        av_store(av, i, *av_fetch(tmpav, i, 0));
    }
    
    SvREFCNT_dec(tmpav);
}

static void modperl_package_unload_dynamic(pTHX_ const char *package, 
                                           I32 dl_index)
{
    AV *librefs = get_av(dl_librefs, 0);
    SV *libref = *av_fetch(librefs, dl_index, 0);

    modperl_sys_dlclose((void *)SvIV(libref));
    
    /* remove package from @dl_librefs and @dl_modules */
    modperl_av_remove_entry(aTHX_ get_av(dl_librefs, 0), dl_index);
    modperl_av_remove_entry(aTHX_ get_av(dl_modules, 0), dl_index);
    
    return;    
}

static int modperl_package_is_dynamic(pTHX_ const char *package,
                                      I32 *dl_index)
{
   I32 i;
   AV *modules = get_av(dl_modules, FALSE);
    
   for (i=0; i<av_len(modules); i++) {
        SV *module = *av_fetch(modules, i, 0);
        if (strEQ(package, SvPVX(module))) {
            *dl_index = i;
            return TRUE;
        }
    }
    return FALSE;
}

modperl_cleanup_data_t *modperl_cleanup_data_new(apr_pool_t *p, void *data)
{
    modperl_cleanup_data_t *cdata =
        (modperl_cleanup_data_t *)apr_pcalloc(p, sizeof(*cdata));
    cdata->pool = p;
    cdata->data = data;
    return cdata;
}

MP_INLINE void modperl_perl_av_push_elts_ref(pTHX_ AV *dst, AV *src)
{
    I32 i, j, src_fill = AvFILLp(src), dst_fill = AvFILLp(dst);

    av_extend(dst, src_fill);
    AvFILLp(dst) += src_fill+1;

    for (i=dst_fill+1, j=0; j<=AvFILLp(src); i++, j++) {
        AvARRAY(dst)[i] = SvREFCNT_inc(AvARRAY(src)[j]);
    }
}

/*
 * similar to hv_fetch_ent, but takes string key and key len rather than SV
 * also skips magic and utf8 fu, since we are only dealing with internal tables
 */
HE *modperl_perl_hv_fetch_he(pTHX_ HV *hv,
                             register char *key,
                             register I32 klen,
                             register U32 hash)
{
    register XPVHV *xhv;
    register HE *entry;

    xhv = (XPVHV *)SvANY(hv);
    if (!xhv->xhv_array) {
        return 0;
    }

#ifdef HvREHASH
    if (HvREHASH(hv)) {
	PERL_HASH_INTERNAL(hash, key, klen);
    }
    else
#endif
    if (!hash) {
	PERL_HASH(hash, key, klen);
    }

    entry = ((HE**)xhv->xhv_array)[hash & (I32)xhv->xhv_max];

    for (; entry; entry = HeNEXT(entry)) {
        if (HeHASH(entry) != hash) {
            continue;
        }
        if (HeKLEN(entry) != klen) {
            continue;
        }
        if (HeKEY(entry) != key && memNE(HeKEY(entry), key, klen)) {
            continue;
        }
        return entry;
    }

    return 0;
}

void modperl_str_toupper(char *str)
{
    while (*str) {
        *str = apr_toupper(*str);
        ++str;
    }
}

/* XXX: same as Perl_do_sprintf(); 
 * but Perl_do_sprintf() is not part of the "public" api
 */
void modperl_perl_do_sprintf(pTHX_ SV *sv, I32 len, SV **sarg)
{
    STRLEN patlen;
    char *pat = SvPV(*sarg, patlen);
    bool do_taint = FALSE;

    sv_vsetpvfn(sv, pat, patlen, Null(va_list*), sarg + 1, len - 1, &do_taint);
    SvSETMAGIC(sv);
    if (do_taint) {
        SvTAINTED_on(sv);
    }
}

void modperl_perl_call_list(pTHX_ AV *subs, const char *name)
{
    I32 i, oldscope = PL_scopestack_ix;
    SV **ary = AvARRAY(subs);

    MP_TRACE_g(MP_FUNC, "pid %lu" MP_TRACEf_TID MP_TRACEf_PERLID
               " running %d %s subs",
               (unsigned long)getpid(), MP_TRACEv_TID_ MP_TRACEv_PERLID_
               AvFILLp(subs)+1, name);
    
    for (i=0; i<=AvFILLp(subs); i++) {
	CV *cv = (CV*)ary[i];
	SV *atsv = ERRSV;

	PUSHMARK(PL_stack_sp);
	call_sv((SV*)cv, G_EVAL|G_DISCARD);

	if (SvCUR(atsv)) {
            Perl_sv_catpvf(aTHX_ atsv, "%s failed--call queue aborted",
                           name);
	    while (PL_scopestack_ix > oldscope) {
		LEAVE;
            }
            Perl_croak(aTHX_ "%s", SvPVX(atsv));
	}
    }
}

void modperl_perl_exit(pTHX_ int status)
{
    ENTER;
    SAVESPTR(PL_diehook);
    PL_diehook = Nullsv; 
    modperl_croak(aTHX_ MODPERL_RC_EXIT, "ModPerl::Util::exit");
}

MP_INLINE SV *modperl_dir_config(pTHX_ request_rec *r, server_rec *s,
                                 char *key, SV *sv_val)
{
    SV *retval = &PL_sv_undef;

    if (r && r->per_dir_config) {				   
        MP_dDCFG;
        retval = modperl_table_get_set(aTHX_ dcfg->configvars,
                                       key, sv_val, FALSE);
    }

    if (!SvOK(retval)) {
        if (s && s->module_config) {
            MP_dSCFG(s);
            SvREFCNT_dec(retval); /* in case above did newSV(0) */
            retval = modperl_table_get_set(aTHX_ scfg->configvars,
                                           key, sv_val, FALSE);
        }
        else {
            retval = &PL_sv_undef;
        }
    }
        
    return retval;
}

SV *modperl_table_get_set(pTHX_ apr_table_t *table, char *key,
                          SV *sv_val, int do_taint)
{
    SV *retval = &PL_sv_undef;

    if (table == NULL) { 
        /* do nothing */
    }
    else if (key == NULL) { 
        retval = modperl_hash_tie(aTHX_ "APR::Table",
                                  Nullsv, (void*)table); 
    }
    else if (!sv_val) { /* no val was passed */
        char *val; 
        if ((val = (char *)apr_table_get(table, key))) { 
            retval = newSVpv(val, 0); 
        } 
        else { 
            retval = newSV(0); 
        } 
        if (do_taint) { 
            SvTAINTED_on(retval); 
        } 
    }
    else if (!SvOK(sv_val)) { /* val was passed in as undef */
        apr_table_unset(table, key); 
    }
    else { 
        apr_table_set(table, key, SvPV_nolen(sv_val));
    } 

    return retval;
}

static char *package2filename(const char *package, int *len)
{
    const char *s;
    char *d;
    char *filename;

    filename = malloc((strlen(package)+4)*sizeof(char));

    for (s = package, d = filename; *s; s++, d++) {
        if (*s == ':' && s[1] == ':') {
            *d = '/';
            s++;
        }
        else {
            *d = *s;
        }
    }
    *d++ = '.';
    *d++ = 'p';
    *d++ = 'm';
    *d   = '\0';

    *len = d - filename;
    return filename;
}

MP_INLINE int modperl_perl_module_loaded(pTHX_ const char *name)
{
    SV **svp;
    int len;
    char *filename = package2filename(name, &len);
    svp = hv_fetch(GvHVn(PL_incgv), filename, len, 0);
    free(filename);

    return (svp && *svp != &PL_sv_undef) ? 1 : 0;
}

#define SLURP_SUCCESS(action) \
    if (rc != APR_SUCCESS) { \
        SvREFCNT_dec(sv); \
        Perl_croak(aTHX_ "Error " action " '%s': %s ", r->filename, \
                   modperl_error_strerror(aTHX_ rc)); \
    }

MP_INLINE SV *modperl_slurp_filename(pTHX_ request_rec *r, int tainted)
{
    SV *sv;
    apr_status_t rc;
    apr_size_t size;
    apr_file_t *file;
    
    size = r->finfo.size;
    sv = newSV(size);

    if (!size) {
        sv_setpvn(sv, "", 0);
        return newRV_noinc(sv);
    }

    /* XXX: could have checked whether r->finfo.filehand is valid and
     * save the apr_file_open call, but apache gives us no API to
     * check whether filehand is valid. we can't test whether it's
     * NULL or not, as it may contain garbagea
     */
    rc = apr_file_open(&file, r->filename, APR_READ|APR_BINARY,
                       APR_OS_DEFAULT, r->pool);
    SLURP_SUCCESS("opening");

    rc = apr_file_read(file, SvPVX(sv), &size);
    SLURP_SUCCESS("reading");

    MP_TRACE_o(MP_FUNC, "read %d bytes from '%s'\n", size, r->filename);
    
    if (r->finfo.size != size) {
        SvREFCNT_dec(sv); 
        Perl_croak(aTHX_ "Error: read %d bytes, expected %d ('%s')",
                   size, r->finfo.size, r->filename);
    }

    rc = apr_file_close(file);
    SLURP_SUCCESS("closing");
    
    SvPVX(sv)[size] = '\0';
    SvCUR_set(sv, size);
    SvPOK_on(sv);

    if (tainted) {
        SvTAINTED_on(sv);
    }
    else {
        SvTAINTED_off(sv);
    }
    
    return newRV_noinc(sv);
}

#define MP_VALID_PKG_CHAR(c) (isalnum(c) ||(c) == '_')
#define MP_VALID_PATH_DELIM(c) ((c) == '/' || (c) =='\\')
char *modperl_file2package(apr_pool_t *p, const char *file)
{
    char *package;
    char *c;
    const char *f;
    int len = strlen(file)+1;

    /* First, skip invalid prefix characters */
    while (!MP_VALID_PKG_CHAR(*file)) {
        file++;
        len--;
    }

    /* Then figure out how big the package name will be like */
    for (f = file; *f; f++) {
        if (MP_VALID_PATH_DELIM(*f)) {
            len++;
        }
    }

    package = apr_pcalloc(p, len);

    /* Then, replace bad characters with '_' */
    for (c = package; *file; c++, file++) {
        if (MP_VALID_PKG_CHAR(*file)) {
            *c = *file;
        }
        else if (MP_VALID_PATH_DELIM(*file)) {

            /* Eliminate subsequent duplicate path delim */
            while (*(file+1) && MP_VALID_PATH_DELIM(*(file+1))) {
                file++;
            }
 
            /* path delim not until end of line */
            if (*(file+1)) {
                *c = *(c+1) = ':';
                c++;
            }
        }
        else {
            *c = '_';
        }
    }
   
    return package;
}

char *modperl_coderef2text(pTHX_ apr_pool_t *p, CV *cv)
{
    dSP;
    int count;
    SV *bdeparse;
    char *text;
    
    /* B::Deparse >= 0.61 needed for blessed code references.
     * 0.6 works fine for non-blessed code refs.
     * notice that B::Deparse is not CPAN-updatable.
     * 0.61 is available starting from 5.8.0
     */
    Perl_load_module(aTHX_ PERL_LOADMOD_NOIMPORT,
                     newSVpvn("B::Deparse", 10),
                     newSVnv(SvOBJECT((SV*)cv) ? 0.61 : 0.60));

    ENTER;
    SAVETMPS;

    /* create the B::Deparse object */
    PUSHMARK(sp);
    XPUSHs(sv_2mortal(newSVpvn("B::Deparse", 10)));
    PUTBACK;
    count = call_method("new", G_SCALAR);
    SPAGAIN;
    if (count != 1) {
        Perl_croak(aTHX_ "Unexpected return value from B::Deparse::new\n");
    }
    if (SvTRUE(ERRSV)) {
        Perl_croak(aTHX_ "error: %s", SvPVX(ERRSV));
    }
    bdeparse = POPs;

    PUSHMARK(sp);
    XPUSHs(bdeparse);
    XPUSHs(sv_2mortal(newRV_inc((SV*)cv)));
    PUTBACK;
    count = call_method("coderef2text", G_SCALAR);
    SPAGAIN;
    if (count != 1) {
        Perl_croak(aTHX_ "Unexpected return value from "
                   "B::Deparse::coderef2text\n");
    }
    if (SvTRUE(ERRSV)) {
        Perl_croak(aTHX_ "error: %s", SvPVX(ERRSV));
    }
    
    {
        STRLEN n_a;
        text = apr_pstrcat(p, "sub ", POPpx, NULL);
    }
    
    PUTBACK;
    
    FREETMPS;
    LEAVE;

    return text;
}

SV *modperl_apr_array_header2avrv(pTHX_ apr_array_header_t *array)
{
    AV *av = newAV(); 

    if (array) {
        int i; 
        for (i = 0; i < array->nelts; i++) {
            av_push(av, newSVpv(((char **)array->elts)[i], 0));
        }
    }
    return newRV_noinc((SV*)av);
}

apr_array_header_t *modperl_avrv2apr_array_header(pTHX_ apr_pool_t *p,
                                                  SV *avrv)
{
    AV *av;
    apr_array_header_t *array;
    int i, av_size;
    
    if (!(SvROK(avrv) && (SvTYPE(SvRV(avrv)) == SVt_PVAV))) {
        Perl_croak(aTHX_ "Not an array reference");
    }
    
    av = (AV*)SvRV(avrv);
    av_size = av_len(av);
    array = apr_array_make(p, av_size+1, sizeof(char *));
    
    for (i = 0; i <= av_size; i++) {
        SV *sv = *av_fetch(av, i, FALSE);
        char **entry = (char **)apr_array_push(array);
        *entry = apr_pstrdup(p, SvPV(sv, PL_na));
    }

    return array;
}

/* Remove a package from %INC */
static void modperl_package_delete_from_inc(pTHX_ const char *package)  
{
    int len;
    char *filename = package2filename(package, &len);
    hv_delete(GvHVn(PL_incgv), filename, len, G_DISCARD);
    free(filename);
}

/* Destroy a package's stash */
static void modperl_package_clear_stash(pTHX_ const char *package)
{
    HV *stash;
    if ((stash = gv_stashpv(package, FALSE))) {
        HE *he;
        I32 len;
        char *key;
        hv_iterinit(stash);
        while ((he = hv_iternext(stash))) {
            key = hv_iterkey(he, &len);
            /* We skip entries ending with ::, they are sub-stashes */
            if (len > 2 && key[len] != ':' && key[len-1] != ':') {
                hv_delete(stash, key, len, G_DISCARD);
            }
        }
    }
}

/* Unload a module as completely and cleanly as possible */
void modperl_package_unload(pTHX_ const char *package)
{
    I32 dl_index;
    
    modperl_package_clear_stash(aTHX_ package);
    modperl_package_delete_from_inc(aTHX_ package);
    
    if (modperl_package_is_dynamic(aTHX_ package, &dl_index)) {
        modperl_package_unload_dynamic(aTHX_ package, dl_index);
    }
    
}
